//------------------------------------------------------------------------------
//
//   Copyright 2018-2019 Fetch.AI Limited
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.
//
//------------------------------------------------------------------------------

#include "dmlf/colearn/muddle_learner_networker_impl.hpp"
#include "dmlf/collective_learning/client_algorithm.hpp"
#include "dmlf/collective_learning/utilities/mnist_client_utilities.hpp"
#include "dmlf/collective_learning/utilities/utilities.hpp"
#include "dmlf/simple_cycling_algorithm.hpp"
#include "http/json_response.hpp"
#include "http/server.hpp"
#include "json/document.hpp"
#include "math/tensor.hpp"
#include "muddle/muddle_status.hpp"
#include "network/management/network_manager.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace fetch::ml::ops;
using namespace fetch::ml::layers;
using namespace fetch::dmlf::collective_learning;

// using DataType         = fetch::fixed_point::FixedPoint<32, 32>;
using DataType         = float;
using TensorType       = fetch::math::Tensor<DataType>;
using VectorTensorType = std::vector<TensorType>;
using SizeType         = fetch::math::SizeType;

/**
 * helper function for stripping instance number from hostname
 */
std::uint64_t InstanceFromHostname(std::string &hostname)
{
  std::size_t current  = 0;
  std::size_t previous = 0;
  std::string instance;
  std::string delim{'-'};

  current = hostname.find(delim);
  while (current != std::string::npos)
  {
    previous = current + 1;
    current  = hostname.find(delim, previous);
  }
  instance = (hostname.substr(previous, current - previous));

  return std::stoul(instance);
}

class MuddleStatusModule : public fetch::http::HTTPModule
{
public:
  MuddleStatusModule()
  {
    Get("/api/status/muddle", "Returns the status of the muddle instances present on the node",
        [](fetch::http::ViewParameters const &, fetch::http::HTTPRequest const &request) {
          auto const &params = request.query();

          std::string network_name{};
          if (params.Has("network"))
          {
            network_name = static_cast<std::string>(params["network"]);
          }

          return fetch::http::CreateJsonResponse(fetch::muddle::GetStatusSummary(network_name));
        });
  }
};

void sleep_forever()
{
  while (true) {
    std::cout << "Sleeping" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(30));
  }
}


int main(int argc, char **argv)
{
  static constexpr std::size_t HOST_NAME_MAX_LEN = 250;

  // This example will create muddle networking distributed client with simple classification neural
  // net and learns how to predict hand written digits from MNIST dataset

  if (argc < 3)
  {
    std::cout << "learner_config.json networker_config.json instance_number" << std::endl;
    return 1;
  }

  // get the host name
  std::uint64_t instance_number;
  if (argc == 4)
  {
    instance_number = static_cast<uint64_t>(std::atoi(argv[3]));
    std::cout << "Getting instance number from input: " << instance_number << std::endl;
  }
  else
  {
    char tmp_hostname[HOST_NAME_MAX_LEN];
    gethostname(tmp_hostname, HOST_NAME_MAX_LEN);
    std::string host_name(tmp_hostname);
    std::cout << "Getting instance number from host_name: " << host_name << std::endl;
    instance_number = InstanceFromHostname(host_name);
  }

  // get the config file
  fetch::json::JSONDocument                                doc;
  fetch::dmlf::collective_learning::ClientParams<DataType> client_params =
      fetch::dmlf::collective_learning::utilities::ClientParamsFromJson<TensorType>(
          std::string(argv[1]), doc);

  auto     data_file      = doc["data"].As<std::string>();
  auto     labels_file    = doc["labels"].As<std::string>();
  auto     n_rounds       = doc["n_rounds"].As<SizeType>();
  auto     n_peers        = doc["n_peers"].As<SizeType>();
  auto     n_clients      = doc["n_clients"].As<SizeType>();
  auto     test_set_ratio = doc["test_set_ratio"].As<float>();
  SizeType start_time     = 0;
  if (!doc["start_time"].IsUndefined())
  {
    start_time = doc["start_time"].As<SizeType>();
  }
  SizeType muddle_delay = 30;
  if (!doc["muddle_delay"].IsUndefined())
  {
    muddle_delay = doc["muddle_delay"].As<SizeType>();
  }
  std::string gcloud_folder = "gs://ml-3000/results/";
  if (!doc["gcloud_folder"].IsUndefined())
  {
    gcloud_folder = doc["gcloud_folder"].As<std::string>();
  }
  uint16_t monitoring_port = 8311;
  if (!doc["monitoring_port"].IsUndefined())
  {
    monitoring_port = doc["monitoring_port"].As<uint16_t>();
  }

  // set up muddle https server
  auto netm = std::make_shared<fetch::network::NetworkManager>("netman", 1);
  netm->Start();
  auto htpptr  = std::make_shared<fetch::http::HTTPServer>(*netm);
  auto mudstat = std::make_shared<MuddleStatusModule>();
  htpptr->AddModule(*mudstat);
  htpptr->Start(monitoring_port);

  // get the network config file
  fetch::json::JSONDocument network_doc;
  std::ifstream             network_config_file{argv[2]};
  std::string               text((std::istreambuf_iterator<char>(network_config_file)),
                                 std::istreambuf_iterator<char>());
  network_doc.Parse(text.c_str());

  size_t config_peer_count = network_doc["peers"].size();
  std::cout << "config_peer_count: " << config_peer_count << std::endl;

  if (instance_number >= n_clients)
  {
    std::cout << "Error: instance number " << instance_number << " greater than number of clients "
              << n_clients << std::endl;
    sleep_forever();
  }
  else if (n_clients > config_peer_count)
  {
    std::cout << "Config only provided for " << config_peer_count << " but " << n_clients
              << " specified in config.json." << std::endl;
    sleep_forever();
  }
  else
  {
    network_doc["peers"].ResizeArray(n_clients);

    /**
     * Prepare environment
     */
    std::cout << "FETCH Distributed MNIST Demo" << std::endl;

    // Create console mutex
    std::shared_ptr<std::mutex> console_mutex_ptr = std::make_shared<std::mutex>();

    // Pause until start time
    std::cout << "start_time: " << start_time << std::endl;
    if (start_time != 0)
    {
      SizeType now = static_cast<SizeType>(std::time(nullptr));
      if (now < start_time)
      {
        SizeType diff = start_time - now;
        std::cout << "Waiting for " << diff << " seconds delay before starting..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(diff));
      }
      else
      {
        std::cout << "Start time is in the past" << std::endl;
      }
    }

    // Create networker and assign shuffle algorithm
    auto networker = std::make_shared<fetch::dmlf::colearn::MuddleLearnerNetworkerImpl>(
        network_doc, instance_number);
    networker->SetShuffleAlgorithm(
        std::make_shared<fetch::dmlf::SimpleCyclingAlgorithm>(networker->GetPeerCount(), n_peers));

    // Pause to let muddle get set up
    std::cout << "Waiting for " << muddle_delay << " seconds to let muddle get set up..."
              << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(muddle_delay));

    // Create learning client
    auto client = fetch::dmlf::collective_learning::utilities::MakeMNISTClient<TensorType>(
        std::to_string(instance_number), client_params, data_file, labels_file, test_set_ratio,
        networker, console_mutex_ptr);

    try
    {
      /**
       * Main loop
       */

      for (SizeType it{0}; it < n_rounds; ++it)
      {
        std::cout << "================= ROUND : " << it << " =================" << std::endl;
        client->RunAlgorithms();
      }
    }
    catch (const std::runtime_error &error)
    {
      std::cout << "Caught error: " << error.what() << std::endl;
    }

    int res = system(("gsutil cp /app/results/* " + gcloud_folder).c_str());
    std::cout << "system() result: " << res << std::endl;

    sleep_forever();
  }

  return 0;
}
