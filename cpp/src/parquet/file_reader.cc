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

#include "parquet/file_reader.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "arrow/buffer.h"
#include "arrow/io/file.h"
#include "arrow/status.h"
#include "arrow/util/logging.h"

#include "parquet/column_reader.h"
#include "parquet/column_scanner.h"
#include "parquet/exception.h"
#include "parquet/metadata.h"
#include "parquet/properties.h"
#include "parquet/schema.h"
#include "parquet/types.h"
#include "parquet/util/memory.h"
#include "parquet/util/crypto.h"

using std::string;

namespace parquet {

// PARQUET-978: Minimize footer reads by reading 64 KB from the end of the file
static constexpr int64_t DEFAULT_FOOTER_READ_SIZE = 64 * 1024;
static constexpr uint32_t FOOTER_SIZE = 8;
static constexpr uint8_t PARQUET_MAGIC[4] = {'P', 'A', 'R', '1'};
static constexpr uint8_t PARQUET_EMAGIC[4] = {'P', 'A', 'R', 'E'};

// For PARQUET-816
static constexpr int64_t kMaxDictHeaderSize = 100;

// ----------------------------------------------------------------------
// RowGroupReader public API

RowGroupReader::RowGroupReader(std::unique_ptr<Contents> contents)
    : contents_(std::move(contents)) {}

std::shared_ptr<ColumnReader> RowGroupReader::Column(int i) {
  DCHECK(i < metadata()->num_columns())
      << "The RowGroup only has " << metadata()->num_columns()
      << "columns, requested column: " << i;
  const ColumnDescriptor* descr = metadata()->schema()->Column(i);

  std::unique_ptr<PageReader> page_reader = contents_->GetColumnPageReader(i);
  return ColumnReader::Make(
      descr, std::move(page_reader),
      const_cast<ReaderProperties*>(contents_->properties())->memory_pool());
}

std::unique_ptr<PageReader> RowGroupReader::GetColumnPageReader(int i) {
  DCHECK(i < metadata()->num_columns())
      << "The RowGroup only has " << metadata()->num_columns()
      << "columns, requested column: " << i;
  return contents_->GetColumnPageReader(i);
}

// Returns the rowgroup metadata
const RowGroupMetaData* RowGroupReader::metadata() const { return contents_->metadata(); }

// RowGroupReader::Contents implementation for the Parquet file specification
class SerializedRowGroup : public RowGroupReader::Contents {
 public:
  SerializedRowGroup(RandomAccessSource* source, FileMetaData* file_metadata,
                     FileCryptoMetaData* file_crypto_metadata, int row_group_number,
                     const ReaderProperties& props)
      : source_(source),
        file_metadata_(file_metadata),
        file_crypto_metadata_(file_crypto_metadata),
        properties_(props),
        row_group_ordinal_((int16_t)row_group_number){
    row_group_metadata_ = file_metadata->RowGroup(row_group_number);
  }

  const RowGroupMetaData* metadata() const override { return row_group_metadata_.get(); }

  const ReaderProperties* properties() const override { return &properties_; }

  std::unique_ptr<PageReader> GetColumnPageReader(int i) override {
    EncryptionAlgorithm algorithm;
    if (file_crypto_metadata_) {
      algorithm = file_crypto_metadata_->encryption_algorithm();
    }
    else if (file_metadata_->is_plaintext_mode()) {
      algorithm = file_metadata_->encryption_algorithm();
    }
    std::shared_ptr<std::map<std::shared_ptr<schema::ColumnPath>,
                             std::string, parquet::schema::ColumnPath::CmpColumnPath>>
      column_map = properties_.column_map();
    // Read column chunk from the file
    auto col = row_group_metadata_->ColumnChunk(i, row_group_ordinal_,
                                                properties_.file_decryption(),
                                                &algorithm,
                                                properties_.fileAAD(),
                                                column_map);
    int64_t col_start = col->data_page_offset();
    if (col->has_dictionary_page() && col_start > col->dictionary_page_offset()) {
      col_start = col->dictionary_page_offset();
    }

    int64_t col_length = col->total_compressed_size();
    std::unique_ptr<InputStream> stream;

    // PARQUET-816 workaround for old files created by older parquet-mr
    const ApplicationVersion& version = file_metadata_->writer_version();
    if (version.VersionLt(ApplicationVersion::PARQUET_816_FIXED_VERSION())) {
      // The Parquet MR writer had a bug in 1.2.8 and below where it didn't include the
      // dictionary page header size in total_compressed_size and total_uncompressed_size
      // (see IMPALA-694). We add padding to compensate.
      int64_t bytes_remaining = source_->Size() - (col_start + col_length);
      int64_t padding = std::min<int64_t>(kMaxDictHeaderSize, bytes_remaining);
      col_length += padding;
    }

    stream = properties_.GetStream(source_, col_start, col_length);
    std::unique_ptr<ColumnCryptoMetaData> crypto_metadata = col->crypto_metadata();

    bool encrypted = true;

    // file is unencrypted
    // or file is encrypted but column is unencrypted
    if ((!file_crypto_metadata_ && !file_metadata_->is_plaintext_mode()) || !crypto_metadata) {
      encrypted = false;
    }

    if (!encrypted) {
      return PageReader::Open(std::move(stream), col->num_values(), col->compression(),
                              col->has_dictionary_page(),
                              row_group_ordinal_,
                              (int16_t)i/* column_ordinal */,
                              nullptr, properties_.memory_pool());
    }

    // the column is encrypted
    std::string aad = parquet_encryption::createModuleAAD(
        properties_.fileAAD(),
        parquet_encryption::ColumnMetaData,
        row_group_ordinal_,
        (int16_t)i, (int16_t)-1);
    
    auto file_decryption = properties_.file_decryption();

    // the column is encrypted with footer key
    if (crypto_metadata->encrypted_with_footer_key()) {
      const std::string& footer_key_metadata = file_metadata_->is_plaintext_mode()
                                  ? file_metadata_->footer_signing_key_metadata()
                                  : file_crypto_metadata_->key_metadata();

      std::string footer_key = file_decryption->getFooterKey();
      // ignore footer key metadata if footer key is explicitly set via API
      if (footer_key.empty()) {
        if (footer_key_metadata.empty())
          throw ParquetException("No footer key or key metadata");

        if (file_decryption->getKeyRetriever() == nullptr)
          throw ParquetException("No footer key or key retriever");
        footer_key = file_decryption->getKeyRetriever()->GetKey(
            footer_key_metadata);
      }
      if (footer_key.empty()) {
        throw ParquetException("column is encrypted with null footer key");
      }
      ParquetCipher::type algorithm = file_metadata_->is_plaintext_mode()
                                ? file_metadata_->encryption_algorithm().algorithm
                                : file_crypto_metadata_->encryption_algorithm().algorithm;

      auto footer_encryption = std::make_shared<EncryptionProperties>(
          algorithm, footer_key, properties_.fileAAD(), aad);

      return PageReader::Open(std::move(stream), col->num_values(),
                              col->compression(),
                              col->has_dictionary_page(), row_group_ordinal_,
                              (int16_t)i, footer_encryption,
                              properties_.memory_pool());
    }

    // file is non-uniform encrypted and the column
    // is encrypted with its own key

    std::string column_key_metadata = crypto_metadata->key_metadata();
    std::shared_ptr<schema::ColumnPath> column_path =
        std::make_shared<schema::ColumnPath>(crypto_metadata->path_in_schema());
    // encrypted with column key
    std::string column_key;
    // first look if we already got the key from before
    if (column_map != NULLPTR
        && column_map->find(column_path) != column_map->end()) {
      column_key = column_map->at(column_path);
    } else {
      column_key = file_decryption->getColumnKey(column_path);
      // No explicit column key given via API. Retrieve via key metadata.
      if (column_key.empty() && !column_key_metadata.empty() &&
          file_decryption->getKeyRetriever() != nullptr){
        try {
          column_key = file_decryption->getKeyRetriever()->GetKey(
              column_key_metadata);
        } catch (KeyAccessDeniedException &e) {
          std::stringstream ss;
          ss << "HiddenColumnException, path=" +
            column_path->ToDotString() + " "
             << e.what() << "\n";
          throw HiddenColumnException(ss.str());
        }
      }
    }
    if (column_key.empty()) {
      throw HiddenColumnException("column is encrypted with null key, path=" +
                                  column_path->ToDotString());
    }
    auto column_encryption = std::make_shared<EncryptionProperties>(
        file_crypto_metadata_->encryption_algorithm().algorithm,
        column_key,
        properties_.fileAAD(), aad);

    return PageReader::Open(std::move(stream), col->num_values(),
                            col->compression(),
                            col->has_dictionary_page(), row_group_ordinal_,
                            (int16_t)i, column_encryption,
                            properties_.memory_pool());
  }

 private:
  RandomAccessSource* source_;
  FileMetaData* file_metadata_;
  FileCryptoMetaData* file_crypto_metadata_;
  std::unique_ptr<RowGroupMetaData> row_group_metadata_;
  ReaderProperties properties_;
  int16_t row_group_ordinal_;
};

// ----------------------------------------------------------------------
// SerializedFile: An implementation of ParquetFileReader::Contents that deals
// with the Parquet file structure, Thrift deserialization, and other internal
// matters

// This class takes ownership of the provided data source
class SerializedFile : public ParquetFileReader::Contents {
 public:
  SerializedFile(std::unique_ptr<RandomAccessSource> source,
                 const ReaderProperties& props = default_reader_properties())
      : source_(std::move(source)), properties_(props) {}

  ~SerializedFile() override {
    try {
      Close();
    } catch (...) {
    }
  }

  void Close() override { source_->Close(); }

  std::shared_ptr<RowGroupReader> GetRowGroup(int i) override {
    std::unique_ptr<SerializedRowGroup> contents(
        new SerializedRowGroup(source_.get(), file_metadata_.get(),
                               file_crypto_metadata_.get(), i, properties_));
    return std::make_shared<RowGroupReader>(std::move(contents));
  }

  std::shared_ptr<FileMetaData> metadata() const override { return file_metadata_; }

  void set_metadata(const std::shared_ptr<FileMetaData>& metadata) {
    file_metadata_ = metadata;
  }

  void ParseMetaData() {
    int64_t file_size = source_->Size();

    if (file_size < FOOTER_SIZE) {
      throw ParquetException("Corrupted file, smaller than file footer");
    }

    uint8_t footer_buffer[DEFAULT_FOOTER_READ_SIZE];
    int64_t footer_read_size = std::min(file_size, DEFAULT_FOOTER_READ_SIZE);
    int64_t bytes_read =
        source_->ReadAt(file_size - footer_read_size, footer_read_size, footer_buffer);

    // Check if all bytes are read. Check if last 4 bytes read have the magic bits
    // no encryption or encryption with plaintext footer
    // TODO: encryption with plaintext footer
    if (bytes_read == footer_read_size &&
        memcmp(footer_buffer + footer_read_size - 4, PARQUET_MAGIC, 4) == 0) {
      uint32_t metadata_len =
          *reinterpret_cast<uint32_t*>(footer_buffer + footer_read_size - FOOTER_SIZE);
      int64_t metadata_start = file_size - FOOTER_SIZE - metadata_len;
      if (FOOTER_SIZE + metadata_len > file_size) {
        throw ParquetException(
            "Invalid parquet file. File is less than "
            "file metadata size.");
      }

      std::shared_ptr<ResizableBuffer> metadata_buffer =
          AllocateBuffer(properties_.memory_pool(), metadata_len);

      // Check if the footer_buffer contains the entire metadata
      if (footer_read_size >= (metadata_len + FOOTER_SIZE)) {
        memcpy(metadata_buffer->mutable_data(),
               footer_buffer + (footer_read_size - metadata_len - FOOTER_SIZE),
               metadata_len);
      } else {
        bytes_read = source_->ReadAt(metadata_start, metadata_len,
                                     metadata_buffer->mutable_data());
        if (bytes_read != metadata_len) {
          throw ParquetException("Invalid parquet file. Could not read metadata bytes.");
        }
      }

      uint32_t read_metadata_len = metadata_len;
      file_metadata_ = FileMetaData::Make(metadata_buffer->data(), &read_metadata_len);

      if (file_metadata_->is_plaintext_mode()) {
        auto file_decryption = properties_.file_decryption();
        if (file_decryption == nullptr) {
          throw ParquetException("No decryption properties are provided");
        }

        EncryptionAlgorithm algo = file_metadata_->encryption_algorithm();
        bool supply_aad_prefix = algo.aad.supply_aad_prefix;
        std::string aad_file_unique = algo.aad.aad_file_unique;
        std::string aad_prefix = algo.aad.aad_prefix;
        if (algo.algorithm != ParquetCipher::AES_GCM_CTR_V1
            && algo.algorithm != ParquetCipher::AES_GCM_V1)
          throw ParquetException("Unsupported algorithm");
        if (!file_decryption->getAADPrefix().empty()) {
          if (file_decryption->getAADPrefix().compare(aad_prefix) != 0) {
            throw ParquetException("ADD Prefix in file and "
                                   "in properties is not the same");
          }
          std::shared_ptr<AADPrefixVerifier> aad_prefix_verifier =
            file_decryption->getAADPrefixVerifier();
          if (aad_prefix_verifier != NULLPTR)
            aad_prefix_verifier->check(aad_prefix);
        }
        if (supply_aad_prefix && file_decryption->getAADPrefix().empty()) {
          throw ParquetException("AAD prefix used for file encryption, "
                                 "but not stored in file and not supplied "
                                 "in decryption properties");
        }
        std::string fileAAD;
        if (!supply_aad_prefix)
          fileAAD = aad_prefix + aad_file_unique;
        else
          fileAAD = file_decryption->getAADPrefix() + aad_file_unique;

        properties_.set_fileAAD(fileAAD);
        if (file_decryption->checkFooterIntegrity()) {
          if (metadata_len - read_metadata_len != 28) {
            throw ParquetException("Invalid parquet file. Cannot verify plaintext"
           "mode footer.");
          }

          // get footer key
          std::string footer_key_metadata = file_metadata_->footer_signing_key_metadata();
          std::string footer_key = file_decryption->getFooterKey();
          // ignore footer key metadata if footer key is explicitly set via API
          if (footer_key.empty()) {
            if (footer_key_metadata.empty())
              throw ParquetException("No footer key or key metadata");
            if (file_decryption->getKeyRetriever() == nullptr)
              throw ParquetException("No footer key or key retriever");
            try {
              footer_key =
                file_decryption->getKeyRetriever()->GetKey(footer_key_metadata);
            } catch (KeyAccessDeniedException &e) {
              std::stringstream ss;
              ss << "Footer key: access denied " << e.what() << "\n";
              throw ParquetException(ss.str());
            }
          }
          if (footer_key.empty()) {
            throw ParquetException("Footer key unavailable. Could not verify "
                                   "plaintext footer metadata");
          }
          std::string aad = parquet_encryption::createFooterAAD(fileAAD);
          auto encryption = std::make_shared<EncryptionProperties>(
              file_metadata_->encryption_algorithm().algorithm,
              footer_key, fileAAD, aad);
          if (! file_metadata_->verify(encryption, metadata_buffer->data()
               + read_metadata_len, 28)) {
            throw ParquetException("Invalid parquet file. Could not verify plaintext"
                                   " footer metadata");
          }
        }
      }
    }
    // encryption with encrypted footer
    else if (bytes_read == footer_read_size &&
             memcmp(footer_buffer + footer_read_size - 4, PARQUET_EMAGIC, 4) == 0) {
      // both metadata & crypto metadata length
      uint32_t footer_len =
          *reinterpret_cast<uint32_t*>(footer_buffer + footer_read_size - FOOTER_SIZE);
      int64_t crypto_metadata_start = file_size - FOOTER_SIZE - footer_len;
       if (FOOTER_SIZE + footer_len > file_size) {
        throw ParquetException(
            "Invalid parquet file. File is less than "
            "file metadata size.");
      }
      std::shared_ptr<ResizableBuffer> crypto_metadata_buffer =
          AllocateBuffer(properties_.memory_pool(), footer_len);

      // Check if the footer_buffer contains the entire metadata
      if (footer_read_size >= (footer_len + FOOTER_SIZE)) {
        memcpy(crypto_metadata_buffer->mutable_data(),
               footer_buffer + (footer_read_size - footer_len - FOOTER_SIZE),
               footer_len);
      } else {
        bytes_read = source_->ReadAt(crypto_metadata_start, footer_len,
                                     crypto_metadata_buffer->mutable_data());
        if (bytes_read != footer_len) {
          throw ParquetException("Invalid parquet file. Could not read metadata bytes.");
        }
      }
      auto file_decryption = properties_.file_decryption();
      if (file_decryption == nullptr) {
        throw ParquetException("No decryption properties are provided. Could not read "
                               "encrypted footer metadata");
      }
      uint32_t crypto_metadata_len = footer_len;
      file_crypto_metadata_ =
        FileCryptoMetaData::Make(crypto_metadata_buffer->data(), &crypto_metadata_len);
      EncryptionAlgorithm algo = file_crypto_metadata_->encryption_algorithm();
      bool supply_aad_prefix = algo.aad.supply_aad_prefix;
      std::string aad_file_unique = algo.aad.aad_file_unique;
      std::string aad_prefix = algo.aad.aad_prefix;
      if (algo.algorithm != ParquetCipher::AES_GCM_CTR_V1 
          && algo.algorithm != ParquetCipher::AES_GCM_V1)
        throw ParquetException("Unsupported algorithm");
      if (!file_decryption->getAADPrefix().empty()) {
        if (file_decryption->getAADPrefix().compare(aad_prefix) != 0) {
          throw ParquetException("ADD Prefix in file and in properties "
                                 "is not the same");
        }
        std::shared_ptr<AADPrefixVerifier> aad_prefix_verifier =
          file_decryption->getAADPrefixVerifier();
        if (aad_prefix_verifier != NULLPTR)
          aad_prefix_verifier->check(aad_prefix);
      }
      if (supply_aad_prefix && file_decryption->getAADPrefix().empty()) {
        throw ParquetException("AAD prefix used for file encryption, "
                               "but not stored in file and not supplied "
                               "in decryption properties");
      }
      std::string fileAAD;
      if (!supply_aad_prefix)
        fileAAD = aad_prefix + aad_file_unique;
      else
        fileAAD = file_decryption->getAADPrefix() + aad_file_unique;
      // save fileAAD for later use
      properties_.set_fileAAD(fileAAD);
      std::string aad = parquet_encryption::createFooterAAD(fileAAD);
      int64_t metadata_offset = file_size - FOOTER_SIZE - footer_len + crypto_metadata_len;
      uint32_t metadata_len = footer_len - crypto_metadata_len;
      std::shared_ptr<ResizableBuffer> metadata_buffer =
        AllocateBuffer(properties_.memory_pool(), metadata_len);
      bytes_read =
        source_->ReadAt(metadata_offset, metadata_len, metadata_buffer->mutable_data());
      if (bytes_read != metadata_len) {
        throw ParquetException("Invalid encrypted parquet file. "
                               "Could not read footer metadata bytes.");
      }

      // get footer key metadata
      std::string footer_key_metadata = file_crypto_metadata_->key_metadata();
      std::string footer_key = file_decryption->getFooterKey();
      if (footer_key.empty()) {
        if (footer_key_metadata.empty())
          throw ParquetException("No footer key or key metadata");
        if (file_decryption->getKeyRetriever() == nullptr)
          throw ParquetException("No footer key or key retriever");
        try {
          footer_key =
            file_decryption->getKeyRetriever()->GetKey(footer_key_metadata);
        } catch (KeyAccessDeniedException &e) {
          std::stringstream ss;
          ss << "Footer key: access denied " << e.what() << "\n";;
          throw ParquetException(ss.str());
        }
      }
      if (footer_key.empty()) {
        throw ParquetException("Invalid footer encryption key. "
                               "Could not parse footer metadata");
      }
      auto footer_encryption = std::make_shared<EncryptionProperties>(
          file_crypto_metadata_->encryption_algorithm().algorithm,
          footer_key,
          fileAAD, aad);
      file_metadata_ = FileMetaData::Make(metadata_buffer->data(),
                                          &metadata_len,
                                          footer_encryption);
    }
    // error
    else {
      throw ParquetException("Invalid parquet file. Corrupt footer.");
    }
  }

 private:
  std::unique_ptr<RandomAccessSource> source_;
  std::shared_ptr<FileMetaData> file_metadata_;
  std::shared_ptr<FileCryptoMetaData> file_crypto_metadata_;
  ReaderProperties properties_;
};

// ----------------------------------------------------------------------
// ParquetFileReader public API

ParquetFileReader::ParquetFileReader() {}

ParquetFileReader::~ParquetFileReader() {
  try {
    Close();
  } catch (...) {
  }
}

// Open the file. If no metadata is passed, it is parsed from the footer of
// the file
std::unique_ptr<ParquetFileReader::Contents> ParquetFileReader::Contents::Open(
    std::unique_ptr<RandomAccessSource> source, const ReaderProperties& props,
    const std::shared_ptr<FileMetaData>& metadata) {
  std::unique_ptr<ParquetFileReader::Contents> result(
      new SerializedFile(std::move(source), props));

  // Access private methods here, but otherwise unavailable
  SerializedFile* file = static_cast<SerializedFile*>(result.get());

  if (metadata == nullptr) {
    // Validates magic bytes, parses metadata, and initializes the SchemaDescriptor
    file->ParseMetaData();
  } else {
    file->set_metadata(metadata);
  }

  return result;
}

std::unique_ptr<ParquetFileReader> ParquetFileReader::Open(
    const std::shared_ptr<::arrow::io::ReadableFileInterface>& source,
    const ReaderProperties& props, const std::shared_ptr<FileMetaData>& metadata) {
  std::unique_ptr<RandomAccessSource> io_wrapper(new ArrowInputFile(source));
  return Open(std::move(io_wrapper), props, metadata);
}

std::unique_ptr<ParquetFileReader> ParquetFileReader::Open(
    std::unique_ptr<RandomAccessSource> source, const ReaderProperties& props,
    const std::shared_ptr<FileMetaData>& metadata) {
  auto contents = SerializedFile::Open(std::move(source), props, metadata);
  std::unique_ptr<ParquetFileReader> result(new ParquetFileReader());
  result->Open(std::move(contents));
  return result;
}

std::unique_ptr<ParquetFileReader> ParquetFileReader::OpenFile(
    const std::string& path, bool memory_map, const ReaderProperties& props,
    const std::shared_ptr<FileMetaData>& metadata) {
  std::shared_ptr<::arrow::io::ReadableFileInterface> source;
  if (memory_map) {
    std::shared_ptr<::arrow::io::MemoryMappedFile> handle;
    PARQUET_THROW_NOT_OK(
        ::arrow::io::MemoryMappedFile::Open(path, ::arrow::io::FileMode::READ, &handle));
    source = handle;
  } else {
    std::shared_ptr<::arrow::io::ReadableFile> handle;
    PARQUET_THROW_NOT_OK(
        ::arrow::io::ReadableFile::Open(path, props.memory_pool(), &handle));
    source = handle;
  }

  return Open(source, props, metadata);
}

void ParquetFileReader::Open(std::unique_ptr<ParquetFileReader::Contents> contents) {
  contents_ = std::move(contents);
}

void ParquetFileReader::Close() {
  if (contents_) {
    contents_->Close();
  }
}

std::shared_ptr<FileMetaData> ParquetFileReader::metadata() const {
  return contents_->metadata();
}

std::shared_ptr<RowGroupReader> ParquetFileReader::RowGroup(int i) {
  DCHECK(i < metadata()->num_row_groups())
      << "The file only has " << metadata()->num_row_groups()
      << "row groups, requested reader for: " << i;
  return contents_->GetRowGroup(i);
}

// ----------------------------------------------------------------------
// File metadata helpers

std::shared_ptr<FileMetaData> ReadMetaData(
    const std::shared_ptr<::arrow::io::ReadableFileInterface>& source) {
  return ParquetFileReader::Open(source)->metadata();
}

// ----------------------------------------------------------------------
// File scanner for performance testing

int64_t ScanFileContents(std::vector<int> columns, const int32_t column_batch_size,
                         ParquetFileReader* reader) {
  std::vector<int16_t> rep_levels(column_batch_size);
  std::vector<int16_t> def_levels(column_batch_size);

  int num_columns = static_cast<int>(columns.size());

  // columns are not specified explicitly. Add all columns
  if (columns.size() == 0) {
    num_columns = reader->metadata()->num_columns();
    columns.resize(num_columns);
    for (int i = 0; i < num_columns; i++) {
      columns[i] = i;
    }
  }

  std::vector<int64_t> total_rows(num_columns, 0);

  for (int r = 0; r < reader->metadata()->num_row_groups(); ++r) {
    auto group_reader = reader->RowGroup(r);
    int col = 0;
    for (auto i : columns) {
      std::shared_ptr<ColumnReader> col_reader = group_reader->Column(i);
      size_t value_byte_size = GetTypeByteSize(col_reader->descr()->physical_type());
      std::vector<uint8_t> values(column_batch_size * value_byte_size);

      int64_t values_read = 0;
      while (col_reader->HasNext()) {
        int64_t levels_read =
            ScanAllValues(column_batch_size, def_levels.data(), rep_levels.data(),
                          values.data(), &values_read, col_reader.get());
        if (col_reader->descr()->max_repetition_level() > 0) {
          for (int64_t i = 0; i < levels_read; i++) {
            if (rep_levels[i] == 0) {
              total_rows[col]++;
            }
          }
        } else {
          total_rows[col] += levels_read;
        }
      }
      col++;
    }
  }

  for (int i = 1; i < num_columns; ++i) {
    if (total_rows[0] != total_rows[i]) {
      throw ParquetException("Parquet error: Total rows among columns do not match");
    }
  }

  return total_rows[0];
}

}  // namespace parquet
