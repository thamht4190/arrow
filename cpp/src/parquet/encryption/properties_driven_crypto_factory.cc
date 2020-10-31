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

#include "arrow/result.h"
#include "arrow/util/logging.h"
#include "arrow/util/string.h"
#include "arrow/util/string_view.h"

#include "parquet/encryption/encryption_internal.h"
#include "parquet/encryption/file_key_material_store.h"
#include "parquet/encryption/file_key_unwrapper.h"
#include "parquet/encryption/key_toolkit_internal.h"
#include "parquet/encryption/properties_driven_crypto_factory.h"

namespace parquet {
namespace encryption {

void PropertiesDrivenCryptoFactory::RegisterKmsClientFactory(
    std::shared_ptr<KmsClientFactory> kms_client_factory) {
  key_toolkit_.RegisterKmsClientFactory(kms_client_factory);
}

std::shared_ptr<FileEncryptionProperties>
PropertiesDrivenCryptoFactory::GetFileEncryptionProperties(
    const KmsConnectionConfig& kms_connection_config,
    std::shared_ptr<EncryptionConfiguration> encryption_config) {
  if (encryption_config == NULL) {
    return NULL;
  }
  if (!encryption_config->uniform_encryption && encryption_config->column_keys.empty()) {
    throw ParquetException("Either column_keys or uniform_encryption must be set");
  } else if (encryption_config->uniform_encryption &&
             !encryption_config->column_keys.empty()) {
    throw ParquetException("Cannot set both column_keys and uniform_encryption");
  }
  const std::string& footer_key_id = encryption_config->footer_key;
  const std::string& column_key_str = encryption_config->column_keys;

  std::shared_ptr<FileKeyMaterialStore> key_material_store = NULL;
  if (!encryption_config->internal_key_material) {
    // TODO: using external key material store with Hadoop file system
    throw ParquetException("External key material store is not supported yet.");
  }

  FileKeyWrapper key_wrapper(&key_toolkit_, kms_connection_config, key_material_store,
                             encryption_config->cache_lifetime_seconds,
                             encryption_config->double_wrapping);

  int32_t dek_length_bits = encryption_config->data_key_length_bits;
  if (!internal::ValidateKeyLength(dek_length_bits)) {
    std::ostringstream ss;
    ss << "Wrong data key length : " << dek_length_bits;
    throw ParquetException(ss.str());
  }

  int dek_length = dek_length_bits / 8;

  std::string footer_key(dek_length, '\0');
  RandBytes(reinterpret_cast<uint8_t*>(&footer_key[0]),
            static_cast<int>(footer_key.size()));

  std::string footer_key_metadata =
      key_wrapper.GetEncryptionKeyMetadata(footer_key, footer_key_id, true);

  FileEncryptionProperties::Builder properties_builder =
      FileEncryptionProperties::Builder(footer_key);
  properties_builder.footer_key_metadata(footer_key_metadata);
  properties_builder.algorithm(encryption_config->encryption_algorithm);

  if (!encryption_config->uniform_encryption) {
    ColumnPathToEncryptionPropertiesMap encrypted_columns =
        GetColumnEncryptionProperties(dek_length, column_key_str, key_wrapper);
    properties_builder.encrypted_columns(encrypted_columns);

    if (encryption_config->plaintext_footer) {
      properties_builder.set_plaintext_footer();
    }
  }

  return properties_builder.build();
}

ColumnPathToEncryptionPropertiesMap
PropertiesDrivenCryptoFactory::GetColumnEncryptionProperties(
    int dek_length, const std::string column_keys, FileKeyWrapper& key_wrapper) {
  ColumnPathToEncryptionPropertiesMap encrypted_columns;

  std::vector<arrow::util::string_view> key_to_columns =
      arrow::internal::SplitString(column_keys, ';');
  for (size_t i = 0; i < key_to_columns.size(); ++i) {
    std::string cur_key_to_columns =
        arrow::internal::TrimString(std::string(key_to_columns[i]));
    if (cur_key_to_columns.empty()) {
      continue;
    }

    std::vector<arrow::util::string_view> parts =
        arrow::internal::SplitString(cur_key_to_columns, ':');
    if (parts.size() != 2) {
      std::ostringstream message;
      message << "Incorrect key to columns mapping in column keys property"
              << ": [" << cur_key_to_columns << "]";
      throw ParquetException(message.str());
    }

    std::string column_key_id = arrow::internal::TrimString(std::string(parts[0]));
    if (column_key_id.empty()) {
      throw ParquetException("Empty key name in column keys property.");
    }

    std::string column_names_str = arrow::internal::TrimString(std::string(parts[1]));
    std::vector<arrow::util::string_view> column_names =
        arrow::internal::SplitString(column_names_str, ',');
    if (0 == column_names.size()) {
      throw ParquetException("No columns to encrypt defined for key: " + column_key_id);
    }

    for (size_t j = 0; j < column_names.size(); ++j) {
      std::string column_name = arrow::internal::TrimString(std::string(column_names[j]));
      if (column_name.empty()) {
        std::ostringstream message;
        message << "Empty column name in column keys property for key: " << column_key_id;
        throw ParquetException(message.str());
      }

      if (encrypted_columns.find(column_name) != encrypted_columns.end()) {
        throw ParquetException("Multiple keys defined for the same column: " +
                               column_name);
      }

      std::string column_key(dek_length, '\0');
      RandBytes(reinterpret_cast<uint8_t*>(&column_key[0]),
                static_cast<int>(column_key.size()));
      std::string column_key_key_metadata =
          key_wrapper.GetEncryptionKeyMetadata(column_key, column_key_id, false);

      std::shared_ptr<ColumnEncryptionProperties> cmd =
          ColumnEncryptionProperties::Builder(column_name)
              .key(column_key)
              ->key_metadata(column_key_key_metadata)
              ->build();
      encrypted_columns.insert({column_name, cmd});
    }
  }
  if (encrypted_columns.empty()) {
    throw ParquetException("No column keys configured in column keys property.");
  }

  return encrypted_columns;
}

std::shared_ptr<FileDecryptionProperties>
PropertiesDrivenCryptoFactory::GetFileDecryptionProperties(
    const KmsConnectionConfig& kms_connection_config,
    std::shared_ptr<DecryptionConfiguration> decryption_config) {
  std::shared_ptr<DecryptionKeyRetriever> key_retriever(new FileKeyUnwrapper(
      &key_toolkit_, kms_connection_config, decryption_config->cache_lifetime_seconds));

  return FileDecryptionProperties::Builder()
      .key_retriever(key_retriever)
      ->plaintext_files_allowed()
      ->build();
}

}  // namespace encryption
}  // namespace parquet
