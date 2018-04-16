#ifndef NODE_DIRECTORY_HPP
#define NODE_DIRECTORY_HPP

// This file holds and manages connections to other nodes

#include"chain/transaction.hpp"
#include"protocols/fetch_protocols.hpp"
#include"protocols/network_test/commands.hpp"
#include"./network_classes.hpp"
#include"service/client.hpp"
#include<set>

namespace fetch
{
namespace network_test
{

class NodeDirectory
{
public:

  NodeDirectory(network::ThreadManager *tm) :
  tm_{tm}
  {}

  NodeDirectory(NodeDirectory &rhs)            = delete;
  NodeDirectory(NodeDirectory &&rhs)           = delete;
  NodeDirectory operator=(NodeDirectory& rhs)  = delete;
  NodeDirectory operator=(NodeDirectory&& rhs) = delete;

  ~NodeDirectory()
  {
    for(auto &i : serviceClients_)
    {
      delete i.second;
    }
  }

  // Only call this during node setup
  void addEndpoint(const Endpoint &endpoint)
  {
    if (serviceClients_.find(endpoint) == serviceClients_.end())
    {
      auto client = new service::ServiceClient<network::TCPClient> {endpoint.IP(), endpoint.TCPPort(), tm_};
      serviceClients_[endpoint] = client;
    }
  }

  template <typename T>
  void BroadcastTransaction(T&& trans)
  {
    CallAllEndpoints(protocols::NetworkTest::SEND_TRANSACTION, std::forward<T>(trans));
  }

  template<typename T, typename... Args>
  void CallAllEndpoints(T CallEnum, Args... args)
  {
    for(auto &i : serviceClients_)
    {
      auto client = i.second;
      client->Call(protocols::FetchProtocols::NETWORK_TEST, CallEnum, args...);
    }
  }

private:
  fetch::network::ThreadManager                                    *tm_;
  std::map<Endpoint, service::ServiceClient<network::TCPClient> *> serviceClients_;
};

}
}
#endif
