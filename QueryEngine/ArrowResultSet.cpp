/*
 * Copyright 2022 HEAVY.AI, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "QueryEngine/ArrowResultSet.h"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/api.h>

#include "QueryEngine/Descriptors/RelAlgExecutionDescriptor.h"
#include "Shared/ArrowUtil.h"

namespace {

SQLTypeInfo type_from_arrow_field(const arrow::Field& field) {
  switch (field.type()->id()) {
    case arrow::Type::INT8:
      return SQLTypeInfo(kTINYINT, !field.nullable());
    case arrow::Type::INT16:
      return SQLTypeInfo(kSMALLINT, !field.nullable());
    case arrow::Type::INT32:
      return SQLTypeInfo(kINT, !field.nullable());
    case arrow::Type::INT64:
      return SQLTypeInfo(kBIGINT, !field.nullable());
    case arrow::Type::FLOAT:
      return SQLTypeInfo(kFLOAT, !field.nullable());
    case arrow::Type::DOUBLE:
      return SQLTypeInfo(kDOUBLE, !field.nullable());
    case arrow::Type::DICTIONARY:
      return SQLTypeInfo(kTEXT, !field.nullable(), kENCODING_DICT);
    case arrow::Type::TIMESTAMP: {
      // TODO(Wamsi): go right fold expr in c++17
      auto get_precision = [&field](auto type) { return field.type()->Equals(type); };
      if (get_precision(arrow::timestamp(arrow::TimeUnit::SECOND))) {
        return SQLTypeInfo(kTIMESTAMP, !field.nullable());
      } else if (get_precision(arrow::timestamp(arrow::TimeUnit::MILLI))) {
        return SQLTypeInfo(kTIMESTAMP, 3, 0, !field.nullable());
      } else if (get_precision(arrow::timestamp(arrow::TimeUnit::MICRO))) {
        return SQLTypeInfo(kTIMESTAMP, 6, 0, !field.nullable());
      } else if (get_precision(arrow::timestamp(arrow::TimeUnit::NANO))) {
        return SQLTypeInfo(kTIMESTAMP, 9, 0, !field.nullable());
      } else {
        UNREACHABLE();
      }
    }
    case arrow::Type::DATE32:
      return SQLTypeInfo(kDATE, !field.nullable(), kENCODING_DATE_IN_DAYS);
    case arrow::Type::DATE64:
      return SQLTypeInfo(kDATE, !field.nullable());
    case arrow::Type::TIME32:
      return SQLTypeInfo(kTIME, !field.nullable());
    default:
      CHECK(false);
  }
  CHECK(false);
  return SQLTypeInfo();
}

}  // namespace

ArrowResultSet::ArrowResultSet(const std::shared_ptr<ResultSet>& rows,
                               const std::vector<TargetMetaInfo>& targets_meta,
                               const ExecutorDeviceType device_type)
    : rows_(rows), targets_meta_(targets_meta), crt_row_idx_(0) {
  resultSetArrowLoopback(device_type);
  auto schema = record_batch_->schema();
  for (int i = 0; i < schema->num_fields(); ++i) {
    std::shared_ptr<arrow::Field> field = schema->field(i);
    SQLTypeInfo type_info = type_from_arrow_field(*schema->field(i));
    column_metainfo_.emplace_back(field->name(), type_info);
    columns_.emplace_back(record_batch_->column(i));
  }
}

ArrowResultSet::ArrowResultSet(
    const std::shared_ptr<ResultSet>& rows,
    const std::vector<TargetMetaInfo>& targets_meta,
    const ExecutorDeviceType device_type,
    const size_t min_result_size_for_bulk_dictionary_fetch,
    const double max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch)
    : rows_(rows), targets_meta_(targets_meta), crt_row_idx_(0) {
  resultSetArrowLoopback(device_type,
                         min_result_size_for_bulk_dictionary_fetch,
                         max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch);
  auto schema = record_batch_->schema();
  for (int i = 0; i < schema->num_fields(); ++i) {
    std::shared_ptr<arrow::Field> field = schema->field(i);
    SQLTypeInfo type_info = type_from_arrow_field(*schema->field(i));
    column_metainfo_.emplace_back(field->name(), type_info);
    columns_.emplace_back(record_batch_->column(i));
  }
}

template <typename Type, typename ArrayType>
void ArrowResultSet::appendValue(std::vector<TargetValue>& row,
                                 const arrow::Array& column,
                                 const Type null_val,
                                 const size_t idx) const {
  const auto& col = static_cast<const ArrayType&>(column);
  row.emplace_back(col.IsNull(idx) ? null_val : static_cast<Type>(col.Value(idx)));
}

std::vector<std::string> ArrowResultSet::getDictionaryStrings(
    const size_t col_idx) const {
  if (col_idx >= colCount()) {
    throw std::runtime_error("ArrowResultSet::getDictionaryStrings: col_idx is invalid.");
  }
  const auto& column_typeinfo = getColType(col_idx);
  if (column_typeinfo.get_type() != kTEXT) {
    throw std::runtime_error(
        "ArrowResultSet::getDictionaryStrings: col_idx does not refer to column of type "
        "TEXT.");
  }
  CHECK_EQ(kENCODING_DICT, column_typeinfo.get_compression());
  const auto& column = *columns_[col_idx];
  CHECK_EQ(arrow::Type::DICTIONARY, column.type_id());
  const auto& dict_column = static_cast<const arrow::DictionaryArray&>(column);
  const auto& dictionary =
      static_cast<const arrow::StringArray&>(*dict_column.dictionary());
  const size_t dictionary_size = dictionary.length();
  std::vector<std::string> dictionary_strings;
  dictionary_strings.reserve(dictionary_size);
  for (size_t d = 0; d < dictionary_size; ++d) {
    dictionary_strings.emplace_back(dictionary.GetString(d));
  }
  return dictionary_strings;
}

std::vector<TargetValue> ArrowResultSet::getRowAt(const size_t index) const {
  if (index >= rowCount()) {
    return {};
  }

  CHECK_LT(index, rowCount());
  std::vector<TargetValue> row;
  for (int i = 0; i < record_batch_->num_columns(); ++i) {
    const auto& column = *columns_[i];
    const auto& column_typeinfo = getColType(i);
    switch (column_typeinfo.get_type()) {
      case kTINYINT: {
        CHECK_EQ(arrow::Type::INT8, column.type_id());
        appendValue<int64_t, arrow::Int8Array>(
            row, column, inline_int_null_val(column_typeinfo), index);
        break;
      }
      case kSMALLINT: {
        CHECK_EQ(arrow::Type::INT16, column.type_id());
        appendValue<int64_t, arrow::Int16Array>(
            row, column, inline_int_null_val(column_typeinfo), index);
        break;
      }
      case kINT: {
        CHECK_EQ(arrow::Type::INT32, column.type_id());
        appendValue<int64_t, arrow::Int32Array>(
            row, column, inline_int_null_val(column_typeinfo), index);
        break;
      }
      case kBIGINT: {
        CHECK_EQ(arrow::Type::INT64, column.type_id());
        appendValue<int64_t, arrow::Int64Array>(
            row, column, inline_int_null_val(column_typeinfo), index);
        break;
      }
      case kFLOAT: {
        CHECK_EQ(arrow::Type::FLOAT, column.type_id());
        appendValue<float, arrow::FloatArray>(
            row, column, inline_fp_null_value<float>(), index);
        break;
      }
      case kDOUBLE: {
        CHECK_EQ(arrow::Type::DOUBLE, column.type_id());
        appendValue<double, arrow::DoubleArray>(
            row, column, inline_fp_null_value<double>(), index);
        break;
      }
      case kTEXT: {
        CHECK_EQ(kENCODING_DICT, column_typeinfo.get_compression());
        CHECK_EQ(arrow::Type::DICTIONARY, column.type_id());
        const auto& dict_column = static_cast<const arrow::DictionaryArray&>(column);
        if (dict_column.IsNull(index)) {
          row.emplace_back(NullableString(nullptr));
        } else {
          const auto& indices =
              static_cast<const arrow::Int32Array&>(*dict_column.indices());
          const auto& dictionary =
              static_cast<const arrow::StringArray&>(*dict_column.dictionary());
          row.emplace_back(dictionary.GetString(indices.Value(index)));
        }
        break;
      }
      case kTIMESTAMP: {
        CHECK_EQ(arrow::Type::TIMESTAMP, column.type_id());
        appendValue<int64_t, arrow::TimestampArray>(
            row, column, inline_int_null_val(column_typeinfo), index);
        break;
      }
      case kDATE: {
        // TODO(wamsi): constexpr?
        CHECK(arrow::Type::DATE32 == column.type_id() ||
              arrow::Type::DATE64 == column.type_id());
        column_typeinfo.is_date_in_days()
            ? appendValue<int64_t, arrow::Date32Array>(
                  row, column, inline_int_null_val(column_typeinfo), index)
            : appendValue<int64_t, arrow::Date64Array>(
                  row, column, inline_int_null_val(column_typeinfo), index);
        break;
      }
      case kTIME: {
        CHECK_EQ(arrow::Type::TIME32, column.type_id());
        appendValue<int64_t, arrow::Time32Array>(
            row, column, inline_int_null_val(column_typeinfo), index);
        break;
      }
      default:
        CHECK(false);
    }
  }
  return row;
}

std::vector<TargetValue> ArrowResultSet::getNextRow(const bool translate_strings,
                                                    const bool decimal_to_double) const {
  if (crt_row_idx_ == rowCount()) {
    return {};
  }
  CHECK_LT(crt_row_idx_, rowCount());
  auto row = getRowAt(crt_row_idx_);
  ++crt_row_idx_;
  return row;
}

size_t ArrowResultSet::colCount() const {
  return column_metainfo_.size();
}

SQLTypeInfo ArrowResultSet::getColType(const size_t col_idx) const {
  CHECK_LT(col_idx, column_metainfo_.size());
  return column_metainfo_[col_idx].get_type_info();
}

bool ArrowResultSet::definitelyHasNoRows() const {
  return !rowCount();
}

size_t ArrowResultSet::rowCount() const {
  return record_batch_->num_rows();
}

// Function is for parity with ResultSet interface
// and associated tests
size_t ArrowResultSet::entryCount() const {
  return rowCount();
}

// Function is for parity with ResultSet interface
// and associated tests
bool ArrowResultSet::isEmpty() const {
  return rowCount() == static_cast<size_t>(0);
}

void ArrowResultSet::resultSetArrowLoopback(const ExecutorDeviceType device_type) {
  resultSetArrowLoopback(
      device_type,
      ArrowResultSetConverter::default_min_result_size_for_bulk_dictionary_fetch,
      ArrowResultSetConverter::
          default_max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch);
}

void ArrowResultSet::resultSetArrowLoopback(
    const ExecutorDeviceType device_type,
    const size_t min_result_size_for_bulk_dictionary_fetch,
    const double max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch) {
  std::vector<std::string> col_names;

  if (!targets_meta_.empty()) {
    for (auto& meta : targets_meta_) {
      col_names.push_back(meta.get_resname());
    }
  } else {
    for (unsigned int i = 0; i < rows_->colCount(); i++) {
      col_names.push_back("col_" + std::to_string(i));
    }
  }

  // We convert the given rows to arrow, which gets serialized
  // into a buffer by Arrow Wire.
  auto converter = ArrowResultSetConverter(
      rows_,
      col_names,
      -1,
      min_result_size_for_bulk_dictionary_fetch,
      max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch);
  converter.transport_method_ = ArrowTransport::WIRE;
  converter.device_type_ = device_type;

  // Lifetime of the result buffer is that of ArrowResultSet
  results_ = std::make_shared<ArrowResult>(converter.getArrowResult());

  // Create a reader for reading back serialized
  auto const* buffer_data = reinterpret_cast<const uint8_t*>(results_->df_buffer.data());
#if ARROW_VERSION_MAJOR >= 14
  auto buffer = std::make_shared<arrow::Buffer>(buffer_data, results_->df_size);
  arrow::io::BufferReader reader(buffer);
#else
  arrow::io::BufferReader reader(buffer_data, results_->df_size);
#endif
  ARROW_ASSIGN_OR_THROW(auto batch_reader,
                        arrow::ipc::RecordBatchStreamReader::Open(&reader));

  ARROW_THROW_NOT_OK(batch_reader->ReadNext(&record_batch_));

  // Collect dictionaries from the record batch into the dictionary memo.
  ARROW_THROW_NOT_OK(
      arrow::ipc::internal::CollectDictionaries(*record_batch_, &dictionary_memo_));

  CHECK_EQ(record_batch_->schema()->num_fields(), record_batch_->num_columns());
}

std::unique_ptr<ArrowResultSet> result_set_arrow_loopback(
    const ExecutionResult& results) {
  // NOTE(wesm): About memory ownership

  // After calling ReadRecordBatch, the buffers inside arrow::RecordBatch now
  // share ownership of the memory in serialized_arrow_output.records (zero
  // copy). Not necessary to retain these buffers. Same is true of any
  // dictionaries contained in serialized_arrow_output.schema; the arrays
  // reference that memory (zero copy).
  return std::make_unique<ArrowResultSet>(results.getRows(), results.getTargetsMeta());
}

std::unique_ptr<ArrowResultSet> result_set_arrow_loopback(
    const ExecutionResult* results,
    const std::shared_ptr<ResultSet>& rows,
    const ExecutorDeviceType device_type) {
  return results ? std::make_unique<ArrowResultSet>(
                       rows, results->getTargetsMeta(), device_type)
                 : std::make_unique<ArrowResultSet>(rows, device_type);
}

std::unique_ptr<ArrowResultSet> result_set_arrow_loopback(
    const ExecutionResult* results,
    const std::shared_ptr<ResultSet>& rows,
    const ExecutorDeviceType device_type,
    const size_t min_result_size_for_bulk_dictionary_fetch,
    const double max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch) {
  std::vector<TargetMetaInfo> dummy_targets_meta;
  return results ? std::make_unique<ArrowResultSet>(
                       rows,
                       results->getTargetsMeta(),
                       device_type,
                       min_result_size_for_bulk_dictionary_fetch,
                       max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch)
                 : std::make_unique<ArrowResultSet>(
                       rows,
                       dummy_targets_meta,
                       device_type,
                       min_result_size_for_bulk_dictionary_fetch,
                       max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch);
}
