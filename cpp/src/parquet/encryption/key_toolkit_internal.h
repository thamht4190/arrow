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

#pragma once

#include <string>

namespace parquet {
namespace encryption {
namespace internal {

// "data encryption key" and "master key identifier" are paired together as output when
// parsing from "key material"
class KeyWithMasterId {
 public:
  KeyWithMasterId(const std::string& key_bytes, const std::string& master_id)
      : key_bytes_(key_bytes), master_id_(master_id) {}

  const std::string& data_key() const { return key_bytes_; }
  const std::string& master_id() const { return master_id_; }

 private:
  const std::string key_bytes_;
  const std::string master_id_;
};

/// Encrypts "key" with "master_key", using AES-GCM and the "aad"
std::string EncryptKeyLocally(const std::string& key, const std::string& master_key,
                              const std::string& aad);

/// Decrypts encrypted key with "master_key", using AES-GCM and the "aad"
std::string DecryptKeyLocally(const std::string& encoded_encrypted_key,
                              const std::string& master_key, const std::string& aad);

bool ValidateKeyLength(int32_t key_length_bits);

}  // namespace internal
}  // namespace encryption
}  // namespace parquet