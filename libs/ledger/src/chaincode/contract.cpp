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

#include "core/byte_array/decoders.hpp"
#include "core/json/document.hpp"
#include "core/json/exceptions.hpp"
#include "ledger/chain/transaction.hpp"
#include "ledger/chaincode/contract.hpp"

namespace fetch {
namespace ledger {

/**
 * Dispatch the initialisation handler
 *
 * @param tx The reference to the originating transaction
 * @return The corresponding status result for the operation
 */
Contract::Result Contract::DispatchInitialise(Address const &owner)
{
  Result status{Status::OK};

  if (init_handler_)
  {
    status = init_handler_(owner);
  }

  return status;
}

/**
 * Dispatch the specified contract query
 *
 * @param name The name of the query
 * @param query The input query parameters
 * @param response The output query parameters
 * @return The corresponding status result for the operation
 */
Contract::Status Contract::DispatchQuery(ContractName const &name, Query const &query,
                                         Query &response)
{
  auto status{Status::NOT_FOUND};

  auto it = query_handlers_.find(name);
  if (it != query_handlers_.end())
  {
    status = it->second(query, response);
    ++query_counters_[name];
  }

  return status;
}

/**
 * Dispatch the specified a contract action
 *
 * @param name The name of the action
 * @param tx The input transaction
 * @return The corresponding status result for the operation
 */
Contract::Result Contract::DispatchTransaction(byte_array::ConstByteArray const &name,
                                               Transaction const &tx, BlockIndex block_index)
{
  Result status{Status::NOT_FOUND};

  auto it = transaction_handlers_.find(name);
  if (it != transaction_handlers_.end())
  {
    // dispatch the contract
    status = it->second(tx, block_index);
    ++transaction_counters_[name];
  }

  return status;
}

/**
 * Register the init. handler
 *
 * @param handler The init handler
 */
void Contract::OnInitialise(InitialiseHandler &&handler)
{
  // detect if a handler has already been set
  if (init_handler_)
  {
    throw std::runtime_error("Duplicate initialise handler");
  }

  // register the handler
  init_handler_ = std::move(handler);
}

/**
 * Register a transaction handler
 *
 * @param name The name of the transaction
 * @param handler The transaction handler to be registered
 */
void Contract::OnTransaction(std::string const &name, TransactionHandler &&handler)
{
  // detect duplicates
  if (transaction_handlers_.find(name) != transaction_handlers_.end())
  {
    throw std::logic_error("Duplicate transaction handler registered");
  }

  // register the handler
  transaction_handlers_[name] = std::move(handler);

  // reset the counters
  transaction_counters_[name] = 0;
}

/**
 * Register a query handler
 *
 * @param name The name of the query
 * @param handler The query handler to be registered
 */
void Contract::OnQuery(std::string const &name, QueryHandler &&handler)
{
  // detect duplicates
  if (query_handlers_.find(name) != query_handlers_.end())
  {
    throw std::logic_error("Duplicate query handler registered");
  }

  // register the handler
  query_handlers_[name] = std::move(handler);

  // reset the counters
  query_counters_[name] = 0;
}

/**
 * Utility: Parse the contents of the transaction payload as a JSON object
 *
 * @param tx The input transaction to be processed
 * @param output THe output JSON object to be populated
 * @return true if successful, otherwise falses
 */
bool Contract::ParseAsJson(Transaction const &tx, variant::Variant &output)
{
  bool success{false};

  json::JSONDocument document;

  try
  {
    // parse the data of the transaction
    document.Parse(tx.data());
    success = true;
  }
  catch (json::JSONParseException const &)
  {
  }

  if (success)
  {
    output = document.root();
  }

  return success;
}

/**
 * State Accessor
 *
 * @return The reference to the state instance
 */
ledger::StateAdapter &Contract::state()
{
  detailed_assert(state_ != nullptr);
  return *state_;
}

/**
 * Attach the state interface to the contract instance
 *
 * @param state The reference
 */
void Contract::Attach(ledger::StateAdapter &state)
{
  state_ = &state;
}

/**
 * Detach the state interface from the contract instance
 */
void Contract::Detach()
{
  state_ = nullptr;
}

/**
 * Query Handler Map Accessor
 *
 * @return The query handler map
 */
Contract::QueryHandlerMap const &Contract::query_handlers() const
{
  return query_handlers_;
}

/**
 * Transaction Handler Map Accessor
 *
 * @return The transaction handler map
 */
Contract::TransactionHandlerMap const &Contract::transaction_handlers() const
{
  return transaction_handlers_;
}

}  // namespace ledger
}  // namespace fetch
