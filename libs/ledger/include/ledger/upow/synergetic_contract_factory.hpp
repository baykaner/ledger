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

#include "ledger/upow/synergetic_contract.hpp"

namespace fetch {
namespace ledger {

class StorageInterface;
class Address;

class SynergeticContractFactory
{
public:
  // Construction / Destruction
  explicit SynergeticContractFactory(StorageInterface &storage);
  SynergeticContractFactory(SynergeticContractFactory const &) = delete;
  SynergeticContractFactory(SynergeticContractFactory &&)      = delete;
  ~SynergeticContractFactory()                                 = default;

  SynergeticContractPtr Create(Digest const &digest);

  // Operators
  SynergeticContractFactory &operator=(SynergeticContractFactory const &) = delete;
  SynergeticContractFactory &operator=(SynergeticContractFactory &&) = delete;

private:
  StorageInterface &storage_;
};

}  // namespace ledger
}  // namespace fetch
