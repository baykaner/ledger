#include <iostream>
#include <unistd.h>
#include <string>
#include <time.h>



#include "network/swarm/swarm_agent_naive.hpp"
#include "network/swarm/swarm_agent_api.hpp"
#include "network/swarm/swarm_http_interface.hpp"
#include "network/swarm/swarm_node.hpp"
#include "network/swarm/swarm_peer_location.hpp"
#include "network/swarm/swarm_random.hpp"
#include "python/fetch_pybind.hpp"
#include "python/network/swarm/py_swarm_agent_api.hpp"
#include "python/ledger/chain/py_main_chain.hpp"
#include <pybind11/embed.h>
#include <pybind11/iostream.h>

void say(pybind11::args args)
{
  std::cout << "PYTHON:";
  for (size_t i = 0; i < args.size(); ++i) {
    std::cout << pybind11::str(args[i]);
  }
  std::cout << std::endl;
}

PYBIND11_EMBEDDED_MODULE(fetchnetwork, module) {
  pybind11::module ns_fetch_network_swarm = module.def_submodule("swarm");
  fetch::swarm::BuildSwarmAgentApi(ns_fetch_network_swarm);
  //pybind11::add_ostream_redirect(ns_fetch_network_swarm, "ostream_redirect");
  ns_fetch_network_swarm.def("say", &say, "A function prints");}


PYBIND11_EMBEDDED_MODULE(fetchledger, module) {
  pybind11::module ns_fetch_ledger_chain_mainchain = module.def_submodule("chain");
  fetch::chain::BuildMainChain(ns_fetch_ledger_chain_mainchain);
}


class PythonContext
{
public:
  typedef std::shared_ptr<fetch::swarm::PySwarm> SWARM_P;
  typedef std::unique_ptr<pybind11::scoped_interpreter> INTERP_P;
  typedef pybind11::dict LOCALS;
  typedef std::shared_ptr<LOCALS> LOCALS_P;

  LOCALS_P locals;
  SWARM_P pySwarm;
  INTERP_P interpreter;

  PythonContext()
  {
  }

  virtual ~PythonContext()
  {
    if (pySwarm)
      {
        pySwarm -> Stop();
      }
    locals.reset();
    interpreter.reset();
  }

    /* See -- 
https://github.com/pybind/pybind11/issues/1296
https://github.com/cython/cython/issues/1877

    */
  void runFile(const string &fn, int argc, char *argv[])
  {
    interpreter = INTERP_P(new pybind11::scoped_interpreter());
    locals = std::make_shared<pybind11::dict>();

    pybind11::print("PYCHAIN? STARTING FILE RUN");

    std::list<std::string> args;
    for(int i=1;i<argc;i++)
      {
        args.push_back(std::string(argv[i]));
      }

    pybind11::list argsList;
    for(auto &s : args)
      {
        argsList.append(s);
      }

    pybind11::module::import("sys").attr("argv") = argsList;

    pybind11::eval_file(fn, pybind11::globals());
    pybind11::gil_scoped_release release;
  }
};

int main(int argc, char *argv[])
{
  PythonContext context;

  if (argc > 1)
    {
      context.runFile(argv[1], argc, argv);
    }
  else
    {
      std::cerr << "Please supply filenames to run" << std::endl;
      exit(1);
    }
}
