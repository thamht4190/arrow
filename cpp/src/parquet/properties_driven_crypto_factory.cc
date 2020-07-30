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

#include <sstream>

#include "arrow/buffer.h"
#include "arrow/result.h"
#include "arrow/util/logging.h"
#include "arrow/util/string.h"

#include "parquet/encryption_internal.h"
#include "parquet/file_key_material_store.h"
#include "parquet/hadoop_fs_key_material_store.h"
#include "parquet/properties_driven_crypto_factory.h"

using Buffer = arrow::Buffer;
using HadoopFileSystem = arrow::io::HadoopFileSystem;

namespace parquet {

namespace encryption {

constexpr char PropertiesDrivenCryptoFactory::COLUMN_KEYS_PROPERTY_NAME[];

std::vector<std::string> SplitString(const std::string& v, char delim) {
  std::vector<std::string> parts;
  size_t start = 0, end;
  while (true) {
    end = v.find(delim, start);
    parts.push_back(v.substr(start, end - start));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return parts;
}

std::string TrimString(const std::string& v) { return arrow::internal::TrimString(v); }

EncryptionConfiguration::Builder* EncryptionConfiguration::Builder::column_keys(
    const std::string& column_keys) {
  DCHECK(!column_keys.empty());
  if (uniform_encryption_) {
    throw ParquetException(
        "Cannot call both column_keys(const std::string&) and uniform_encryption()");
  }
  column_keys_ = column_keys;
  return this;
}

EncryptionConfiguration::Builder* EncryptionConfiguration::Builder::uniform_encryption() {
  if (!column_keys_.empty()) {
    throw ParquetException(
        "Cannot call both column_keys(const std::string&) and uniform_encryption()");
  }
  uniform_encryption_ = true;
  return this;
}

EncryptionConfiguration::Builder* EncryptionConfiguration::Builder::encryption_algorithm(
    ParquetCipher::type algo) {
  encryption_algorithm_ = algo;
  return this;
}

EncryptionConfiguration::Builder* EncryptionConfiguration::Builder::plaintext_footer(
    bool plaintext_footer) {
  plaintext_footer_ = plaintext_footer;
  return this;
}

EncryptionConfiguration::Builder* EncryptionConfiguration::Builder::double_wrapping(
    bool double_wrapping) {
  double_wrapping_ = double_wrapping;
  return this;
}

EncryptionConfiguration::Builder* EncryptionConfiguration::Builder::wrap_locally(
    bool wrap_locally) {
  wrap_locally_ = wrap_locally;
  return this;
}

EncryptionConfiguration::Builder*
EncryptionConfiguration::Builder::cache_lifetime_seconds(
    uint64_t cache_lifetime_seconds) {
  cache_lifetime_seconds_ = cache_lifetime_seconds;
  return this;
}

EncryptionConfiguration::Builder* EncryptionConfiguration::Builder::internal_key_material(
    bool internal_key_material) {
  internal_key_material_ = internal_key_material;
  return this;
}

EncryptionConfiguration::Builder* EncryptionConfiguration::Builder::data_key_length_bits(
    int32_t data_key_length_bits) {
  data_key_length_bits_ = data_key_length_bits;
  return this;
}

std::shared_ptr<EncryptionConfiguration> EncryptionConfiguration::Builder::build() {
  if (!uniform_encryption_ && column_keys_.empty()) {
    throw ParquetException(
        "Either column_keys(const std::string&) or uniform_encryption() must be "
        "called.");
  }

  return std::make_shared<EncryptionConfiguration>(
      footer_key_, column_keys_, encryption_algorithm_, plaintext_footer_,
      double_wrapping_, wrap_locally_, cache_lifetime_seconds_, internal_key_material_,
      uniform_encryption_, data_key_length_bits_);
}

void PropertiesDrivenCryptoFactory::kms_client_factory(
    std::shared_ptr<KmsClientFactory> kms_client_factory) {
  kms_client_factory_ = kms_client_factory;
}

std::shared_ptr<FileEncryptionProperties>
PropertiesDrivenCryptoFactory::GetFileEncryptionProperties(
    const KmsConnectionConfig& kms_connection_config,
    std::shared_ptr<EncryptionConfiguration> encryption_config,
    const arrow::io::HdfsConnectionConfig& hdfs_connection_config,
    const std::string& temp_file_path) {
  const std::string& footer_key_id = encryption_config->footer_key();
  const std::string& column_key_str = encryption_config->column_keys();

  std::shared_ptr<FileKeyMaterialStore> key_material_store = NULL;
  if (!encryption_config->internal_key_material()) {
    std::shared_ptr<HadoopFileSystem> hadoop_file_system;
    arrow::Status status =
        HadoopFileSystem::Connect(&hdfs_connection_config, &hadoop_file_system);
    if (!status.ok()) {
      throw new ParquetException("Failed to get key material store: " + status.message());
    }
    key_material_store.reset(new HadoopFSKeyMaterialStore(hadoop_file_system));
    key_material_store->Initialize(temp_file_path, false);
  }

  FileKeyWrapper key_wrapper(
      kms_client_factory_, kms_connection_config, key_material_store,
      encryption_config->cache_lifetime_seconds(), encryption_config->double_wrapping(),
      encryption_config->wrap_locally());

  int32_t dek_length_bits = encryption_config->data_key_length_bits();
  int32_t* found_key_length = std::find(
      const_cast<int32_t*>(ACCEPTABLE_DATA_KEY_LENGTHS),
      const_cast<int32_t*>(std::end(ACCEPTABLE_DATA_KEY_LENGTHS)), dek_length_bits);
  if (found_key_length == std::end(ACCEPTABLE_DATA_KEY_LENGTHS)) {
    throw new ParquetException("Wrong data key length : " + dek_length_bits);
  }

  int dek_length = dek_length_bits / 8;

  std::string footer_key_bytes;
  footer_key_bytes.resize(dek_length);
  std::shared_ptr<Buffer> footer_key_buffer = Buffer::FromString(footer_key_bytes);
  RandBytes(footer_key_buffer->mutable_data(), footer_key_buffer->size());
  std::string footer_key_metadata =
      key_wrapper.GetEncryptionKeyMetadata(footer_key_buffer, footer_key_id, true);

  FileEncryptionProperties::Builder properties_builder =
      FileEncryptionProperties::Builder(footer_key_bytes);
  properties_builder.footer_key_metadata(footer_key_metadata);
  properties_builder.algorithm(encryption_config->encryption_algorithm());

  if (!encryption_config->uniform_encryption()) {
    ColumnPathToEncryptionPropertiesMap encrypted_columns =
        GetColumnEncryptionProperties(dek_length, column_key_str, key_wrapper);
    properties_builder.encrypted_columns(encrypted_columns);

    if (encryption_config->plaintext_footer()) {
      properties_builder.set_plaintext_footer();
    }
  }

  if (NULL != key_material_store) {
    key_material_store->SaveMaterial();
  }

  return properties_builder.build();
}

ColumnPathToEncryptionPropertiesMap
PropertiesDrivenCryptoFactory::GetColumnEncryptionProperties(
    int dek_length, const std::string column_keys, FileKeyWrapper& key_wrapper) {
  ColumnPathToEncryptionPropertiesMap encrypted_columns;

  std::vector<std::string> key_to_columns = SplitString(column_keys, ';');
  for (size_t i = 0; i < key_to_columns.size(); ++i) {
    std::string cur_key_to_columns = TrimString(key_to_columns[i]);
    if (cur_key_to_columns.empty()) {
      continue;
    }

    std::vector<std::string> parts = SplitString(cur_key_to_columns, ':');
    if (parts.size() != 2) {
      std::ostringstream message;
      message << "Incorrect key to columns mapping in " << COLUMN_KEYS_PROPERTY_NAME
              << ": [" << cur_key_to_columns << "]";
      throw new ParquetException(message.str());
    }

    std::string column_key_id = TrimString(parts[0]);
    if (column_key_id.empty()) {
      std::ostringstream message;
      message << "Empty key name in " << COLUMN_KEYS_PROPERTY_NAME;
      throw new ParquetException(message.str());
    }

    std::string column_names_str = TrimString(parts[1]);
    std::vector<std::string> column_names = SplitString(column_names_str, ',');
    if (0 == column_names.size()) {
      throw new ParquetException("No columns to encrypt defined for key: " +
                                 column_key_id);
    }

    for (size_t j = 0; j < column_names.size(); ++j) {
      std::string column_name = TrimString(column_names[j]);
      if (column_name.empty()) {
        std::ostringstream message;
        message << "Empty column name in " << COLUMN_KEYS_PROPERTY_NAME
                << " for key: " << column_key_id;
        throw new ParquetException(message.str());
      }

      if (encrypted_columns.find(column_name) != encrypted_columns.end()) {
        throw new ParquetException("Multiple keys defined for the same column: " +
                                   column_name);
      }

      std::string column_key_bytes;
      column_key_bytes.resize(dek_length);
      std::shared_ptr<Buffer> column_key_buffer = Buffer::FromString(column_key_bytes);
      RandBytes(column_key_buffer->mutable_data(), column_key_buffer->size());
      std::string column_key_key_metadata =
          key_wrapper.GetEncryptionKeyMetadata(column_key_buffer, column_key_id, false);

      std::shared_ptr<ColumnEncryptionProperties> cmd =
          ColumnEncryptionProperties::Builder(column_name)
              .key(column_key_bytes)
              ->key_metadata(column_key_key_metadata)
              ->build();
      encrypted_columns.insert({column_name, cmd});
    }
  }
  if (encrypted_columns.empty()) {
    std::ostringstream message;
    message << "No column keys configured in " << COLUMN_KEYS_PROPERTY_NAME;
    throw new ParquetException(message.str());
  }

  return encrypted_columns;
}

}  // namespace encryption

}  // namespace parquet
