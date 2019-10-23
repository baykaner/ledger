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

#include "dmlf/distributed_learning/distributed_learning_client.hpp"
#include "dmlf/networkers/muddle_learner_networker.hpp"
#include "dmlf/simple_cycling_algorithm.hpp"
#include "math/matrix_operations.hpp"
#include "math/tensor.hpp"
#include "ml/dataloaders/mnist_loaders/mnist_loader.hpp"
#include "ml/ops/loss_functions/cross_entropy_loss.hpp"
#include "ml/optimisation/adam_optimiser.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace fetch::ml::ops;
using namespace fetch::ml::layers;
using namespace fetch::ml::distributed_learning;

using DataType         = fetch::fixed_point::FixedPoint<32, 32>;
using TensorType       = fetch::math::Tensor<DataType>;
using VectorTensorType = std::vector<TensorType>;
using SizeType         = fetch::math::SizeType;

std::shared_ptr<TrainingClient<TensorType>> MakeClient(
    std::string const &id, ClientParams<DataType> &client_params, std::string const &images,
    std::string const &labels, float test_set_ratio, std::shared_ptr<std::mutex> console_mutex_ptr)
{
  // Initialise model
  std::shared_ptr<fetch::ml::Graph<TensorType>> g_ptr =
      std::make_shared<fetch::ml::Graph<TensorType>>();

  client_params.inputs_names = {g_ptr->template AddNode<PlaceHolder<TensorType>>("Input", {})};
  g_ptr->template AddNode<FullyConnected<TensorType>>("FC1", {"Input"}, 28u * 28u, 10u);
  g_ptr->template AddNode<Relu<TensorType>>("Relu1", {"FC1"});
  g_ptr->template AddNode<FullyConnected<TensorType>>("FC2", {"Relu1"}, 10u, 10u);
  g_ptr->template AddNode<Relu<TensorType>>("Relu2", {"FC2"});
  g_ptr->template AddNode<FullyConnected<TensorType>>("FC3", {"Relu2"}, 10u, 10u);
  g_ptr->template AddNode<Softmax<TensorType>>("Softmax", {"FC3"});
  client_params.label_name = g_ptr->template AddNode<PlaceHolder<TensorType>>("Label", {});
  client_params.error_name =
      g_ptr->template AddNode<CrossEntropyLoss<TensorType>>("Error", {"Softmax", "Label"});
  g_ptr->Compile();

  // Initialise DataLoader
  std::shared_ptr<fetch::ml::dataloaders::MNISTLoader<TensorType, TensorType>> dataloader_ptr =
      std::make_shared<fetch::ml::dataloaders::MNISTLoader<TensorType, TensorType>>(images, labels);
  dataloader_ptr->SetTestRatio(test_set_ratio);
  dataloader_ptr->SetRandomMode(true);
  // Initialise Optimiser
  std::shared_ptr<fetch::ml::optimisers::Optimiser<TensorType>> optimiser_ptr =
      std::make_shared<fetch::ml::optimisers::AdamOptimiser<TensorType>>(
          std::shared_ptr<fetch::ml::Graph<TensorType>>(g_ptr), client_params.inputs_names,
          client_params.label_name, client_params.error_name, client_params.learning_rate);

  return std::make_shared<TrainingClient<TensorType>>(id, g_ptr, dataloader_ptr, optimiser_ptr,
                                                      client_params, console_mutex_ptr);
}

int main(int ac, char **av)
{
  if (ac < 3)
  {
    std::cout << "Usage : " << av[0]
              << " PATH/TO/train-images-idx3-ubyte PATH/TO/train-labels-idx1-ubyte "
                 "networker_config instance_number"
              << std::endl;
    return 1;
  }

  ClientParams<DataType> client_params;

  // Command line parameters
  std::string images_filename = av[1];
  std::string labels_filename = av[2];
  std::string config          = std::string(av[3]);
  int         instance_number = std::atoi(av[4]);

  SizeType number_of_rounds                     = 10;
  client_params.max_updates                     = 100;  // Round ends after this number of batches
  SizeType number_of_peers                      = 3;
  client_params.batch_size                      = 32;
  client_params.learning_rate                   = static_cast<DataType>(.001f);
  float                       test_set_ratio    = 0.03f;
  std::shared_ptr<std::mutex> console_mutex_ptr = std::make_shared<std::mutex>();

  std::cout << "FETCH Distributed MNIST Demo" << std::endl;

  auto client    = MakeClient(std::to_string(instance_number), client_params, images_filename,
                           labels_filename, test_set_ratio, console_mutex_ptr);
  auto networker = std::make_shared<fetch::dmlf::MuddleLearnerNetworker>(config, instance_number);
  networker->Initialize<fetch::dmlf::Update<TensorType>>();

  networker->SetShuffleAlgorithm(std::make_shared<fetch::dmlf::SimpleCyclingAlgorithm>(
      networker->GetPeerCount(), number_of_peers));

  client->SetNetworker(networker);

  // Main loop
  for (SizeType it{0}; it < number_of_rounds; ++it)
  {
    // Start all clients
    std::cout << "================= ROUND : " << it << " =================" << std::endl;

    client->Run();
  }

  return 0;
}
