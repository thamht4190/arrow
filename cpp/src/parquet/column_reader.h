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

#include <cstdint>
#include <memory>
#include <vector>

#include "parquet/exception.h"
#include "parquet/platform.h"
#include "parquet/schema.h"
#include "parquet/types.h"

namespace arrow {

class Array;
class ChunkedArray;

namespace BitUtil {
class BitReader;
}  // namespace BitUtil

namespace util {
class RleDecoder;
}  // namespace util

}  // namespace arrow

namespace parquet {

class Page;
class Decryptor;

// 16 MB is the default maximum page header size
static constexpr uint32_t kDefaultMaxPageHeaderSize = 16 * 1024 * 1024;

// 16 KB is the default expected page header size
static constexpr uint32_t kDefaultPageHeaderSize = 16 * 1024;

class PARQUET_EXPORT LevelDecoder {
 public:
  LevelDecoder();
  ~LevelDecoder();

  // Initialize the LevelDecoder state with new data
  // and return the number of bytes consumed
  int SetData(Encoding::type encoding, int16_t max_level, int num_buffered_values,
              const uint8_t* data);

  // Decodes a batch of levels into an array and returns the number of levels decoded
  int Decode(int batch_size, int16_t* levels);

 private:
  int bit_width_;
  int num_values_remaining_;
  Encoding::type encoding_;
  std::unique_ptr<::arrow::util::RleDecoder> rle_decoder_;
  std::unique_ptr<::arrow::BitUtil::BitReader> bit_packed_decoder_;
};

// Abstract page iterator interface. This way, we can feed column pages to the
// ColumnReader through whatever mechanism we choose
class PARQUET_EXPORT PageReader {
 public:
  virtual ~PageReader() = default;

  static std::unique_ptr<PageReader> Open(
      const std::shared_ptr<ArrowInputStream>& stream, int64_t total_num_rows,
      Compression::type codec, ::arrow::MemoryPool* pool = ::arrow::default_memory_pool(),
      bool column_has_dictionary = false, int16_t row_group_ordinal = -1,
      int16_t column_ordinal = -1, std::shared_ptr<Decryptor> meta_decryptor = NULLPTR,
      std::shared_ptr<Decryptor> data_decryptor = NULLPTR);

  // @returns: shared_ptr<Page>(nullptr) on EOS, std::shared_ptr<Page>
  // containing new Page otherwise
  virtual std::shared_ptr<Page> NextPage() = 0;

  virtual void set_max_page_header_size(uint32_t size) = 0;
};

class PARQUET_EXPORT ColumnReader {
 public:
  virtual ~ColumnReader() = default;

  static std::shared_ptr<ColumnReader> Make(
      const ColumnDescriptor* descr, std::unique_ptr<PageReader> pager,
      ::arrow::MemoryPool* pool = ::arrow::default_memory_pool());

  // Returns true if there are still values in this column.
  virtual bool HasNext() = 0;

  virtual Type::type type() const = 0;

  virtual const ColumnDescriptor* descr() const = 0;
};

// API to read values from a single column. This is a main client facing API.
template <typename DType>
class TypedColumnReader : public ColumnReader {
 public:
  typedef typename DType::c_type T;

  // Read a batch of repetition levels, definition levels, and values from the
  // column.
  //
  // Since null values are not stored in the values, the number of values read
  // may be less than the number of repetition and definition levels. With
  // nested data this is almost certainly true.
  //
  // Set def_levels or rep_levels to nullptr if you want to skip reading them.
  // This is only safe if you know through some other source that there are no
  // undefined values.
  //
  // To fully exhaust a row group, you must read batches until the number of
  // values read reaches the number of stored values according to the metadata.
  //
  // This API is the same for both V1 and V2 of the DataPage
  //
  // @returns: actual number of levels read (see values_read for number of values read)
  virtual int64_t ReadBatch(int64_t batch_size, int16_t* def_levels, int16_t* rep_levels,
                            T* values, int64_t* values_read) = 0;

  /// Read a batch of repetition levels, definition levels, and values from the
  /// column and leave spaces for null entries on the lowest level in the values
  /// buffer.
  ///
  /// In comparision to ReadBatch the length of repetition and definition levels
  /// is the same as of the number of values read for max_definition_level == 1.
  /// In the case of max_definition_level > 1, the repetition and definition
  /// levels are larger than the values but the values include the null entries
  /// with definition_level == (max_definition_level - 1).
  ///
  /// To fully exhaust a row group, you must read batches until the number of
  /// values read reaches the number of stored values according to the metadata.
  ///
  /// @param batch_size the number of levels to read
  /// @param[out] def_levels The Parquet definition levels, output has
  ///   the length levels_read.
  /// @param[out] rep_levels The Parquet repetition levels, output has
  ///   the length levels_read.
  /// @param[out] values The values in the lowest nested level including
  ///   spacing for nulls on the lowest levels; output has the length
  ///   values_read.
  /// @param[out] valid_bits Memory allocated for a bitmap that indicates if
  ///   the row is null or on the maximum definition level. For performance
  ///   reasons the underlying buffer should be able to store 1 bit more than
  ///   required. If this requires an additional byte, this byte is only read
  ///   but never written to.
  /// @param valid_bits_offset The offset in bits of the valid_bits where the
  ///   first relevant bit resides.
  /// @param[out] levels_read The number of repetition/definition levels that were read.
  /// @param[out] values_read The number of values read, this includes all
  ///   non-null entries as well as all null-entries on the lowest level
  ///   (i.e. definition_level == max_definition_level - 1)
  /// @param[out] null_count The number of nulls on the lowest levels.
  ///   (i.e. (values_read - null_count) is total number of non-null entries)
  virtual int64_t ReadBatchSpaced(int64_t batch_size, int16_t* def_levels,
                                  int16_t* rep_levels, T* values, uint8_t* valid_bits,
                                  int64_t valid_bits_offset, int64_t* levels_read,
                                  int64_t* values_read, int64_t* null_count) = 0;

  // Skip reading levels
  // Returns the number of levels skipped
  virtual int64_t Skip(int64_t num_rows_to_skip) = 0;
};

namespace internal {

/// \brief Stateful column reader that delimits semantic records for both flat
/// and nested columns
///
/// \note API EXPERIMENTAL
/// \since 1.3.0
class RecordReader {
 public:
  static std::shared_ptr<RecordReader> Make(
      const ColumnDescriptor* descr,
      ::arrow::MemoryPool* pool = ::arrow::default_memory_pool(),
      const bool read_dictionary = false);

  virtual ~RecordReader() = default;

  /// \brief Attempt to read indicated number of records from column chunk
  /// \return number of records read
  virtual int64_t ReadRecords(int64_t num_records) = 0;

  /// \brief Pre-allocate space for data. Results in better flat read performance
  virtual void Reserve(int64_t num_values) = 0;

  /// \brief Clear consumed values and repetition/definition levels as the
  /// result of calling ReadRecords
  virtual void Reset() = 0;

  /// \brief Transfer filled values buffer to caller. A new one will be
  /// allocated in subsequent ReadRecords calls
  virtual std::shared_ptr<ResizableBuffer> ReleaseValues() = 0;

  /// \brief Transfer filled validity bitmap buffer to caller. A new one will
  /// be allocated in subsequent ReadRecords calls
  virtual std::shared_ptr<ResizableBuffer> ReleaseIsValid() = 0;

  /// \brief Return true if the record reader has more internal data yet to
  /// process
  virtual bool HasMoreData() const = 0;

  /// \brief Advance record reader to the next row group
  /// \param[in] reader obtained from RowGroupReader::GetColumnPageReader
  virtual void SetPageReader(std::unique_ptr<PageReader> reader) = 0;

  virtual void DebugPrintState() = 0;

  /// \brief Decoded definition levels
  int16_t* def_levels() const {
    return reinterpret_cast<int16_t*>(def_levels_->mutable_data());
  }

  /// \brief Decoded repetition levels
  int16_t* rep_levels() const {
    return reinterpret_cast<int16_t*>(rep_levels_->mutable_data());
  }

  /// \brief Decoded values, including nulls, if any
  uint8_t* values() const { return values_->mutable_data(); }

  /// \brief Number of values written including nulls (if any)
  int64_t values_written() const { return values_written_; }

  /// \brief Number of definition / repetition levels (from those that have
  /// been decoded) that have been consumed inside the reader.
  int64_t levels_position() const { return levels_position_; }

  /// \brief Number of definition / repetition levels that have been written
  /// internally in the reader
  int64_t levels_written() const { return levels_written_; }

  /// \brief Number of nulls in the leaf
  int64_t null_count() const { return null_count_; }

  /// \brief True if the leaf values are nullable
  bool nullable_values() const { return nullable_values_; }

  /// \brief True if reading directly as Arrow dictionary-encoded
  bool read_dictionary() const { return read_dictionary_; }

 protected:
  bool nullable_values_;

  bool at_record_start_;
  int64_t records_read_;

  int64_t values_written_;
  int64_t values_capacity_;
  int64_t null_count_;

  int64_t levels_written_;
  int64_t levels_position_;
  int64_t levels_capacity_;

  std::shared_ptr<::arrow::ResizableBuffer> values_;
  // In the case of false, don't allocate the values buffer (when we directly read into
  // builder classes).
  bool uses_values_;

  std::shared_ptr<::arrow::ResizableBuffer> valid_bits_;
  std::shared_ptr<::arrow::ResizableBuffer> def_levels_;
  std::shared_ptr<::arrow::ResizableBuffer> rep_levels_;

  bool read_dictionary_ = false;
};

class BinaryRecordReader : virtual public RecordReader {
 public:
  virtual std::vector<std::shared_ptr<::arrow::Array>> GetBuilderChunks() = 0;
};

/// \brief Read records directly to dictionary-encoded Arrow form (int32
/// indices). Only valid for BYTE_ARRAY columns
class DictionaryRecordReader : virtual public RecordReader {
 public:
  virtual std::shared_ptr<::arrow::ChunkedArray> GetResult() = 0;
};

static inline void DefinitionLevelsToBitmap(
    const int16_t* def_levels, int64_t num_def_levels, const int16_t max_definition_level,
    const int16_t max_repetition_level, int64_t* values_read, int64_t* null_count,
    uint8_t* valid_bits, int64_t valid_bits_offset) {
  // We assume here that valid_bits is large enough to accommodate the
  // additional definition levels and the ones that have already been written
  ::arrow::internal::BitmapWriter valid_bits_writer(valid_bits, valid_bits_offset,
                                                    num_def_levels);

  // TODO(itaiin): As an interim solution we are splitting the code path here
  // between repeated+flat column reads, and non-repeated+nested reads.
  // Those paths need to be merged in the future
  for (int i = 0; i < num_def_levels; ++i) {
    if (def_levels[i] == max_definition_level) {
      valid_bits_writer.Set();
    } else if (max_repetition_level > 0) {
      // repetition+flat case
      if (def_levels[i] == (max_definition_level - 1)) {
        valid_bits_writer.Clear();
        *null_count += 1;
      } else {
        continue;
      }
    } else {
      // non-repeated+nested case
      if (def_levels[i] < max_definition_level) {
        valid_bits_writer.Clear();
        *null_count += 1;
      } else {
        throw ParquetException("definition level exceeds maximum");
      }
    }

    valid_bits_writer.Next();
  }
  valid_bits_writer.Finish();
  *values_read = valid_bits_writer.position();
}

}  // namespace internal

namespace internal {

// TODO(itaiin): another code path split to merge when the general case is done
static inline bool HasSpacedValues(const ColumnDescriptor* descr) {
  if (descr->max_repetition_level() > 0) {
    // repeated+flat case
    return !descr->schema_node()->is_required();
  } else {
    // non-repeated+nested case
    // Find if a node forces nulls in the lowest level along the hierarchy
    const schema::Node* node = descr->schema_node().get();
    while (node) {
      if (node->is_optional()) {
        return true;
      }
      node = node->parent();
    }
    return false;
  }
}

}  // namespace internal

using BoolReader = TypedColumnReader<BooleanType>;
using Int32Reader = TypedColumnReader<Int32Type>;
using Int64Reader = TypedColumnReader<Int64Type>;
using Int96Reader = TypedColumnReader<Int96Type>;
using FloatReader = TypedColumnReader<FloatType>;
using DoubleReader = TypedColumnReader<DoubleType>;
using ByteArrayReader = TypedColumnReader<ByteArrayType>;
using FixedLenByteArrayReader = TypedColumnReader<FLBAType>;

}  // namespace parquet
