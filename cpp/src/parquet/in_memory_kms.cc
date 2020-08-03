// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/util/base64.h"

#include "parquet/exception.h"
#include "parquet/in_memory_kms.h"
#include "parquet/key_toolkit.h"
#include "parquet/kms_client_factory.h"
#include "parquet/remote_kms_client.h"
#include "parquet/string_util.h"

using parquet::encryption::KeyToolkit;
using parquet::encryption::KmsClient;
using parquet::encryption::KmsClientFactory;
using parquet::encryption::KmsConnectionConfig;
using parquet::encryption::RemoteKmsClient;

namespace parquet {

namespace test {

using parquet::encryption::RemoteKmsClient;

std::map<std::string, std::string> InMemoryKms::master_key_map_;
std::map<std::string, std::string> InMemoryKms::new_master_key_map_;

void InMemoryKms::StartKeyRotation(const std::vector<std::string>& new_master_keys) {
  new_master_key_map_ = ParseKeyList(new_master_keys);
}

void InMemoryKms::FinishKeyRotation() { master_key_map_ = new_master_key_map_; }

void InMemoryKms::InitializeMasterKey(const std::vector<std::string>& master_keys) {
  master_key_map_ = ParseKeyList(master_keys);
  new_master_key_map_ = master_key_map_;
}

void InMemoryKms::InitializeInternal() {}

std::string InMemoryKms::WrapKeyInServer(std::shared_ptr<arrow::Buffer> key_bytes,
                                         const std::string& master_key_identifier) {
  // Always use the latest key version for writing
  if (new_master_key_map_.find(master_key_identifier) == new_master_key_map_.end()) {
    throw new ParquetException("Key not found: " + master_key_identifier);
  }
  const std::string& master_key = new_master_key_map_.at(master_key_identifier);

  std::shared_ptr<arrow::Buffer> aad = arrow::Buffer::FromString(master_key_identifier);
  return KeyToolkit::EncryptKeyLocally(key_bytes->data(), key_bytes->size(),
                                       reinterpret_cast<const uint8_t*>(&master_key[0]),
                                       master_key.size(), aad->data(), aad->size());
}

std::vector<uint8_t> InMemoryKms::UnwrapKeyInServer(
    const std::string& wrapped_key, const std::string& master_key_identifier) {
  if (new_master_key_map_.find(master_key_identifier) == new_master_key_map_.end()) {
    throw new ParquetException("Key not found: " + master_key_identifier);
  }
  const std::string& master_key = new_master_key_map_.at(master_key_identifier);

  std::shared_ptr<arrow::Buffer> aad = arrow::Buffer::FromString(master_key_identifier);
  return KeyToolkit::DecryptKeyLocally(
      wrapped_key, reinterpret_cast<const uint8_t*>(master_key.c_str()),
      master_key.size(), aad->data(), aad->size());
}

std::vector<uint8_t> InMemoryKms::GetMasterKeyFromServer(
    const std::string& master_key_identifier) {
  // Always return the latest key version
  const std::string master_key_str = new_master_key_map_.at(master_key_identifier);
  std::vector<uint8_t> master_key(master_key_str.size());
  memcpy(master_key.data(), master_key_str.c_str(), master_key_str.size());

  return master_key;
}

std::map<std::string, std::string> InMemoryKms::ParseKeyList(
    const std::vector<std::string>& master_keys) {
  std::map<std::string, std::string> key_map;

  int n_keys = master_keys.size();
  for (int i = 0; i < n_keys; i++) {
    std::vector<std::string> parts = SplitString(master_keys[i], ':');
    std::string key_name = TrimString(parts[0]);
    if (parts.size() != 2) {
      throw new std::invalid_argument("Key '" + key_name +
                                      "' is not formatted correctly");
    }
    std::string key = TrimString(parts[1]);

    std::string key_bytes = arrow::util::base64_decode(key);
    key_map.insert({key_name, key_bytes});
  }
  return key_map;
}

}  // namespace test

}  // namespace parquet
