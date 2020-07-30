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

#include "parquet/exception.h"
#include "parquet/key_material.h"
#include "parquet/key_metadata.h"

namespace parquet {

namespace encryption {

constexpr char KeyMaterial::KEY_MATERIAL_TYPE_FIELD[];
constexpr char KeyMaterial::KEY_MATERIAL_TYPE1[];

constexpr char KeyMaterial::FOOTER_KEY_ID_IN_FILE[];
constexpr char KeyMaterial::COLUMN_KEY_ID_IN_FILE_PREFIX[];

constexpr char KeyMaterial::IS_FOOTER_KEY_FIELD[];
constexpr char KeyMaterial::DOUBLE_WRAPPING_FIELD[];
constexpr char KeyMaterial::KMS_INSTANCE_ID_FIELD[];
constexpr char KeyMaterial::KMS_INSTANCE_URL_FIELD[];
constexpr char KeyMaterial::MASTER_KEY_ID_FIELD[];
constexpr char KeyMaterial::WRAPPED_DEK_FIELD[];
constexpr char KeyMaterial::KEK_ID_FIELD[];
constexpr char KeyMaterial::WRAPPED_KEK_FIELD[];

KeyMaterial::KeyMaterial(bool is_footer_key, const std::string& kms_instance_id,
                         const std::string& kms_instance_url,
                         const std::string& master_key_id, bool is_double_wrapped,
                         const std::string& kek_id,
                         const std::string& encoded_wrapped_kek,
                         const std::string& encoded_wrapped_dek)
    : is_footer_key_(is_footer_key),
      kms_instance_id_(kms_instance_id),
      kms_instance_url_(kms_instance_url),
      master_key_id_(master_key_id),
      is_double_wrapped_(is_double_wrapped),
      kek_id_(kek_id),
      encoded_wrapped_kek_(encoded_wrapped_kek),
      encoded_wrapped_dek_(encoded_wrapped_dek) {}

KeyMaterial KeyMaterial::Parse(const std::string& key_material_string) {
  rapidjson::Document document;
  document.Parse(key_material_string.c_str());

  if (document.HasParseError() || !document.IsObject()) {
    throw new ParquetException("Failed to parse key metadata " + key_material_string);
  }

  // External key material - extract "key material type", and make sure it is supported
  std::string key_material_type = document[KEY_MATERIAL_TYPE_FIELD].GetString();
  if (KEY_MATERIAL_TYPE1 != key_material_type) {
    throw new ParquetException("Wrong key material type: " + key_material_type + " vs " +
                               KEY_MATERIAL_TYPE1);
  }
  // Parse other fields (common to internal and external key material)
  return Parse(document);
}

KeyMaterial KeyMaterial::Parse(const rapidjson::Document& key_material_json) {
  // 2. Check if "key material" belongs to file footer key
  bool is_footer_key = key_material_json[IS_FOOTER_KEY_FIELD].GetBool();
  std::string kms_instance_id;
  std::string kms_instance_url;
  if (is_footer_key) {
    // 3.  For footer key, extract KMS Instance ID
    kms_instance_id = key_material_json[KMS_INSTANCE_ID_FIELD].GetString();
    // 4.  For footer key, extract KMS Instance URL
    kms_instance_url = key_material_json[KMS_INSTANCE_URL_FIELD].GetString();
  }
  // 5. Extract master key ID
  std::string master_key_id = key_material_json[MASTER_KEY_ID_FIELD].GetString();
  // 6. Extract wrapped DEK
  std::string encoded_wrapped_dek = key_material_json[WRAPPED_DEK_FIELD].GetString();
  std::string kek_id;
  std::string encoded_wrapped_kek;
  // 7. Check if "key material" was generated in double wrapping mode
  bool is_double_wrapped = key_material_json[DOUBLE_WRAPPING_FIELD].GetBool();
  if (is_double_wrapped) {
    // 8. In double wrapping mode, extract KEK ID
    kek_id = key_material_json[KEK_ID_FIELD].GetString();
    // 9. In double wrapping mode, extract wrapped KEK
    encoded_wrapped_kek = key_material_json[WRAPPED_KEK_FIELD].GetString();
  }

  return KeyMaterial(is_footer_key, kms_instance_id, kms_instance_url, master_key_id,
                     is_double_wrapped, kek_id, encoded_wrapped_kek, encoded_wrapped_dek);
}

std::string KeyMaterial::CreateSerialized(
    bool is_footer_key, const std::string& kms_instance_id,
    const std::string& kms_instance_url, const std::string& master_key_id,
    bool is_double_wrapped, const std::string& kek_id,
    const std::string& encoded_wrapped_kek, const std::string& encoded_wrapped_dek,
    bool is_internal_storage) {
  rapidjson::Document d;
  auto& allocator = d.GetAllocator();
  rapidjson::Value key_material_map(rapidjson::kObjectType);
  rapidjson::Value str_value(rapidjson::kStringType);

  key_material_map.AddMember(KEY_MATERIAL_TYPE_FIELD, KEY_MATERIAL_TYPE1, allocator);

  if (is_internal_storage) {
    // for internal storage, key material and key metadata are the same.
    // adding the "internalStorage" field that belongs to KeyMetadata.
    key_material_map.AddMember(KeyMetadata::KEY_MATERIAL_INTERNAL_STORAGE_FIELD, true,
                               allocator);
  }
  // 2. Write isFooterKey
  key_material_map.AddMember(IS_FOOTER_KEY_FIELD, is_footer_key, allocator);
  if (is_footer_key) {
    // 3. For footer key, write KMS Instance ID
    str_value.SetString(kms_instance_id.c_str(), allocator);
    key_material_map.AddMember(KMS_INSTANCE_ID_FIELD, str_value, allocator);
    // 4. For footer key, write KMS Instance URL
    str_value.SetString(kms_instance_url.c_str(), allocator);
    key_material_map.AddMember(KMS_INSTANCE_URL_FIELD, str_value, allocator);
  }
  // 5. Write master key ID
  str_value.SetString(master_key_id.c_str(), allocator);
  key_material_map.AddMember(MASTER_KEY_ID_FIELD, str_value, allocator);
  // 6. Write wrapped DEK
  str_value.SetString(encoded_wrapped_dek.c_str(), allocator);
  key_material_map.AddMember(WRAPPED_DEK_FIELD, str_value, allocator);
  // 7. Write isDoubleWrapped
  key_material_map.AddMember(DOUBLE_WRAPPING_FIELD, is_double_wrapped, allocator);
  if (is_double_wrapped) {
    // 8. In double wrapping mode, write KEK ID
    str_value.SetString(kek_id.c_str(), allocator);
    key_material_map.AddMember(KEK_ID_FIELD, str_value, allocator);
    // 9. In double wrapping mode, write wrapped KEK
    str_value.SetString(encoded_wrapped_kek.c_str(), allocator);
    key_material_map.AddMember(WRAPPED_KEK_FIELD, str_value, allocator);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  key_material_map.Accept(writer);

  return buffer.GetString();
}

}  // namespace encryption

}  // namespace parquet
