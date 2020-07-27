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

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "parquet/key_metadata.h"

namespace parquet {

namespace encryption {

KeyMetadata::KeyMetadata(bool is_internal_storage, const std::string& key_reference,
                         const KeyMaterial& key_material)
    : is_internal_storage_(is_internal_storage),
      key_reference_(key_reference),
      key_material_(key_material) {}

// For external material only. For internal material, create serialized KeyMaterial
// directly
std::string KeyMetadata::CreateSerializedForExternalMaterial(
    const std::string& key_reference) {
  rapidjson::Document d;
  auto& allocator = d.GetAllocator();
  rapidjson::Value key_metadata_map(rapidjson::kObjectType);

  key_metadata_map.AddMember(KeyMaterial::KEY_MATERIAL_TYPE_FIELD,
                             KeyMaterial::KEY_MATERIAL_TYPE1, allocator);
  key_metadata_map.AddMember(KEY_MATERIAL_INTERNAL_STORAGE_FIELD, false, allocator);

  rapidjson::Value value(rapidjson::kStringType);
  value.SetString(key_reference.c_str(), allocator);
  key_metadata_map.AddMember(KEY_REFERENCE_FIELD, value, allocator);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  key_metadata_map.Accept(writer);

  return buffer.GetString();
}

}  // namespace encryption

}  // namespace parquet
