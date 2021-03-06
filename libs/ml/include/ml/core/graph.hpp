#pragma once
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

#include "ml/core/node.hpp"
#include "ml/meta/ml_type_traits.hpp"
#include "ml/ops/weights.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// TODO(1604) - rework AddTrainable and GetTrainables so that graph stores trainables recursively,
// but optimiser gets a flat vector of ptrs
// TODO(1605) - harmonise InsertSharedCopy with AddTrainable
// TODO(#1554) - we should only reset the cache for trained nodes, not all nodes
// TODO(1467) - implement validity checks on graph compilation - e.g. loss function should not
// appear in middle of graph

namespace fetch {
namespace ml {

namespace optimisers {
template <typename TensorType>
class Optimiser;
}  // namespace optimisers

namespace model {
template <typename TensorType>
class ModelInterface;
}  // namespace model

namespace distributed_learning {
template <typename TensorType>
class TrainingClient;
}  // namespace distributed_learning

enum class GraphState : uint8_t
{
  INVALID,       // graph described through adding nodes is not valid for compilation
  NOT_COMPILED,  // occurs whenever adding new nodes to graph
  COMPILED,      // added nodes have been link and trainables compiled
  EVALUATED,     // forward pass has been completed - ready for backprop
  BACKWARD,      // backward pass has been completed - ready for apply gradients
  UPDATED        // gradients have been applied
};

/**
 * The full graph on which to run the computation
 */
template <class T>
class Graph
{
public:
  using TensorType       = T;
  using ArrayPtrType     = std::shared_ptr<TensorType>;
  using SizeType         = typename TensorType::SizeType;
  using DataType         = typename TensorType::Type;
  using NodePtrType      = typename std::shared_ptr<fetch::ml::Node<TensorType>>;
  using TrainablePtrType = typename std::shared_ptr<fetch::ml::ops::Trainable<TensorType>>;
  using PlaceholderType  = typename fetch::ml::ops::PlaceHolder<TensorType>;
  using RegPtrType       = std::shared_ptr<fetch::ml::regularisers::Regulariser<T>>;
  using SPType           = GraphSaveableParams<TensorType>;
  using OpPtrType        = std::shared_ptr<fetch::ml::ops::Ops<TensorType>>;

  virtual ~Graph() = default;
  Graph()          = default;

  /////////////////////////////
  /// graph setup functions ///
  /////////////////////////////

  template <class OperationType, typename... Params>
  std::string AddNode(std::string const &node_name, std::vector<std::string> const &inputs,
                      Params... params);

  void ResetCompile();
  void Compile();
  void AddTrainable(NodePtrType node_ptr, std::string const &node_name);

  void SetRegularisation(RegPtrType regulariser, DataType regularisation_rate = DataType{0.0});
  bool SetRegularisation(std::string node_name, RegPtrType regulariser,
                         DataType regularisation_rate = DataType{0.0});

  ////////////////////////////////
  /// graph training functions ///
  ////////////////////////////////

  void       SetInput(std::string const &node_name, TensorType data);
  TensorType Evaluate(std::string const &node_name, bool is_training = true);
  void       BackPropagate(std::string const &node_name, TensorType const &error_signal = {});
  void       ApplyRegularisation();
  void       ApplyGradients(std::vector<TensorType> &grad);

  /////////////////////////////////////
  /// graph serialisation functions ///
  /////////////////////////////////////

  bool InsertNode(std::string const &node_name, NodePtrType node_ptr);

  NodePtrType GetNode(std::string const &node_name) const;

  virtual struct fetch::ml::StateDict<TensorType> StateDict();
  virtual void LoadStateDict(struct fetch::ml::StateDict<T> const &dict);

  std::vector<TensorType> get_weights() const;
  void                    SetWeights(std::vector<TensorType> &new_weights);
  std::vector<TensorType> GetGradientsReferences() const;
  std::vector<TensorType> GetGradients() const;

  std::vector<TrainablePtrType> GetTrainables();

  void                            ResetGradients();
  GraphSaveableParams<TensorType> GetGraphSaveableParams();
  void                            SetGraphSaveableParams(GraphSaveableParams<TensorType> const &sp);

  void AddGradients(std::vector<TensorType> grads);

  static constexpr char const *DESCRIPTOR = "Graph";

protected:
  std::unordered_map<std::string, NodePtrType>                  nodes_;
  std::vector<std::pair<std::string, std::vector<std::string>>> connections_;
  std::unordered_map<std::string, SizeType>                     trainable_lookup_;
  std::vector<NodePtrType>                                      trainable_nodes_;

  void       InsertSharedCopy(std::shared_ptr<Graph<TensorType>> output_ptr);
  TensorType ForwardPropagate(std::string const &node_name, bool is_training = true);

private:
  GraphState graph_state_ = GraphState::NOT_COMPILED;

  friend class optimisers::Optimiser<TensorType>;
  friend class model::ModelInterface<TensorType>;
  friend class distributed_learning::TrainingClient<TensorType>;

  TensorType ForwardImplementation(std::string const &node_name, bool is_training,
                                   bool evaluate_mode);

  template <typename OperationType>
  bool UpdateVariableName(std::string const &name, std::string &ret);

  void LinkNodesInGraph(std::string const &node_name, std::vector<std::string> const &inputs);

  template <class OperationType, typename... Params>
  meta::IfIsShareable<TensorType, OperationType, NodePtrType> MakeDuplicateNode(
      std::string const &node_name, std::string &updated_name);

  template <class OperationType, typename... Params>
  meta::IfIsNotShareable<TensorType, OperationType, NodePtrType> MakeDuplicateNode(
      std::string const &node_name, std::string &updated_name);

  void ResetGraphCache(bool input_size_changed, std::shared_ptr<Node<T>> n = {});
};

//////////////////////
/// PUBLIC METHODS ///
//////////////////////

template <typename TensorType>
template <class OperationType, typename... Params>
std::string Graph<TensorType>::AddNode(std::string const &             node_name,
                                       std::vector<std::string> const &inputs, Params... params)
{
  graph_state_ = GraphState::NOT_COMPILED;

  // guarantee unique op name
  std::string updated_name;
  bool        is_duplicate = UpdateVariableName<OperationType>(node_name, updated_name);

  NodePtrType node_ptr;
  if (!is_duplicate)
  {
    // Instantiate the node based on params
    node_ptr = std::make_shared<Node<TensorType>>(
        OperationType::OpCode(), updated_name,
        [params...]() { return std::make_shared<OperationType>(params...); });
  }
  else
  {
    node_ptr = MakeDuplicateNode<OperationType, Params...>(node_name, updated_name);
  }

  // put node in look up table
  nodes_[updated_name] = node_ptr;

  // define connections between nodes
  connections_.emplace_back(std::make_pair(updated_name, inputs));

  return updated_name;
}

/**
 * Undoes the work of a previous Compile call.
 * Since compilation could be called multiple times during graph construction, this is
 * necessary to avoid duplication connections/trainables
 * @tparam TensorType
 */
template <typename TensorType>
void Graph<TensorType>::ResetCompile()
{
  graph_state_ = GraphState::NOT_COMPILED;

  // clear trainables from any previous compilation
  trainable_lookup_.clear();
  trainable_nodes_.clear();

  for (auto &connection : connections_)
  {
    auto node_name   = connection.first;
    auto node_inputs = connection.second;

    // remove inputs and output from the node
    nodes_.at(node_name)->ResetInputsAndOutputs();
  }
}

/**
 * Links Node inputs and sets up the trainables object ready for use by an optimiser
 * @tparam TensorType
 */
template <typename TensorType>
void Graph<TensorType>::Compile()
{
  switch (graph_state_)
  {
  case GraphState::COMPILED:
  case GraphState::EVALUATED:
  case GraphState::BACKWARD:
  case GraphState::UPDATED:
  {
    // graph already compiled. do nothing
    break;
  }
  case GraphState::INVALID:
  case GraphState::NOT_COMPILED:
  {
    bool valid = true;

    ResetCompile();

    // set inputs and outputs to nodes and set trainables
    for (auto &connection : connections_)
    {
      auto node_name   = connection.first;
      auto node_inputs = connection.second;
      LinkNodesInGraph(node_name, node_inputs);

      auto node_ptr = nodes_.at(node_name);

      AddTrainable(node_ptr, node_name);
    }

    // TODO(1467) - implement validity checks on graph compilation - e.g. loss function should not
    // appear in middle of graph
    if (valid)
    {
      graph_state_ = GraphState::COMPILED;
    }
    else
    {
      graph_state_ = GraphState::INVALID;
    }
    break;
  }
  default:
  {
    throw std::runtime_error("cannot evaluate graph - unrecognised graph state");
  }
  }
}

/**
 * Appends op to map of trainable nodes. Called by AddNode
 * If this op is a layer/subgraph/graph then appends all trainable ops from op.trainable_
 * @tparam TensorType
 * @param node_ptr
 * @param node_name
 */
template <typename TensorType>
void Graph<TensorType>::AddTrainable(NodePtrType node_ptr, std::string const &node_name)
{
  auto op_ptr        = node_ptr->GetOp();
  auto trainable_ptr = std::dynamic_pointer_cast<fetch::ml::ops::Trainable<TensorType>>(op_ptr);
  auto graph_ptr     = std::dynamic_pointer_cast<Graph<TensorType>>(op_ptr);

  // if its a trainable
  if (trainable_ptr)
  {
    trainable_nodes_.emplace_back(node_ptr);
    trainable_lookup_[node_name] = trainable_nodes_.size() - 1;
  }
  // if its a graph
  else if (graph_ptr)
  {
    for (auto &trainable : graph_ptr->trainable_lookup_)
    {
      std::string subnode_name(node_name + "_" + trainable.first);

      // only add new trainable
      // graph re-compilation can lead to a valid call to add the same trainables twice, this should
      // be ignored
      if (graph_ptr->trainable_lookup_.find(subnode_name) == graph_ptr->trainable_lookup_.end())
      {
        trainable_nodes_.emplace_back(graph_ptr->trainable_nodes_.at(trainable.second));
        trainable_lookup_[subnode_name] = trainable_nodes_.size() - 1;
      }
    }
  }
}

/**
 * Evaluates the output of a node (calling all necessary forward prop)
 * @param node_name name of node to evaluate for output
 * @return a copy of the output tensor
 */
template <typename TensorType>
TensorType Graph<TensorType>::Evaluate(std::string const &node_name, bool is_training)
{
  return ForwardImplementation(node_name, is_training, true);
}

/**
 * Evaluates the output of a node via a shallow copy. This is used by the optimiser
 * and isn't safe for external users.
 * @param node_name name of node to evaluate for output
 * @return a copy of the output tensor
 */
template <typename TensorType>
TensorType Graph<TensorType>::ForwardPropagate(std::string const &node_name, bool is_training)
{
  return ForwardImplementation(node_name, is_training, false);
}

/**
 * computes the forward pass. either invoked by an external and returns a deep copy of the result,
 * or invoked by a friend of graph and returns a shallow copy of the result tensor
 * @tparam TensorType
 * @param node_name
 * @param is_training
 * @param evaluate_mode if true, returns a deep copy of the result tensor
 * @return
 */
template <typename TensorType>
TensorType Graph<TensorType>::ForwardImplementation(std::string const &node_name, bool is_training,
                                                    bool evaluate_mode)
{
  Compile();

  if (nodes_.find(node_name) != nodes_.end())
  {
    switch (graph_state_)
    {
    case GraphState::INVALID:
    case GraphState::NOT_COMPILED:
    {
      throw std::runtime_error("cannot compile and evaluate graph");
    }
    case GraphState::COMPILED:
    case GraphState::EVALUATED:
    case GraphState::BACKWARD:
    case GraphState::UPDATED:
    {
      graph_state_ = GraphState::EVALUATED;
      auto ret     = (*(nodes_[node_name]->Evaluate(is_training)));
      if (evaluate_mode)
      {
        return ret.Copy();
      }
      return ret;
    }
    default:
    {
      throw std::runtime_error("cannot evaluate graph - unrecognised graph state");
    }
    }
  }
  else
  {
    throw std::runtime_error("Cannot evaluate: node [" + node_name + "] not in graph");
  }
}

/**
 * Backpropagate given error signal through the graph
 * If no error signal is given, an empty error signal is used
 * (which is valid when backpropagating from a loss function op
 * @param node_name name of node from which to begin backprop
 * @param error_signal pointer to array containing error signal to backprop
 */
template <typename TensorType>
void Graph<TensorType>::BackPropagate(std::string const &node_name, TensorType const &error_signal)
{
  Compile();

  // check node to backprop from exists in graph
  if (nodes_.find(node_name) != nodes_.end())
  {
    switch (graph_state_)
    {
    case GraphState::INVALID:
    case GraphState::NOT_COMPILED:
    {
      throw std::runtime_error("Cannot backpropagate: graph not compiled or invalid");
    }
    case GraphState::COMPILED:
    {
      throw std::runtime_error("Cannot backpropagate: forward pass not completed on graph");
    }
    case GraphState::EVALUATED:
    case GraphState::BACKWARD:
    case GraphState::UPDATED:
    {
      nodes_[node_name]->BackPropagate(error_signal);
      graph_state_ = GraphState::BACKWARD;
      break;
    }
    default:
    {
      throw std::runtime_error("cannot backpropagate: unrecognised graph state");
    }
    }
  }
  else
  {
    throw std::runtime_error("Cannot backpropagate: node [" + node_name + "] not in graph");
  }
}

/////////////////////////
/// PROTECTED METHODS ///
/////////////////////////

///////////////////////
/// PRIVATE METHODS ///
///////////////////////

/**
 * Set regularisation type and rate for all trainables in graph
 * @tparam TensorType
 * @param regularisation_type L1, L2 or NONE
 * @param regularisation_rate
 */
template <typename TensorType>
void Graph<TensorType>::SetRegularisation(RegPtrType regulariser, DataType regularisation_rate)
{
  Compile();
  for (auto &t : trainable_nodes_)
  {
    auto tmp = std::dynamic_pointer_cast<ops::Trainable<TensorType>>(t->GetOp());
    tmp->SetRegularisation(regulariser, regularisation_rate);
  }
}

/**
 * Set regularisation type and rate for specified trainable by it's name
 * @tparam TensorType
 * @param node_name name of specific trainable
 * @param regularisation_type L1, L2 or NONE
 * @param regularisation_rate
 */
template <typename TensorType>
bool Graph<TensorType>::SetRegularisation(std::string node_name, RegPtrType regulariser,
                                          DataType regularisation_rate)
{
  Compile();
  NodePtrType t             = trainable_nodes_.at(trainable_lookup_.at(node_name));
  auto        trainable_ptr = std::dynamic_pointer_cast<ops::Trainable<TensorType>>(t->GetOp());
  trainable_ptr->SetRegularisation(regulariser, regularisation_rate);

  return true;
}

/**
 * Add gradient values to weight for each trainable
 * @param grad vector of gradient values for each trainable stored in TensorType
 */
template <typename TensorType>
void Graph<TensorType>::ApplyGradients(std::vector<TensorType> &grad)
{
  Compile();

  switch (graph_state_)
  {
  case GraphState::INVALID:
  case GraphState::NOT_COMPILED:
  case GraphState::COMPILED:
  case GraphState::EVALUATED:
  {
    throw std::runtime_error(
        "cannot apply gradients: backpropagate not previously called on graph");
  }
  case GraphState::BACKWARD:
  {
    auto grad_it = grad.begin();
    for (auto const &t : trainable_nodes_)
    {
      auto trainable_ptr = std::dynamic_pointer_cast<ops::Trainable<TensorType>>(t->GetOp());
      trainable_ptr->ApplyGradient(*grad_it);
      ++grad_it;
    }

    for (auto const &t : nodes_)
    {
      // TODO(#1554) - we should only reset the cache for trained nodes, not all nodes
      ResetGraphCache(false, t.second);
    }
    return;
  }
  case GraphState::UPDATED:
  {
    // no gradients to apply - nothing to do
    return;
  }
  default:
  {
    throw std::runtime_error("cannot apply gradients: unrecognised graph state");
  }
  }
}

template <typename TensorType>
void Graph<TensorType>::ApplyRegularisation()
{
  Compile();

  for (auto &t : trainable_nodes_)
  {
    auto trainable_ptr = std::dynamic_pointer_cast<ops::Trainable<TensorType>>(t->GetOp());
    trainable_ptr->ApplyRegularisation();
  }

  // TODO(#1554) - we should only reset the cache for trained nodes, not all nodes
  ResetGraphCache(false);
}

/**
 * Method for directly inserting nodes to graph - used for serialisation
 * @tparam T
 * @param node_name
 * @return
 */
template <typename T>
bool Graph<T>::InsertNode(std::string const &node_name, NodePtrType node_ptr)
{
  // put node in look up table
  nodes_[node_name] = node_ptr;
  return nodes_.find(node_name) != nodes_.end();
}

/**
 * @brief Method for constructing a graph saveable params object for serializing a Graph
 * This method constructs and returns a graph saveable params object containing all
 * necessary data for serialising and deserialising a Graph object. In order to do this
 * it must first construct a vector of connections to store, and then repeatedly call
 * the Node save params construction method
 * @tparam TensorType
 * @return GraphSaveableParams object fully defining a graph
 */
template <typename TensorType>
GraphSaveableParams<TensorType> Graph<TensorType>::GetGraphSaveableParams()
{
  GraphSaveableParams<TensorType> gs;
  gs.connections = connections_;
  for (auto const &npair : nodes_)
  {
    std::string node_name = npair.first;
    auto        node      = npair.second;

    auto nsp = node->GetNodeSaveableParams();
    gs.nodes.insert(std::make_pair(node_name, nsp));
  }

  gs.graph_state = static_cast<uint8_t>(graph_state_);
  return gs;
}

template <typename TensorType>
void Graph<TensorType>::SetGraphSaveableParams(GraphSaveableParams<TensorType> const &sp)
{
  assert(nodes_.size() == sp.connections.size());

  connections_ = sp.connections;

  // assign inputs and outputs to the nodes
  for (auto &node : sp.connections)
  {
    LinkNodesInGraph(node.first, node.second);
  }

  graph_state_ = static_cast<GraphState>(sp.graph_state);

  switch (graph_state_)
  {
  case GraphState::INVALID:
  case GraphState::NOT_COMPILED:
  case GraphState::COMPILED:
  {
    // valid graph states, nothing to do
    return;
  }
  case GraphState::EVALUATED:
  case GraphState::BACKWARD:
  case GraphState::UPDATED:
  {
    // we revert state back to compile to prevent immediate backpropagation after deserialisation
    graph_state_ = GraphState::COMPILED;
    return;
  }
  default:
  {
    throw std::runtime_error("cannot setGraphSaveableParams: graph state not recognised");
  }
  }
}

template <typename TensorType>
typename Graph<TensorType>::NodePtrType Graph<TensorType>::GetNode(
    std::string const &node_name) const
{
  NodePtrType ret = nodes_.at(node_name);
  if (!ret)
  {
    throw std::runtime_error("couldn't find node [" + node_name + "] in graph!");
  }
  return ret;
}

/**
 * Assigns data to a dataholder if the node can be found in the graph.
 * Also resets the graph cache to avoid erroneous leftover outputs
 * @param node_name name of the placeholder node in the graph (must be unique)
 * @param data the pointer to a tensor to assign to the placeholder
 */
template <typename TensorType>
void Graph<TensorType>::SetInput(std::string const &node_name, TensorType data)
{
  auto dataholder =
      std::dynamic_pointer_cast<ops::DataHolder<TensorType>>(nodes_.at(node_name)->GetOp());

  if (dataholder)
  {
    bool input_size_changed = dataholder->SetData(data);
    ResetGraphCache(input_size_changed, nodes_[node_name]);
  }
  else
  {
    throw std::runtime_error("No placeholder node with name [" + node_name + "] found in graph!");
  }
}

/**
 * Resets graph cache, clearing stored evaluation outputs
 * and recursively updating the input size for all downstream nodes
 * (or for all nodes if none specified)
 */
template <typename TensorType>
void Graph<TensorType>::ResetGraphCache(bool input_size_changed, NodePtrType n)
{
  if (!n)
  {
    for (auto &node : nodes_)
    {
      node.second->ResetCache(input_size_changed);

      auto graph_pointer = std::dynamic_pointer_cast<Graph<TensorType>>(node.second->GetOp());
      if (graph_pointer)
      {
        graph_pointer->ResetGraphCache(input_size_changed);
      }
    }
  }
  else
  {
    n->ResetCache(input_size_changed);
    for (auto &node : n->GetOutputs())
    {
      ResetGraphCache(input_size_changed, node);
    }
  }
}

/**
 * Assigns all trainable parameters to a stateDict for exporting and serialising
 * @return  d is the StateDict of all trainable params
 */
template <typename TensorType>
struct fetch::ml::StateDict<TensorType> Graph<TensorType>::StateDict()
{
  Compile();
  struct fetch::ml::StateDict<TensorType> d;
  for (auto const &t : trainable_lookup_)
  {
    auto node_ptr    = trainable_nodes_.at(t.second);
    auto op_ptr      = node_ptr->GetOp();
    auto weights_ptr = std::dynamic_pointer_cast<ops::Weights<TensorType>>(op_ptr);
    d.dict_.emplace(t.first, weights_ptr->StateDict());
  }
  return d;
}

/**
 * Import trainable parameters from an exported model
 * @param dict  state dictionary to import to weights
 */
template <typename TensorType>
void Graph<TensorType>::LoadStateDict(struct fetch::ml::StateDict<TensorType> const &dict)
{
  assert(!dict.weights_);
  for (auto const &t : trainable_lookup_)
  {
    auto trainable_ptr = std::dynamic_pointer_cast<ops::Trainable<TensorType>>(
        trainable_nodes_.at(t.second)->GetOp());
    trainable_ptr->LoadStateDict(dict.dict_.at(t.first));
  }
}

/**
 * Assigns all trainable weights parameters to vector of TensorType for exporting and serialising
 * @return ret is vector containing values for all weights
 */
template <typename TensorType>
std::vector<TensorType> Graph<TensorType>::get_weights() const
{
  std::vector<TensorType> ret;

  for (auto const &t : trainable_nodes_)
  {
    auto trainable_ptr = std::dynamic_pointer_cast<ops::Trainable<TensorType>>(t->GetOp());
    ret.emplace_back(trainable_ptr->get_weights());
  }
  return ret;
}

/**
 * Write weights from vector to trainables - used for importing model
 * @tparam TensorType
 * @param new_weights
 */
template <typename TensorType>
void Graph<TensorType>::SetWeights(std::vector<TensorType> &new_weights)
{
  auto trainable_it = trainable_nodes_.begin();
  auto weights_it   = new_weights.begin();
  {
    auto trainable_ptr =
        std::dynamic_pointer_cast<ops::Trainable<TensorType>>((*trainable_it)->GetOp());
    trainable_ptr->SetWeights(*weights_it);

    ++trainable_it;
    ++weights_it;
  }
}

/**
 * Assigns all trainable accumulated gradient parameters to vector of TensorType for exporting and
 * serialising
 * @return ret is vector containing all gradient values
 */
template <typename TensorType>
std::vector<TensorType> Graph<TensorType>::GetGradientsReferences() const
{
  std::vector<TensorType> ret;

  for (auto const &t : trainable_nodes_)
  {
    auto trainable_ptr = std::dynamic_pointer_cast<ops::Trainable<TensorType>>(t->GetOp());
    ret.emplace_back(trainable_ptr->GetGradientsReferences());
  }
  return std::move(ret);
}

/**
 * Assigns all trainable accumulated gradient parameters to vector of TensorType for exporting and
 * serialising
 * @return ret is vector containing all gradient values
 */
template <typename TensorType>
std::vector<TensorType> Graph<TensorType>::GetGradients() const
{
  std::vector<TensorType> ret;

  for (auto const &t : trainable_nodes_)
  {
    auto trainable_ptr = std::dynamic_pointer_cast<ops::Trainable<TensorType>>(t->GetOp());
    ret.emplace_back(trainable_ptr->GetGradients());
  }
  return ret;
}

/**
 * Sets all accumulated gradients for each trainable to zero
 */
template <typename TensorType>
void Graph<TensorType>::ResetGradients()
{
  for (auto const &t : trainable_nodes_)
  {
    auto trainable_ptr = std::dynamic_pointer_cast<ops::Trainable<TensorType>>(t->GetOp());
    trainable_ptr->ResetGradients();
  }
}

/**
 * Adds a vector of Tensors to the gradient accumulation of all the trainable pointers in the graph.
 * Typical use case is injecting external gradients e.g when doing distributed learning.
 * @tparam T
 * @param grads The vector of gradients - needs to have the same length as the number of trainables
 */
template <typename T>
void Graph<T>::AddGradients(std::vector<TensorType> grads)
{
  assert(grads.size() == trainable_nodes_.size());
  auto gt_it = trainable_nodes_.begin();
  for (auto const &grad : grads)
  {
    auto weights_ptr = std::dynamic_pointer_cast<ops::Weights<TensorType>>((*gt_it)->GetOp());
    weights_ptr->AddToGradient(grad);
    ++gt_it;
  }
}

/**
 * Connect the new node to the current graph by setting input and output nodes to it and saving it
 * in the lookup table. Can also be used by ResetCompile to unlink previously linked nodes
 * @tparam TensorType
 * @param node_name
 * @param inputs
 * @param unlink
 */
template <typename TensorType>
void Graph<TensorType>::LinkNodesInGraph(std::string const &             node_name,
                                         std::vector<std::string> const &inputs)
{
  // assign inputs and outputs
  for (auto const &i : inputs)
  {
    nodes_.at(node_name)->AddInput(nodes_.at(i));
    nodes_[i]->AddOutput(nodes_[node_name]);
  }
}

/**
 * generates a new variable name if necessary to ensure uniqueness within graph
 * @param pre_string
 * @return
 */
template <typename TensorType>
template <typename OperationType>
bool Graph<TensorType>::UpdateVariableName(std::string const &name, std::string &ret)
{
  ret                       = name;
  std::string op_descriptor = (OperationType::DESCRIPTOR);
  bool        is_duplicate  = false;
  // search graph for existing variable names
  if (ret.empty())
  {  // if no name is specified, generate a default name
    std::uint64_t name_idx = 0;
    ret                    = op_descriptor + "_" + std::to_string(name_idx);
    while (!(nodes_.find(ret) == nodes_.end()))
    {
      ++name_idx;
      ret = op_descriptor + "_" + std::to_string(name_idx);
    }
  }
  else if (nodes_.find(ret) != nodes_.end())
  {  // if a duplicated name is specified, shared weight is assumed
    is_duplicate           = true;
    std::uint64_t name_idx = 1;
    ret                    = name + "_Copy_" + std::to_string(name_idx);
    while (!(nodes_.find(ret) == nodes_.end()))
    {
      ++name_idx;
      ret = name + "_Copy_" + std::to_string(name_idx);
    }
  }

  return is_duplicate;
}

/**
 * Assigns all trainable pointers to vector for optimiser purpose
 * @return ret is vector containing pointers to all trainables
 */
template <typename TensorType>
std::vector<typename Graph<TensorType>::TrainablePtrType> Graph<TensorType>::GetTrainables()
{
  std::vector<TrainablePtrType> ret;
  for (auto &t : trainable_nodes_)
  {
    auto trainable_ptr = std::dynamic_pointer_cast<ops::Trainable<TensorType>>(t->GetOp());
    ret.emplace_back(trainable_ptr);
  }
  return ret;
}

/**
 * Inserts a copy of the graph (with shared op ptrs where appropriate) into output_ptr
 * @tparam T
 * @param output_ptr shared_ptr to the new graph. Needs to not be the same as the old graph!
 */
template <class T>
void Graph<T>::InsertSharedCopy(std::shared_ptr<Graph<TensorType>> output_ptr)
{
  if (output_ptr.get() == this)
  {
    throw std::runtime_error("This needs to be called with a separate ptr.");
  }

  std::shared_ptr<Graph<TensorType>> copyshare = output_ptr;

  // copy all the nodes, sharing the weights using MakeSharedCopy
  for (auto const &n : nodes_)
  {
    std::string                       node_name = n.first;
    std::shared_ptr<Node<TensorType>> n_ptr     = n.second;
    OpPtrType                         op_ptr    = n_ptr->GetOp();

    OpPtrType op_copyshare = op_ptr->MakeSharedCopy(op_ptr);

    assert(copyshare->nodes_.find(node_name) == copyshare->nodes_.end());

    copyshare->nodes_[node_name] =
        std::make_shared<Node<TensorType>>(*n_ptr, node_name, op_copyshare);

    // add node_ptr to trainable lookup etc. if required
    auto trainable_ptr =
        std::dynamic_pointer_cast<fetch::ml::ops::Trainable<TensorType>>(op_copyshare);
    auto graph_ptr = std::dynamic_pointer_cast<Graph<TensorType>>(op_copyshare);
    if (trainable_ptr)
    {
      copyshare->trainable_nodes_.emplace_back(n_ptr);
      copyshare->trainable_lookup_[node_name] = copyshare->trainable_nodes_.size() - 1;
    }
    else if (graph_ptr)
    {
      for (auto &trainable : graph_ptr->trainable_lookup_)
      {
        std::string subnode_name(node_name + "_" + trainable.first);
        assert(copyshare->trainable_lookup_.find(subnode_name) ==
               copyshare->trainable_lookup_.end());
        copyshare->trainable_nodes_.emplace_back(graph_ptr->trainable_nodes_.at(trainable.second));
        copyshare->trainable_lookup_[subnode_name] = copyshare->trainable_nodes_.size() - 1;
      }
    }
  }

  for (auto const &n : this->nodes_)
  {
    std::string                       node_name = n.first;
    std::shared_ptr<Node<TensorType>> n_ptr     = n.second;

    copyshare->LinkNodesInGraph(node_name, this->nodes_[node_name]->GetInputNames());
  }
}

template <typename TensorType>
template <class OperationType, typename... Params>
meta::IfIsShareable<TensorType, OperationType, typename Graph<TensorType>::NodePtrType>
Graph<TensorType>::MakeDuplicateNode(std::string const &node_name, std::string &updated_name)
{
  // if name is duplicated then shared node is required
  NodePtrType target_node = GetNode(node_name);

  // get a copy (shared when appropriate) of the target node Op
  auto op_copyshare = target_node->GetOp()->MakeSharedCopy(target_node->GetOp());

  // make a new node by giving it the copied op
  return std::make_shared<Node<TensorType>>(OperationType::OpCode(), updated_name, op_copyshare);
}

template <typename TensorType>
template <class OperationType, typename... Params>
meta::IfIsNotShareable<TensorType, OperationType, typename Graph<TensorType>::NodePtrType>
Graph<TensorType>::MakeDuplicateNode(std::string const &node_name, std::string & /* updated_name */)
{
  throw std::runtime_error("OperationType is not shareable. Cannot make duplicate of node named: " +
                           node_name);
}

}  // namespace ml
}  // namespace fetch
