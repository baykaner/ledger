#include"network/thread_manager.hpp"
#include"./network_test_service.hpp"

int main(int argc, char **argv)
{
  // Default to 10 threads, this can/will be changed by HTTP interface
  fetch::network::ThreadManager *tm = new fetch::network::ThreadManager(10);

  {
    int seed = 0;
    if (argc > 1)
    {
      std::stringstream s(argv[1]);
      s >> seed;
    }

    uint16_t tcpPort  = 9080+seed;
    uint16_t httpPort = 8080+seed;

    fetch::network_test::NetworkTestService serv(tm, tcpPort, httpPort, seed);
    tm->Start();
    //serv.Start(); // the python will do this

    std::cout << "press any key to quit" << std::endl;
    std::cin >> seed;
  }

  // TODO: (`HUT`) : investigate: tcp or http server doens't like destructing before tm stopped
  tm->Stop();
  //delete tm;
  return 0;
}
