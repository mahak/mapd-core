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

#include "ColumnarResults.h"
#include "Descriptors/RowSetMemoryOwner.h"
#include "ErrorHandling.h"
#include "Execute.h"
#include "Geospatial/Compression.h"
#include "Geospatial/Types.h"
#include "Shared/Intervals.h"
#include "Shared/likely.h"
#include "Shared/sqltypes.h"
#include "Shared/thread_count.h"

#include <tbb/parallel_reduce.h>
#include <atomic>
#include <future>
#include <numeric>

namespace {

inline int64_t fixed_encoding_nullable_val(const int64_t val,
                                           const SQLTypeInfo& type_info) {
  if (type_info.get_compression() != kENCODING_NONE) {
    CHECK(type_info.get_compression() == kENCODING_FIXED ||
          type_info.get_compression() == kENCODING_DICT);
    auto logical_ti = get_logical_type_info(type_info);
    if (val == inline_int_null_val(logical_ti)) {
      return inline_fixed_encoding_null_val(type_info);
    }
  }
  return val;
}

std::vector<size_t> get_padded_target_sizes(
    const ResultSet& rows,
    const std::vector<SQLTypeInfo>& target_types) {
  std::vector<size_t> padded_target_sizes;
  // We have to check that the result set is valid as one entry point
  // to columnar results constructs effectively a fake result set.
  // In these cases it should be safe to assume that we can use the type
  // target widths
  if (!rows.hasValidBuffer() ||
      rows.getQueryMemDesc().getColCount() < target_types.size()) {
    for (const auto& target_type : target_types) {
      padded_target_sizes.emplace_back(target_type.get_size());
    }
    return padded_target_sizes;
  }

  // If here we have a valid result set, so use it's QMD padded widths
  const auto col_context = rows.getQueryMemDesc().getColSlotContext();
  for (size_t col_idx = 0; col_idx < target_types.size(); col_idx++) {
    // Lazy fetch columns will have 0 as a padded with, so use the type's
    // logical width for those
    const auto idx = col_context.getSlotsForCol(col_idx).front();
    const size_t padded_slot_width =
        static_cast<size_t>(rows.getPaddedSlotWidthBytes(idx));
    padded_target_sizes.emplace_back(
        padded_slot_width == 0UL ? target_types[col_idx].get_size() : padded_slot_width);
  }
  return padded_target_sizes;
}

int64_t toBuffer(const TargetValue& col_val, const SQLTypeInfo& type_info, int8_t* buf) {
  CHECK(!type_info.is_geometry());
  if (type_info.is_array()) {
    const auto array_col_val = boost::get<ArrayTargetValue>(&col_val);
    CHECK(array_col_val);
    const auto& vec = array_col_val->get();
    int64_t offset = 0;
    const auto elem_type_info = type_info.get_elem_type();
    for (const auto& item : vec) {
      offset += toBuffer(item, elem_type_info, buf + offset);
    }
    return offset;
  } else if (type_info.is_fp()) {
    const auto scalar_col_val = boost::get<ScalarTargetValue>(&col_val);
    switch (type_info.get_type()) {
      case kFLOAT: {
        auto float_p = boost::get<float>(scalar_col_val);
        *((float*)buf) = static_cast<float>(*float_p);
        return 4;
      }
      case kDOUBLE: {
        auto double_p = boost::get<double>(scalar_col_val);
        *((double*)buf) = static_cast<double>(*double_p);
        return 8;
      }
      default:
        UNREACHABLE();
    }
  } else {
    const auto scalar_col_val = boost::get<ScalarTargetValue>(&col_val);
    CHECK(scalar_col_val);
    auto i64_p = boost::get<int64_t>(scalar_col_val);
    const auto val = fixed_encoding_nullable_val(*i64_p, type_info);
    switch (type_info.get_size()) {
      case 1:
        *buf = static_cast<int8_t>(val);
        return 1;
      case 2:
        *((int16_t*)buf) = static_cast<int16_t>(val);
        return 2;
      case 4:
        *((int32_t*)buf) = static_cast<int32_t>(val);
        return 4;
      case 8:
        *((int64_t*)buf) = static_cast<int64_t>(val);
        return 8;
      default:
        UNREACHABLE();
    }
  }
  return 0;
}

/*
  computeTotalNofValuesForColumn<Type> functions compute the total
  number of values that exists in a result set. The total number of
  values defines the maximal number of values that a FlatBuffer
  storage will be able to hold.

  A "value" is defined as the largest fixed-size element in a column
  structure.

  For instance, for a column of scalars or a column of
  (varlen) arrays of scalars, the "value" is a scalar value. For a
  column of geo-types (points, multipoints, etc), the "value" is a
  Point (a Point is a two-tuple of coordinate values). For a column
  of TextEncodingNone and column of arrays of TextEncodingNone, the
  "value" is a byte value.
 */

int64_t computeTotalNofValuesForColumnArray(const ResultSet& rows,
                                            const size_t column_idx) {
  return tbb::parallel_reduce(
      tbb::blocked_range<int64_t>(0, rows.entryCount()),
      static_cast<int64_t>(0),
      [&](tbb::blocked_range<int64_t> r, int64_t running_count) {
        for (int i = r.begin(); i < r.end(); ++i) {
          const auto crt_row = rows.getRowAtNoTranslations(i);
          if (crt_row.empty()) {
            continue;
          }
          const auto arr_tv = boost::get<ArrayTargetValue>(&crt_row[column_idx]);
          CHECK(arr_tv);
          if (arr_tv->is_initialized()) {
            const auto& vec = arr_tv->get();
            running_count += vec.size();
          }
        }
        return running_count;
      },
      std::plus<int64_t>());
}

template <typename TargetValue, typename TargetValuePtr>
int64_t computeTotalNofValuesForColumnGeoType(const ResultSet& rows,
                                              const SQLTypeInfo& ti,
                                              const size_t column_idx) {
  return tbb::parallel_reduce(
      tbb::blocked_range<int64_t>(0, rows.entryCount()),
      static_cast<int64_t>(0),
      [&](tbb::blocked_range<int64_t> r, int64_t running_count) {
        for (int i = r.begin(); i < r.end(); ++i) {
          const auto crt_row = rows.getRowAtNoTranslations(i);
          if (crt_row.empty()) {
            continue;
          }
          if (const auto tv = boost::get<ScalarTargetValue>(&crt_row[column_idx])) {
            const auto ns = boost::get<NullableString>(tv);
            CHECK(ns);
            const auto s_ptr = boost::get<std::string>(ns);
            if (s_ptr) {
              // We count the number of commas in WKT representation
              // (e.g. POLYGON ((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2
              // 1,1 1))) to get the number of points it contains.
              // This method is usable for any geo type.
              running_count += std::count(s_ptr->begin(), s_ptr->end(), ',') + 1;
            }
          } else if (const auto tv =
                         boost::get<GeoTargetValuePtr>(&crt_row[column_idx])) {
            const auto s = boost::get<TargetValuePtr>(tv);
            CHECK(s);
            VarlenDatum* d = s->coords_data.get();
            if (d != nullptr) {
              running_count +=
                  d->length /
                  (ti.get_compression() == kENCODING_GEOINT ? sizeof(int32_t)
                                                            : sizeof(double)) /
                  2;
            }  // else s is NULL
          } else if (const auto tv = boost::get<GeoTargetValue>(&crt_row[column_idx])) {
            if (tv->get_ptr() != nullptr) {
              const auto s = boost::get<TargetValue>(tv->get());
              std::vector<double>* d = s.coords.get();
              CHECK(d);
              running_count += d->size();
            }  // else s is NULL
          } else {
            UNREACHABLE();
          }
        }
        return running_count;
      },
      std::plus<int64_t>());
}

int64_t computeTotalNofValuesForColumnTextEncodingNone(const ResultSet& rows,
                                                       const size_t column_idx) {
  return tbb::parallel_reduce(
      tbb::blocked_range<int64_t>(0, rows.entryCount()),
      static_cast<int64_t>(0),
      [&](tbb::blocked_range<int64_t> r, int64_t running_count) {
        for (int i = r.begin(); i < r.end(); ++i) {
          // Apparently, ResultSet permutation vector may be sparse
          // (len(permutation) > entryCount), so we cannot ignore the
          // permutation vector when iterating over all entries.
          const auto crt_row = rows.getRowAtNoTranslations(i);
          if (crt_row.empty()) {
            continue;
          }
          const auto col_val = crt_row[column_idx];
          if (const auto tv = boost::get<ScalarTargetValue>(&col_val)) {
            const auto ns = boost::get<NullableString>(tv);
            CHECK(ns);
            const auto s_ptr = boost::get<std::string>(ns);
            if (s_ptr) {
              running_count += s_ptr->size();
            }
          } else {
            UNREACHABLE();
          }
        }
        return running_count;
      },
      std::plus<int64_t>());
}

}  // namespace

ColumnarResults::ColumnarResults(std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
                                 const ResultSet& rows,
                                 const size_t num_columns,
                                 const std::vector<SQLTypeInfo>& target_types,
                                 const size_t executor_id,
                                 const size_t thread_idx,
                                 const bool is_parallel_execution_enforced)
    : column_buffers_(num_columns)
    , direct_columnar_conversion_(rows.isDirectColumnarConversionPossible())
    , num_rows_(direct_columnar_conversion_ ? rows.entryCount() : rows.rowCount())
    , target_types_(target_types)
    , parallel_conversion_(is_parallel_execution_enforced ||
                           result_set::use_parallel_algorithms(rows))
    , thread_idx_(thread_idx)
    , padded_target_sizes_(get_padded_target_sizes(rows, target_types)) {
  auto timer = DEBUG_TIMER(__func__);
  column_buffers_.resize(num_columns);
  executor_ = Executor::getExecutor(executor_id);
  VLOG(1) << "Columnarize resultset, # rows: " << num_rows_
          << " (rowCount: " << rows.rowCount() << ", entryCount: " << rows.entryCount()
          << "), direct columnarization? " << ::toString(direct_columnar_conversion_)
          << ", parallel conversion? " << ::toString(parallel_conversion_);
  CHECK(executor_);
  CHECK_EQ(padded_target_sizes_.size(), target_types.size());

  for (size_t i = 0; i < num_columns; ++i) {
    const auto& src_ti = rows.getColType(i);
    // ti is initialized in columnarize_result() function in
    // ColumnFetcher.cpp and it may differ from src_ti with respect to
    // uses_flatbuffer attribute
    const auto& ti = target_types_[i];

    if (rows.isZeroCopyColumnarConversionPossible(i)) {
      CHECK_EQ(ti.usesFlatBuffer(), src_ti.usesFlatBuffer());
      // The column buffer will be assigned in
      // ColumnarResults::copyAllNonLazyColumns.
      column_buffers_[i] = nullptr;
      continue;
    }
    CHECK(!(src_ti.usesFlatBuffer() && ti.usesFlatBuffer()));
    // When the source result set uses FlatBuffer layout, it must
    // support zero-copy columnar conversion. Otherwise, the source
    // result will be columnarized according to ti.usesFlatBuffer()
    // state that is set in columnarize_result function in
    // ColumnFetcher.cpp.
    if (src_ti.usesFlatBuffer() && ti.usesFlatBuffer()) {
      // If both source and target result sets use FlatBuffer layout,
      // creating a columnar result should be using zero-copy columnar
      // conversion.
      UNREACHABLE();
    } else if (ti.usesFlatBuffer()) {
      int64_t values_count = -1;
      switch (ti.get_type()) {
        case kARRAY:
          if (ti.get_subtype() == kTEXT && ti.get_compression() == kENCODING_NONE) {
            throw std::runtime_error(
                "Column<Array<TextEncodedNone>> support not implemented yet "
                "(ColumnarResults)");
          } else {
            values_count = computeTotalNofValuesForColumnArray(rows, i);
          }
          break;
        case kPOINT:
          values_count = num_rows_;
          break;
        case kLINESTRING:
          values_count =
              computeTotalNofValuesForColumnGeoType<GeoLineStringTargetValue,
                                                    GeoLineStringTargetValuePtr>(
                  rows, ti, i);
          break;
        case kPOLYGON:
          values_count =
              computeTotalNofValuesForColumnGeoType<GeoPolyTargetValue,
                                                    GeoPolyTargetValuePtr>(rows, ti, i);
          break;
        case kMULTIPOINT:
          values_count =
              computeTotalNofValuesForColumnGeoType<GeoMultiPointTargetValue,
                                                    GeoMultiPointTargetValuePtr>(
                  rows, ti, i);
          break;
        case kMULTILINESTRING:
          values_count =
              computeTotalNofValuesForColumnGeoType<GeoMultiLineStringTargetValue,
                                                    GeoMultiLineStringTargetValuePtr>(
                  rows, ti, i);
          break;
        case kMULTIPOLYGON:
          values_count =
              computeTotalNofValuesForColumnGeoType<GeoMultiPolyTargetValue,
                                                    GeoMultiPolyTargetValuePtr>(
                  rows, ti, i);
          break;
        case kTEXT:
          if (ti.get_compression() == kENCODING_NONE) {
            values_count = computeTotalNofValuesForColumnTextEncodingNone(rows, i);
            break;
          }
          if (ti.get_compression() == kENCODING_DICT) {
            values_count = num_rows_;
            break;
          }
        default:
          UNREACHABLE() << "computing number of values not implemented for "
                        << ti.toString();
      }
      // TODO: include sizes count to optimize flatbuffer size
      const int64_t flatbuffer_size = getFlatBufferSize(num_rows_, values_count, ti);
      VLOG(1) << "Allocate " << flatbuffer_size
              << " bytes for columnarized flat buffer of " << i
              << "-th column (# values: " << values_count << ")";
      column_buffers_[i] = row_set_mem_owner->allocate(flatbuffer_size, thread_idx_);
      FlatBufferManager m{column_buffers_[i]};
      initializeFlatBuffer(m, num_rows_, values_count, ti);
      // The column buffer will be initialized either directly or
      // through iteration.
      // TODO: implement QE-808 resolution here.
    } else {
      if (ti.is_varlen()) {
        throw ColumnarConversionNotSupported();
      }
      // The column buffer will be initialized either directly or
      // through iteration.
      auto const buf_size = num_rows_ * padded_target_sizes_[i];
      VLOG(1) << "Allocate " << buf_size << " bytes for columnarized buffer of " << i
              << "-th column (column size: " << padded_target_sizes_[i] << ")";
      column_buffers_[i] = row_set_mem_owner->allocate(buf_size, thread_idx_);
    }
  }

  if (isDirectColumnarConversionPossible() && rows.entryCount() > 0) {
    materializeAllColumnsDirectly(rows, num_columns);
  } else {
    materializeAllColumnsThroughIteration(rows, num_columns);
  }
}

ColumnarResults::ColumnarResults(std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
                                 const int8_t* one_col_buffer,
                                 const size_t num_rows,
                                 const SQLTypeInfo& target_type,
                                 const size_t executor_id,
                                 const size_t thread_idx)
    : column_buffers_(1)
    , direct_columnar_conversion_(false)
    , num_rows_(num_rows)
    , target_types_{target_type}
    , parallel_conversion_(false)
    , thread_idx_(thread_idx) {
  auto timer = DEBUG_TIMER(__func__);
  const bool is_varlen =
      target_type.is_array() ||
      (target_type.is_string() && target_type.get_compression() == kENCODING_NONE) ||
      target_type.is_geometry();
  if (is_varlen) {
    throw ColumnarConversionNotSupported();
  }
  VLOG(1) << "Columnarize resultset, # rows: " << num_rows_
          << ", direct columnarization? false, parallel conversion? false";
  executor_ = Executor::getExecutor(executor_id);
  padded_target_sizes_.emplace_back(target_type.get_size());
  CHECK(executor_);
  const auto buf_size = num_rows * target_type.get_size();
  VLOG(1) << "Allocate " << buf_size
          << " bytes for columnarized buffer of a column (column size: "
          << target_type.get_size() << ")";
  column_buffers_[0] =
      reinterpret_cast<int8_t*>(row_set_mem_owner->allocate(buf_size, thread_idx_));
  memcpy(((void*)column_buffers_[0]), one_col_buffer, buf_size);
}

std::unique_ptr<ColumnarResults> ColumnarResults::mergeResults(
    std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
    const std::vector<std::unique_ptr<ColumnarResults>>& sub_results) {
  auto timer = DEBUG_TIMER(__func__);
  // TODO: this method requires a safe guard when trying to merge
  // columns using FlatBuffer layout.
  if (sub_results.empty()) {
    return nullptr;
  }
  const auto total_row_count = std::accumulate(
      sub_results.begin(),
      sub_results.end(),
      size_t(0),
      [](const size_t init, const std::unique_ptr<ColumnarResults>& result) {
        return init + result->size();
      });
  std::unique_ptr<ColumnarResults> merged_results(
      new ColumnarResults(total_row_count,
                          sub_results[0]->target_types_,
                          sub_results[0]->padded_target_sizes_));
  const auto col_count = sub_results[0]->column_buffers_.size();
  const auto nonempty_it = std::find_if(
      sub_results.begin(),
      sub_results.end(),
      [](const std::unique_ptr<ColumnarResults>& needle) { return needle->size(); });
  if (nonempty_it == sub_results.end()) {
    return nullptr;
  }
  VLOG(1) << "Merge columnarized sub-resultsets, total # rows: " << total_row_count;
  for (size_t col_idx = 0; col_idx < col_count; ++col_idx) {
    const auto byte_width = merged_results->padded_target_sizes_[col_idx];
    auto const buf_size = byte_width * total_row_count;
    VLOG(1) << "Allocate " << buf_size << " bytes for columnarized buffer of " << col_idx
            << "-th column (column size: " << byte_width << ")";
    auto write_ptr = row_set_mem_owner->allocate(buf_size);
    merged_results->column_buffers_.push_back(write_ptr);
    for (auto& rs : sub_results) {
      CHECK_EQ(col_count, rs->column_buffers_.size());
      if (!rs->size()) {
        continue;
      }
      CHECK_EQ(byte_width, rs->padded_target_sizes_[col_idx]);
      memcpy(write_ptr, rs->column_buffers_[col_idx], rs->size() * byte_width);
      write_ptr += rs->size() * byte_width;
    }
  }
  return merged_results;
}

/**
 * This function iterates through the result set (using the getRowAtNoTranslation and
 * getNextRow family of functions) and writes back the results into output column buffers.
 */
void ColumnarResults::materializeAllColumnsThroughIteration(const ResultSet& rows,
                                                            const size_t num_columns) {
  auto timer = DEBUG_TIMER(__func__);
  if (!rows.isEmpty()) {
    for (size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
      CHECK(column_buffers_[col_idx])
          << "Columnarization buffer for " << col_idx << "-th column is not initialized";
    }
  }
  if (isParallelConversion()) {
    std::atomic<size_t> row_idx{0};
    const size_t worker_count = cpu_threads();
    std::vector<std::future<void>> conversion_threads;
    std::mutex write_mutex;
    const auto do_work =
        [num_columns, &rows, &row_idx, &write_mutex, this](const size_t i) {
          const auto crt_row = rows.getRowAtNoTranslations(i);
          if (!crt_row.empty()) {
            auto cur_row_idx = row_idx.fetch_add(1);
            for (size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
              auto& type_info = target_types_[col_idx];
              writeBackCell(crt_row[col_idx],
                            cur_row_idx,
                            type_info,
                            column_buffers_[col_idx],
                            &write_mutex);
            }
          }
        };
    for (auto interval : makeIntervals(size_t(0), rows.entryCount(), worker_count)) {
      conversion_threads.push_back(std::async(
          std::launch::async,
          [&do_work, this](const size_t start, const size_t end) {
            if (g_enable_non_kernel_time_query_interrupt) {
              size_t local_idx = 0;
              for (size_t i = start; i < end; ++i, ++local_idx) {
                checkInterruption(local_idx);
                do_work(i);
              }
            } else {
              for (size_t i = start; i < end; ++i) {
                do_work(i);
              }
            }
          },
          interval.begin,
          interval.end));
    }

    try {
      for (auto& child : conversion_threads) {
        child.wait();
      }
    } catch (QueryExecutionError& e) {
      if (e.hasErrorCode(ErrorCode::INTERRUPTED)) {
        throw QueryExecutionError(ErrorCode::INTERRUPTED);
      }
      throw e;
    } catch (...) {
      throw;
    }

    num_rows_ = row_idx;
    rows.setCachedRowCount(num_rows_);
    return;
  }
  bool done = false;
  size_t row_idx = 0;
  const auto do_work = [num_columns, &row_idx, &rows, &done, this]() {
    const auto crt_row = rows.getNextRow(false, false);
    if (crt_row.empty()) {
      done = true;
      return;
    }
    for (size_t i = 0; i < num_columns; ++i) {
      auto& type_info = target_types_[i];
      writeBackCell(crt_row[i], row_idx, type_info, column_buffers_[i]);
    }
    ++row_idx;
  };
  if (g_enable_non_kernel_time_query_interrupt) {
    while (!done) {
      checkInterruption(row_idx);
      do_work();
    }
  } else {
    while (!done) {
      do_work();
    }
  }

  rows.moveToBegin();
}

template <size_t NDIM,
          typename GeospatialGeoType,
          typename GeoTypeTargetValue,
          typename GeoTypeTargetValuePtr,
          bool is_multi>
void writeBackCellGeoNestedArray(FlatBufferManager& m,
                                 const int64_t index,
                                 const SQLTypeInfo& ti,
                                 const TargetValue& col_val,
                                 std::mutex* write_mutex) {
  const SQLTypeInfoLite* ti_lite =
      reinterpret_cast<const SQLTypeInfoLite*>(m.get_user_data_buffer());
  CHECK(ti_lite);
  if (ti_lite->is_geoint()) {
    CHECK_EQ(ti.get_compression(), kENCODING_GEOINT);
  } else {
    CHECK_EQ(ti.get_compression(), kENCODING_NONE);
  }
  FlatBufferManager::Status status{};
  if (const auto tv = boost::get<ScalarTargetValue>(&col_val)) {
    const auto ns = boost::get<NullableString>(tv);
    CHECK(ns);
    const auto s_ptr = boost::get<std::string>(ns);
    if (s_ptr == nullptr || *s_ptr == "NULL") {
      auto lock_scope =
          (write_mutex == nullptr ? std::unique_lock<std::mutex>()
                                  : std::unique_lock<std::mutex>(*write_mutex));
      status = m.setNull(index);
    } else {
      std::vector<double> coords;
      std::vector<double> bounds;
      std::vector<int32_t> ring_sizes;
      std::vector<int32_t> poly_rings;
      int64_t approx_nof_coords = 2 * std::count(s_ptr->begin(), s_ptr->end(), ',');
      coords.reserve(approx_nof_coords);
      bounds.reserve(4);
      const auto gdal_wkt_ls = GeospatialGeoType(*s_ptr);
      if constexpr (NDIM == 1) {
        gdal_wkt_ls.getColumns(coords, bounds);
      } else if constexpr (NDIM == 2) {
        int64_t approx_nof_rings = std::count(s_ptr->begin(), s_ptr->end(), '(') - 1;
        ring_sizes.reserve(approx_nof_rings);
        gdal_wkt_ls.getColumns(coords, ring_sizes, bounds);
      } else if constexpr (NDIM == 3) {
        int64_t approx_nof_rings = std::count(s_ptr->begin(), s_ptr->end(), '(') - 1;
        ring_sizes.reserve(approx_nof_rings);
        poly_rings.reserve(approx_nof_rings);
        gdal_wkt_ls.getColumns(coords, ring_sizes, poly_rings, bounds);
      } else {
        UNREACHABLE();
      }
      const std::vector<uint8_t> compressed_coords =
          Geospatial::compress_coords(coords, ti);
      {
        auto lock_scope =
            (write_mutex == nullptr ? std::unique_lock<std::mutex>()
                                    : std::unique_lock<std::mutex>(*write_mutex));
        if constexpr (NDIM == 1) {
          status = m.setItem(index, compressed_coords);
        } else if constexpr (NDIM == 2) {
          status = m.setItem(index, compressed_coords, ring_sizes);
        } else if constexpr (NDIM == 3) {
          status = m.setItem(index, compressed_coords, ring_sizes, poly_rings);
        } else {
          UNREACHABLE();
        }
      }
    }
  } else if (const auto tv = boost::get<GeoTargetValuePtr>(&col_val)) {
    const auto s = boost::get<GeoTypeTargetValuePtr>(tv);
    CHECK(s);
    if (s->coords_data == nullptr || s->coords_data->pointer == nullptr) {
      status = m.setNull(index);
    } else {
      const VarlenDatum* d = s->coords_data.get();
      CHECK(d);
      CHECK(d->pointer);

      int32_t nof_values =
          d->length / (ti_lite->is_geoint() ? 2 * sizeof(int32_t) : 2 * sizeof(double));
      {
        auto lock_scope =
            (write_mutex == nullptr ? std::unique_lock<std::mutex>()
                                    : std::unique_lock<std::mutex>(*write_mutex));
        if constexpr (NDIM == 1) {
          status = m.setItem<0, false>(index, d->pointer, nof_values);
        } else if constexpr (NDIM == 2) {
          VarlenDatum* r = nullptr;
          if constexpr (is_multi) {
            r = s->linestring_sizes_data.get();
          } else {
            r = s->ring_sizes_data.get();
          }
          status = m.setItem<1, /*check_sizes=*/false>(
              index,
              d->pointer,
              nof_values,
              reinterpret_cast<const int32_t*>(r->pointer),
              r->length / sizeof(int32_t));
        } else if constexpr (NDIM == 3) {
          const VarlenDatum* r = s->ring_sizes_data.get();
          const VarlenDatum* p = s->poly_rings_data.get();
          status = m.setItem<2, /*check_sizes=*/false>(
              index,
              d->pointer,
              nof_values,
              reinterpret_cast<const int32_t*>(r->pointer),
              r->length / sizeof(int32_t),
              reinterpret_cast<const int32_t*>(p->pointer),
              p->length / sizeof(int32_t));
        } else {
          UNREACHABLE();
        }
      }
    }
  } else if (const auto tv = boost::get<GeoTargetValue>(&col_val)) {
    if (tv->get_ptr() == nullptr) {
      auto lock_scope =
          (write_mutex == nullptr ? std::unique_lock<std::mutex>()
                                  : std::unique_lock<std::mutex>(*write_mutex));
      status = m.setNull(index);
    } else {
      const auto s = boost::get<GeoTypeTargetValue>(tv->get());
      const std::vector<double>* d = s.coords.get();
      const std::vector<int32_t>* r = nullptr;
      const std::vector<int32_t>* p = nullptr;
      if constexpr (NDIM == 1) {
        CHECK(r == nullptr);
        CHECK(p == nullptr);
      } else if constexpr (NDIM == 2) {
        if constexpr (is_multi) {
          r = s.linestring_sizes.get();
        } else {
          r = s.ring_sizes.get();
        }
        CHECK(p == nullptr);
      } else if constexpr (NDIM == 3) {
        r = s.ring_sizes.get();
        p = s.poly_rings.get();
      } else {
        UNREACHABLE();
      }
      CHECK(d);
      CHECK_NE(d->size(), 0);
      std::vector<uint8_t> compressed_coords = Geospatial::compress_coords(*d, ti);
      {
        auto lock_scope =
            (write_mutex == nullptr ? std::unique_lock<std::mutex>()
                                    : std::unique_lock<std::mutex>(*write_mutex));
        if constexpr (NDIM == 1) {
          status = m.setItem(index, compressed_coords);
        } else if constexpr (NDIM == 2) {
          status = m.setItem(index, compressed_coords, *r);
        } else if constexpr (NDIM == 3) {
          status = m.setItem(index, compressed_coords, *r, *p);
        } else {
          UNREACHABLE();
        }
      }
    }
  } else {
    UNREACHABLE();
  }
  CHECK_EQ(status, FlatBufferManager::Status::Success);
}

template <typename scalar_type, typename value_type>
void writeBackCellArrayScalar(FlatBufferManager& m,
                              const size_t row_idx,
                              const TargetValue& col_val,
                              std::mutex* write_mutex) {
  FlatBufferManager::Status status{};
  const auto arr_tv = boost::get<ArrayTargetValue>(&col_val);
  if (arr_tv->is_initialized()) {
    const auto& vec = arr_tv->get();
    // add a new item to flatbuffer, no initialization
    {
      auto lock_scope =
          (write_mutex == nullptr ? std::unique_lock<std::mutex>()
                                  : std::unique_lock<std::mutex>(*write_mutex));
      status = m.setItem<1, false>(row_idx, nullptr, vec.size());
    }
    CHECK_EQ(status, FlatBufferManager::Status::Success);
    FlatBufferManager::NestedArrayItem<1> item;
    // retrieve the item
    status = m.getItem(row_idx, item);
    CHECK_EQ(status, FlatBufferManager::Status::Success);
    CHECK_EQ(item.nof_sizes, 0);            // for sanity
    CHECK_EQ(item.nof_values, vec.size());  // for sanity
    // initialize the item's buffer
    scalar_type* values = reinterpret_cast<scalar_type*>(item.values);
    size_t index = 0;
    for (const TargetValue val : vec) {
      const auto& scalar_val = boost::get<ScalarTargetValue>(&val);
      values[index++] = static_cast<scalar_type>(*boost::get<value_type>(scalar_val));
    }
  } else {
    // add a new NULL item to flatbuffer
    {
      auto lock_scope =
          (write_mutex == nullptr ? std::unique_lock<std::mutex>()
                                  : std::unique_lock<std::mutex>(*write_mutex));
      status = m.setNull(row_idx);
    }
    CHECK_EQ(status, FlatBufferManager::Status::Success);
  }
}

inline void writeBackCellTextEncodingNone(FlatBufferManager& m,
                                          const size_t row_idx,
                                          const TargetValue& col_val,
                                          std::mutex* write_mutex) {
  FlatBufferManager::Status status{};
  if (const auto tv = boost::get<ScalarTargetValue>(&col_val)) {
    const auto ns = boost::get<NullableString>(tv);
    CHECK(ns);
    const auto s_ptr = boost::get<std::string>(ns);
    {
      auto lock_scope =
          (write_mutex == nullptr ? std::unique_lock<std::mutex>()
                                  : std::unique_lock<std::mutex>(*write_mutex));
      if (s_ptr) {
        status = m.setItem(row_idx, *s_ptr);
      } else {
        status = m.setNull(row_idx);
      }
    }
    CHECK_EQ(status, FlatBufferManager::Status::Success);
  } else {
    UNREACHABLE();
  }
}

inline void writeBackCellGeoPoint(FlatBufferManager& m,
                                  const size_t row_idx,
                                  const SQLTypeInfo& type_info,
                                  const TargetValue& col_val,
                                  std::mutex* write_mutex) {
  FlatBufferManager::Status status{};
  // to be deprecated, this function uses old FlatBuffer API
  if (const auto tv = boost::get<ScalarTargetValue>(&col_val)) {
    const auto ns = boost::get<NullableString>(tv);
    CHECK(ns);
    const auto s_ptr = boost::get<std::string>(ns);
    std::vector<double> coords;
    coords.reserve(2);
    if (s_ptr == nullptr) {
      coords.push_back(NULL_ARRAY_DOUBLE);
      coords.push_back(NULL_ARRAY_DOUBLE);
    } else {
      const auto gdal_wkt_pt = Geospatial::GeoPoint(*s_ptr);
      gdal_wkt_pt.getColumns(coords);
      CHECK_EQ(coords.size(), 2);
    }
    std::vector<std::uint8_t> data = Geospatial::compress_coords(coords, type_info);
    {
      auto lock_scope =
          (write_mutex == nullptr ? std::unique_lock<std::mutex>()
                                  : std::unique_lock<std::mutex>(*write_mutex));
      status = m.setItemOld(
          row_idx, reinterpret_cast<const int8_t*>(data.data()), data.size());
    }
    CHECK_EQ(status, FlatBufferManager::Status::Success);
  } else if (const auto tv = boost::get<GeoTargetValuePtr>(&col_val)) {
    const auto s = boost::get<GeoPointTargetValuePtr>(tv);
    CHECK(s);
    VarlenDatum* d = s->coords_data.get();
    CHECK(d);
    CHECK_EQ(type_info.get_compression() == kENCODING_GEOINT,
             m.getGeoPointMetadata()->is_geoint);
    {
      auto lock_scope =
          (write_mutex == nullptr ? std::unique_lock<std::mutex>()
                                  : std::unique_lock<std::mutex>(*write_mutex));
      status =
          m.setItemOld(row_idx, reinterpret_cast<const int8_t*>(d->pointer), d->length);
    }
    CHECK_EQ(status, FlatBufferManager::Status::Success);
  } else if (const auto tv = boost::get<GeoTargetValue>(&col_val)) {
    /*
      Warning: the following code fails for NULL row values
      because of the failure to detect the nullness correctly.
    */
    const auto s = boost::get<GeoPointTargetValue>(tv->get());
    const std::vector<double>* d = s.coords.get();
    CHECK_EQ(d->size(), 2);
    {
      auto lock_scope =
          (write_mutex == nullptr ? std::unique_lock<std::mutex>()
                                  : std::unique_lock<std::mutex>(*write_mutex));
      status = m.setItemOld(
          row_idx, reinterpret_cast<const int8_t*>(d->data()), m.dtypeSize());
    }
    CHECK_EQ(d->size(), 2);
    CHECK_EQ(status, FlatBufferManager::Status::Success);
  } else {
    UNREACHABLE();
  }
}

/*
 * This function processes and decodes its input TargetValue
 * and write it into its corresponding column buffer's cell (with corresponding
 * row and column indices)
 *
 * NOTE: this is not supposed to be processing varlen types (except
 * FlatBuffer supported types such as Array, GeoPoint, etc), and they
 * should be handled differently outside this function. TODO: QE-808.
 */

inline void ColumnarResults::writeBackCell(const TargetValue& col_val,
                                           const size_t row_idx,
                                           const SQLTypeInfo& type_info,
                                           int8_t* column_buf,
                                           std::mutex* write_mutex) {
  if (!type_info.usesFlatBuffer()) {
    toBuffer(col_val, type_info, column_buf + type_info.get_size() * row_idx);
    return;
  }
  CHECK(FlatBufferManager::isFlatBuffer(column_buf));
  FlatBufferManager m{column_buf};
  if (type_info.is_geometry() && type_info.get_type() == kPOINT) {
    writeBackCellGeoPoint(m, row_idx, type_info, col_val, write_mutex);
    return;
  }
  const SQLTypeInfoLite* ti_lite =
      reinterpret_cast<const SQLTypeInfoLite*>(m.get_user_data_buffer());
  CHECK(ti_lite);
  if (type_info.is_array()) {
    if (type_info.get_subtype() == kTEXT &&
        type_info.get_compression() == kENCODING_NONE) {
      throw std::runtime_error(
          "Column<Array<TextEncodedNone>> support not implemented yet (writeBackCell)");
    }
    switch (ti_lite->subtype) {
      case SQLTypeInfoLite::DOUBLE:
        writeBackCellArrayScalar<double, double>(m, row_idx, col_val, write_mutex);
        break;
      case SQLTypeInfoLite::FLOAT:
        writeBackCellArrayScalar<float, float>(m, row_idx, col_val, write_mutex);
        break;
      case SQLTypeInfoLite::BOOLEAN:
      case SQLTypeInfoLite::TINYINT:
        writeBackCellArrayScalar<int8_t, int64_t>(m, row_idx, col_val, write_mutex);
        break;
      case SQLTypeInfoLite::SMALLINT:
        writeBackCellArrayScalar<int16_t, int64_t>(m, row_idx, col_val, write_mutex);
        break;
      case SQLTypeInfoLite::INT:
      case SQLTypeInfoLite::TEXT:
        writeBackCellArrayScalar<int32_t, int64_t>(m, row_idx, col_val, write_mutex);
        break;
      case SQLTypeInfoLite::BIGINT:
        writeBackCellArrayScalar<int64_t, int64_t>(m, row_idx, col_val, write_mutex);
        break;
      default:
        UNREACHABLE();
    }
  } else if (type_info.is_text_encoding_none()) {
    writeBackCellTextEncodingNone(m, row_idx, col_val, write_mutex);
  } else if (type_info.is_geometry()) {
    switch (type_info.get_type()) {
      case kLINESTRING: {
        writeBackCellGeoNestedArray<1,
                                    Geospatial::GeoLineString,
                                    GeoLineStringTargetValue,
                                    GeoLineStringTargetValuePtr,
                                    /*is_multi=*/false>(
            m, row_idx, type_info, col_val, write_mutex);
        break;
      }
      case kPOLYGON: {
        writeBackCellGeoNestedArray<2,
                                    Geospatial::GeoPolygon,
                                    GeoPolyTargetValue,
                                    GeoPolyTargetValuePtr,
                                    /*is_multi=*/false>(
            m, row_idx, type_info, col_val, write_mutex);
        break;
      }
      case kMULTIPOINT: {
        writeBackCellGeoNestedArray<1,
                                    Geospatial::GeoMultiPoint,
                                    GeoMultiPointTargetValue,
                                    GeoMultiPointTargetValuePtr,
                                    /*is_multi=*/true>(
            m, row_idx, type_info, col_val, write_mutex);
        break;
      }
      case kMULTILINESTRING: {
        writeBackCellGeoNestedArray<2,
                                    Geospatial::GeoMultiLineString,
                                    GeoMultiLineStringTargetValue,
                                    GeoMultiLineStringTargetValuePtr,
                                    /*is_multi=*/true>(
            m, row_idx, type_info, col_val, write_mutex);
        break;
      }
      case kMULTIPOLYGON: {
        writeBackCellGeoNestedArray<3,
                                    Geospatial::GeoMultiPolygon,
                                    GeoMultiPolyTargetValue,
                                    GeoMultiPolyTargetValuePtr,
                                    /*is_true=*/false>(
            m, row_idx, type_info, col_val, write_mutex);
        break;
      }
      default:
        UNREACHABLE() << "writeBackCell not implemented for " << type_info.toString();
    }
  } else {
    UNREACHABLE();
  }
}

/**
 * A set of write functions to be used to directly write into final column_buffers_.
 * The read_from_function is used to read from the input result set's storage
 * NOTE: currently only used for direct columnarizations
 */
template <typename DATA_TYPE>
void ColumnarResults::writeBackCellDirect(const ResultSet& rows,
                                          const size_t input_buffer_entry_idx,
                                          const size_t output_buffer_entry_idx,
                                          const size_t target_idx,
                                          const size_t slot_idx,
                                          const ReadFunction& read_from_function) {
  const auto val = static_cast<DATA_TYPE>(fixed_encoding_nullable_val(
      read_from_function(rows, input_buffer_entry_idx, target_idx, slot_idx),
      target_types_[target_idx]));
  reinterpret_cast<DATA_TYPE*>(column_buffers_[target_idx])[output_buffer_entry_idx] =
      val;
}

template <>
void ColumnarResults::writeBackCellDirect<float>(const ResultSet& rows,
                                                 const size_t input_buffer_entry_idx,
                                                 const size_t output_buffer_entry_idx,
                                                 const size_t target_idx,
                                                 const size_t slot_idx,
                                                 const ReadFunction& read_from_function) {
  const int32_t ival =
      read_from_function(rows, input_buffer_entry_idx, target_idx, slot_idx);
  const float fval = *reinterpret_cast<const float*>(may_alias_ptr(&ival));
  reinterpret_cast<float*>(column_buffers_[target_idx])[output_buffer_entry_idx] = fval;
}

template <>
void ColumnarResults::writeBackCellDirect<double>(
    const ResultSet& rows,
    const size_t input_buffer_entry_idx,
    const size_t output_buffer_entry_idx,
    const size_t target_idx,
    const size_t slot_idx,
    const ReadFunction& read_from_function) {
  const int64_t ival =
      read_from_function(rows, input_buffer_entry_idx, target_idx, slot_idx);
  const double dval = *reinterpret_cast<const double*>(may_alias_ptr(&ival));
  reinterpret_cast<double*>(column_buffers_[target_idx])[output_buffer_entry_idx] = dval;
}

/**
 * This function materializes all columns from the main storage and all appended storages
 * and form a single continguous column for each output column. Depending on whether the
 * column is lazily fetched or not, it will treat them differently.
 *
 * NOTE: this function should
 * only be used when the result set is columnar and completely compacted (e.g., in
 * columnar projections).
 */
void ColumnarResults::materializeAllColumnsDirectly(const ResultSet& rows,
                                                    const size_t num_columns) {
  auto timer = DEBUG_TIMER(__func__);
  CHECK(isDirectColumnarConversionPossible());
  switch (rows.getQueryDescriptionType()) {
    case QueryDescriptionType::Projection: {
      materializeAllColumnsProjection(rows, num_columns);
      break;
    }
    case QueryDescriptionType::TableFunction: {
      materializeAllColumnsTableFunction(rows, num_columns);
      break;
    }
    case QueryDescriptionType::GroupByPerfectHash:
    case QueryDescriptionType::GroupByBaselineHash: {
      materializeAllColumnsGroupBy(rows, num_columns);
      break;
    }
    default:
      UNREACHABLE()
          << "Direct columnar conversion for this query type is not supported yet.";
  }
}

/**
 * This function handles materialization for two types of columns in columnar projections:
 * 1. for all non-lazy columns, it directly copies the results from the result set's
 * storage into the output column buffers
 * 2. for all lazy fetched columns, it uses result set's iterators to decode the proper
 * values before storing them into the output column buffers
 */

void ColumnarResults::materializeAllColumnsProjection(const ResultSet& rows,
                                                      const size_t num_columns) {
  auto timer = DEBUG_TIMER(__func__);
  CHECK(rows.query_mem_desc_.didOutputColumnar());
  CHECK(isDirectColumnarConversionPossible() &&
        (rows.query_mem_desc_.getQueryDescriptionType() ==
         QueryDescriptionType::Projection));

  const auto& lazy_fetch_info = rows.getLazyFetchInfo();

  // We can directly copy each non-lazy column's content
  copyAllNonLazyColumns(lazy_fetch_info, rows, num_columns);

  materializeAllLazyColumns(lazy_fetch_info, rows, num_columns);
}

void ColumnarResults::materializeAllColumnsTableFunction(const ResultSet& rows,
                                                         const size_t num_columns) {
  CHECK(rows.query_mem_desc_.didOutputColumnar());
  CHECK(isDirectColumnarConversionPossible() &&
        (rows.query_mem_desc_.getQueryDescriptionType() ==
         QueryDescriptionType::TableFunction));

  const auto& lazy_fetch_info = rows.getLazyFetchInfo();
  // Lazy fetching is not currently allowed for table function outputs
  for (const auto& col_lazy_fetch_info : lazy_fetch_info) {
    CHECK(!col_lazy_fetch_info.is_lazily_fetched);
  }
  // We can directly copy each non-lazy column's content
  copyAllNonLazyColumns(lazy_fetch_info, rows, num_columns);
}

/*
 * For all non-lazy columns, we can directly copy back the results of each column's
 * contents from different storages and put them into the corresponding output buffer.
 *
 * This function is parallelized through assigning each column to a CPU thread.
 */
void ColumnarResults::copyAllNonLazyColumns(
    const std::vector<ColumnLazyFetchInfo>& lazy_fetch_info,
    const ResultSet& rows,
    const size_t num_columns) {
  auto timer = DEBUG_TIMER(__func__);

  CHECK(isDirectColumnarConversionPossible());
  const auto is_column_non_lazily_fetched = [&lazy_fetch_info](const size_t col_idx) {
    // Saman: make sure when this lazy_fetch_info is empty
    if (lazy_fetch_info.empty()) {
      return true;
    } else {
      return !lazy_fetch_info[col_idx].is_lazily_fetched;
    }
  };

  // parallelized by assigning each column to a thread
  std::vector<std::future<void>> direct_copy_threads;
  for (size_t col_idx = 0; col_idx < num_columns; col_idx++) {
    if (rows.isZeroCopyColumnarConversionPossible(col_idx)) {
      CHECK(!column_buffers_[col_idx]);
      // The name of the method implies a copy but this is not a copy!!
      column_buffers_[col_idx] = const_cast<int8_t*>(rows.getColumnarBuffer(col_idx));
    } else if (is_column_non_lazily_fetched(col_idx)) {
      CHECK(!(rows.query_mem_desc_.getQueryDescriptionType() ==
              QueryDescriptionType::TableFunction));
      if (rows.getColType(col_idx).usesFlatBuffer() &&
          target_types_[col_idx].usesFlatBuffer()) {
        // If both source and target result sets use FlatBuffer
        // layout, creating a columnar result should be using
        // zero-copy columnar conversion.
        UNREACHABLE();
      }
      direct_copy_threads.push_back(std::async(
          std::launch::async,
          [&rows, this](const size_t column_index) {
            size_t column_size = rows.getColumnarBufferSize(column_index);
            rows.copyColumnIntoBuffer(
                column_index, column_buffers_[column_index], column_size);
          },
          col_idx));
    }
  }

  for (auto& child : direct_copy_threads) {
    child.wait();
  }
}

namespace {

bool has_lazy_fetched_column(const std::vector<ColumnLazyFetchInfo>& lazy_fetch_info) {
  return std::any_of(lazy_fetch_info.begin(),
                     lazy_fetch_info.end(),
                     [](const auto& col_info) { return col_info.is_lazily_fetched; });
}

bool has_lazy_fetched_flat_buffer_column(
    const std::vector<ColumnLazyFetchInfo>& lazy_fetch_info,
    const ResultSet& rows,
    const std::vector<SQLTypeInfo>& target_types,
    const size_t num_columns) {
  for (size_t col_idx = 0; col_idx < num_columns; col_idx++) {
    if (lazy_fetch_info[col_idx].is_lazily_fetched) {
      if (rows.getColType(col_idx).usesFlatBuffer() ||
          target_types[col_idx].usesFlatBuffer()) {
        return true;
      }
    }
  }
  return false;
}

std::vector<bool> generate_targets_to_skip(
    const ResultSet& rows,
    const std::vector<ColumnLazyFetchInfo>& lazy_fetch_info,
    const size_t num_columns) {
  std::vector<bool> targets_to_skip;
  if (rows.isPermutationBufferEmpty()) {
    targets_to_skip.resize(num_columns);
    for (size_t i = 0; i < num_columns; i++) {
      targets_to_skip[i] = !lazy_fetch_info[i].is_lazily_fetched;
    }
  }
  return targets_to_skip;
}

std::vector<size_t> generate_lazy_fetched_column_indices(
    const std::vector<ColumnLazyFetchInfo>& lazy_fetch_info) {
  std::vector<size_t> lazy_fetched_indices;
  for (size_t i = 0; i < lazy_fetch_info.size(); ++i) {
    if (lazy_fetch_info[i].is_lazily_fetched) {
      lazy_fetched_indices.push_back(i);
    }
  }
  return lazy_fetched_indices;
}

}  // anonymous namespace

/**
 * For all lazy fetched columns, we should iterate through the column's content and
 * properly materialize it.
 *
 * This function is parallelized through dividing total rows among all existing threads.
 * Since there's no invalid element in the result set (e.g., columnar projections), the
 * output buffer will have as many rows as there are in the result set, removing the need
 * for atomicly incrementing the output buffer position.
 */

void ColumnarResults::materializeAllLazyColumns(
    const std::vector<ColumnLazyFetchInfo>& lazy_fetch_info,
    const ResultSet& rows,
    const size_t num_columns) {
  auto timer = DEBUG_TIMER(__func__);
  CHECK(isDirectColumnarConversionPossible());
  CHECK(!(rows.query_mem_desc_.getQueryDescriptionType() ==
          QueryDescriptionType::TableFunction));

  if (!has_lazy_fetched_column(lazy_fetch_info)) {
    return;
  }

  const size_t worker_count =
      result_set::use_parallel_algorithms(rows) ? cpu_threads() : 1;
  std::vector<std::future<void>> conversion_threads;

  const bool has_lazy_fetched_flat_buffer_col = has_lazy_fetched_flat_buffer_column(
      lazy_fetch_info, rows, target_types_, num_columns);

  if (has_lazy_fetched_flat_buffer_col) {
    // Use the slower logic for flat buffers
    const auto targets_to_skip =
        generate_targets_to_skip(rows, lazy_fetch_info, num_columns);

    const auto lazy_fetched_indices =
        generate_lazy_fetched_column_indices(lazy_fetch_info);

    std::mutex write_mutex;
    const auto do_work = [this,
                          &rows,
                          &write_mutex,
                          &lazy_fetched_indices,
                          &targets_to_skip](const size_t start, const size_t end) {
      for (size_t row_idx = start; row_idx < end; ++row_idx) {
        const auto crt_row = rows.getRowAtNoTranslations(row_idx, targets_to_skip);
        for (const auto i : lazy_fetched_indices) {
          writeBackCell(
              crt_row[i], row_idx, target_types_[i], column_buffers_[i], &write_mutex);
        }
        checkInterruption(row_idx);
      }
    };

    for (auto interval : makeIntervals(size_t(0), rows.entryCount(), worker_count)) {
      conversion_threads.push_back(
          std::async(std::launch::async, do_work, interval.begin, interval.end));
    }
    try {
      for (auto& child : conversion_threads) {
        child.wait();
      }
    } catch (QueryExecutionError& e) {
      if (e.hasErrorCode(ErrorCode::INTERRUPTED)) {
        throw QueryExecutionError(ErrorCode::INTERRUPTED);
      }
      throw;
    } catch (...) {
      throw;
    }
  } else {
    // Use the faster logic for non-flat buffers
    const auto do_work = [this, &rows, &lazy_fetch_info, num_columns](const size_t start,
                                                                      const size_t end) {
      for (size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
        if (lazy_fetch_info[col_idx].is_lazily_fetched) {
          const auto& type_info = target_types_[col_idx];
          const size_t col_width = type_info.get_size();
          const auto sql_type = type_info.get_type();
          int8_t* col_buffer = column_buffers_[col_idx];
          switch (sql_type) {
            case SQLTypes::kBOOLEAN:
              fetchAndCheckInterruption<bool>(
                  start, end, col_idx, col_width, col_buffer, rows);
              break;
            case SQLTypes::kTINYINT:
              fetchAndCheckInterruption<int8_t>(
                  start, end, col_idx, col_width, col_buffer, rows);
              break;
            case SQLTypes::kSMALLINT:
              fetchAndCheckInterruption<int16_t>(
                  start, end, col_idx, col_width, col_buffer, rows);
              break;
            case SQLTypes::kINT:
              fetchAndCheckInterruption<int32_t>(
                  start, end, col_idx, col_width, col_buffer, rows);
              break;
            case SQLTypes::kBIGINT:
            case SQLTypes::kNUMERIC:
            case SQLTypes::kDECIMAL:
            case SQLTypes::kTIMESTAMP:
            case SQLTypes::kDATE:
            case SQLTypes::kTIME:
            case SQLTypes::kINTERVAL_DAY_TIME:
            case SQLTypes::kINTERVAL_YEAR_MONTH:
              fetchAndCheckInterruption<int64_t>(
                  start, end, col_idx, col_width, col_buffer, rows);
              break;
            case SQLTypes::kFLOAT:
              fetchAndCheckInterruption<float>(
                  start, end, col_idx, col_width, col_buffer, rows);
              break;
            case SQLTypes::kDOUBLE:
              fetchAndCheckInterruption<double>(
                  start, end, col_idx, col_width, col_buffer, rows);
              break;
            case SQLTypes::kTEXT:
            case SQLTypes::kVARCHAR:
            case SQLTypes::kCHAR:
              if (type_info.get_compression() == kENCODING_DICT) {
                fetchAndCheckInterruption<int32_t>(
                    start, end, col_idx, col_width, col_buffer, rows);
              } else {
                throw std::runtime_error("Unsupported non-encoded TEXT type");
              }
              break;
            default:
              throw std::runtime_error("Unsupported column type: " +
                                       std::to_string(static_cast<int>(sql_type)));
          }
        }
      }
    };

    for (auto interval : makeIntervals(size_t(0), rows.entryCount(), worker_count)) {
      conversion_threads.push_back(
          std::async(std::launch::async, do_work, interval.begin, interval.end));
    }
    try {
      for (auto& child : conversion_threads) {
        child.wait();
      }
    } catch (QueryExecutionError& e) {
      if (e.hasErrorCode(ErrorCode::INTERRUPTED)) {
        throw QueryExecutionError(ErrorCode::INTERRUPTED);
      }
      throw;
    } catch (...) {
      throw;
    }
  }
}

/**
 * This function is to directly columnarize a result set for group by queries.
 * Its main difference with the traditional alternative is that it directly reads
 * non-empty entries from the result set, and then writes them into output column buffers,
 * rather than using the result set's iterators.
 */
void ColumnarResults::materializeAllColumnsGroupBy(const ResultSet& rows,
                                                   const size_t num_columns) {
  auto timer = DEBUG_TIMER(__func__);
  CHECK(isDirectColumnarConversionPossible());
  CHECK(rows.getQueryDescriptionType() == QueryDescriptionType::GroupByPerfectHash ||
        rows.getQueryDescriptionType() == QueryDescriptionType::GroupByBaselineHash);

  const size_t num_threads = isParallelConversion() ? cpu_threads() : 1;
  const size_t entry_count = rows.entryCount();
  const size_t size_per_thread = (entry_count + num_threads - 1) / num_threads;

  // step 1: compute total non-empty elements and store a bitmap per thread
  std::vector<size_t> non_empty_per_thread(num_threads,
                                           0);  // number of non-empty entries per thread

  ColumnBitmap bitmap(size_per_thread, num_threads);

  locateAndCountEntries(
      rows, bitmap, non_empty_per_thread, entry_count, num_threads, size_per_thread);

  // step 2: go through the generated bitmap and copy/decode corresponding entries
  // into the output buffer
  compactAndCopyEntries(rows,
                        bitmap,
                        non_empty_per_thread,
                        num_columns,
                        entry_count,
                        num_threads,
                        size_per_thread);
}

/**
 * This function goes through all the keys in the result set, and count the total number
 * of non-empty keys. It also store the location of non-empty keys in a bitmap data
 * structure for later faster access.
 */
void ColumnarResults::locateAndCountEntries(const ResultSet& rows,
                                            ColumnBitmap& bitmap,
                                            std::vector<size_t>& non_empty_per_thread,
                                            const size_t entry_count,
                                            const size_t num_threads,
                                            const size_t size_per_thread) const {
  auto timer = DEBUG_TIMER(__func__);
  CHECK(isDirectColumnarConversionPossible());
  CHECK(rows.getQueryDescriptionType() == QueryDescriptionType::GroupByPerfectHash ||
        rows.getQueryDescriptionType() == QueryDescriptionType::GroupByBaselineHash);
  CHECK_EQ(num_threads, non_empty_per_thread.size());
  auto do_work = [&rows, &bitmap](size_t& total_non_empty,
                                  const size_t local_idx,
                                  const size_t entry_idx,
                                  const size_t thread_idx) {
    if (!rows.isRowAtEmpty(entry_idx)) {
      total_non_empty++;
      bitmap.set(local_idx, thread_idx, true);
    }
  };
  auto locate_and_count_func =
      [&do_work, &non_empty_per_thread, this](
          size_t start_index, size_t end_index, size_t thread_idx) {
        size_t total_non_empty = 0;
        size_t local_idx = 0;
        if (g_enable_non_kernel_time_query_interrupt) {
          for (size_t entry_idx = start_index; entry_idx < end_index;
               entry_idx++, local_idx++) {
            checkInterruption(local_idx);
            do_work(total_non_empty, local_idx, entry_idx, thread_idx);
          }
        } else {
          for (size_t entry_idx = start_index; entry_idx < end_index;
               entry_idx++, local_idx++) {
            do_work(total_non_empty, local_idx, entry_idx, thread_idx);
          }
        }
        non_empty_per_thread[thread_idx] = total_non_empty;
      };

  std::vector<std::future<void>> conversion_threads;
  for (size_t thread_idx = 0; thread_idx < num_threads; thread_idx++) {
    const size_t start_entry = thread_idx * size_per_thread;
    const size_t end_entry = std::min(start_entry + size_per_thread, entry_count);
    conversion_threads.push_back(std::async(
        std::launch::async, locate_and_count_func, start_entry, end_entry, thread_idx));
  }

  try {
    for (auto& child : conversion_threads) {
      child.wait();
    }
  } catch (QueryExecutionError& e) {
    if (e.hasErrorCode(ErrorCode::INTERRUPTED)) {
      throw QueryExecutionError(ErrorCode::INTERRUPTED);
    }
    throw e;
  } catch (...) {
    throw;
  }
}

/**
 * This function goes through all non-empty elements marked in the bitmap data structure,
 * and store them back into output column buffers. The output column buffers are compacted
 * without any holes in it.
 *
 * TODO(Saman): if necessary, we can look into the distribution of non-empty entries
 * and choose a different load-balanced strategy (assigning equal number of non-empties
 * to each thread) as opposed to equal partitioning of the bitmap
 */
void ColumnarResults::compactAndCopyEntries(
    const ResultSet& rows,
    const ColumnBitmap& bitmap,
    const std::vector<size_t>& non_empty_per_thread,
    const size_t num_columns,
    const size_t entry_count,
    const size_t num_threads,
    const size_t size_per_thread) {
  auto timer = DEBUG_TIMER(__func__);
  CHECK(isDirectColumnarConversionPossible());
  CHECK(rows.getQueryDescriptionType() == QueryDescriptionType::GroupByPerfectHash ||
        rows.getQueryDescriptionType() == QueryDescriptionType::GroupByBaselineHash);
  CHECK_EQ(num_threads, non_empty_per_thread.size());

  // compute the exclusive scan over all non-empty totals
  std::vector<size_t> global_offsets(num_threads + 1, 0);
  std::partial_sum(non_empty_per_thread.begin(),
                   non_empty_per_thread.end(),
                   std::next(global_offsets.begin()));

  const auto slot_idx_per_target_idx = rows.getSlotIndicesForTargetIndices();
  const auto [single_slot_targets_to_skip, num_single_slot_targets] =
      rows.getSupportedSingleSlotTargetBitmap();

  // We skip multi-slot targets (e.g., AVG). These skipped targets are treated
  // differently and accessed through result set's iterator
  if (num_single_slot_targets < num_columns) {
    compactAndCopyEntriesWithTargetSkipping(rows,
                                            bitmap,
                                            non_empty_per_thread,
                                            global_offsets,
                                            single_slot_targets_to_skip,
                                            slot_idx_per_target_idx,
                                            num_columns,
                                            entry_count,
                                            num_threads,
                                            size_per_thread);
  } else {
    compactAndCopyEntriesWithoutTargetSkipping(rows,
                                               bitmap,
                                               non_empty_per_thread,
                                               global_offsets,
                                               slot_idx_per_target_idx,
                                               num_columns,
                                               entry_count,
                                               num_threads,
                                               size_per_thread);
  }
}

/**
 * This functions takes a bitmap of non-empty entries within the result set's storage
 * and compact and copy those contents back into the output column_buffers_.
 * In this variation, multi-slot targets (e.g., AVG) are treated with the existing
 * result set's iterations, but everything else is directly columnarized.
 */
void ColumnarResults::compactAndCopyEntriesWithTargetSkipping(
    const ResultSet& rows,
    const ColumnBitmap& bitmap,
    const std::vector<size_t>& non_empty_per_thread,
    const std::vector<size_t>& global_offsets,
    const std::vector<bool>& targets_to_skip,
    const std::vector<size_t>& slot_idx_per_target_idx,
    const size_t num_columns,
    const size_t entry_count,
    const size_t num_threads,
    const size_t size_per_thread) {
  auto timer = DEBUG_TIMER(__func__);
  CHECK(isDirectColumnarConversionPossible());
  CHECK(rows.getQueryDescriptionType() == QueryDescriptionType::GroupByPerfectHash ||
        rows.getQueryDescriptionType() == QueryDescriptionType::GroupByBaselineHash);

  const auto [write_functions, read_functions] =
      initAllConversionFunctions(rows, slot_idx_per_target_idx, targets_to_skip);
  CHECK_EQ(write_functions.size(), num_columns);
  CHECK_EQ(read_functions.size(), num_columns);
  std::mutex write_mutex;
  auto do_work = [this,
                  &bitmap,
                  &rows,
                  &slot_idx_per_target_idx,
                  &global_offsets,
                  &targets_to_skip,
                  &num_columns,
                  &write_mutex,
                  &write_functions = write_functions,
                  &read_functions = read_functions](size_t& non_empty_idx,
                                                    const size_t total_non_empty,
                                                    const size_t local_idx,
                                                    size_t& entry_idx,
                                                    const size_t thread_idx,
                                                    const size_t end_idx) {
    if (non_empty_idx >= total_non_empty) {
      // all non-empty entries has been written back
      entry_idx = end_idx;
    }
    const size_t output_buffer_row_idx = global_offsets[thread_idx] + non_empty_idx;
    if (bitmap.get(local_idx, thread_idx)) {
      // targets that are recovered from the result set iterators:
      const auto crt_row = rows.getRowAtNoTranslations(entry_idx, targets_to_skip);
      for (size_t column_idx = 0; column_idx < num_columns; ++column_idx) {
        if (!targets_to_skip.empty() && !targets_to_skip[column_idx]) {
          auto& type_info = target_types_[column_idx];
          writeBackCell(crt_row[column_idx],
                        output_buffer_row_idx,
                        type_info,
                        column_buffers_[column_idx],
                        &write_mutex);
        }
      }
      // targets that are copied directly without any translation/decoding from
      // result set
      for (size_t column_idx = 0; column_idx < num_columns; column_idx++) {
        if (!targets_to_skip.empty() && !targets_to_skip[column_idx]) {
          continue;
        }
        write_functions[column_idx](rows,
                                    entry_idx,
                                    output_buffer_row_idx,
                                    column_idx,
                                    slot_idx_per_target_idx[column_idx],
                                    read_functions[column_idx]);
      }
      non_empty_idx++;
    }
  };

  auto compact_buffer_func = [&non_empty_per_thread, &do_work, this](
                                 const size_t start_index,
                                 const size_t end_index,
                                 const size_t thread_idx) {
    const size_t total_non_empty = non_empty_per_thread[thread_idx];
    size_t non_empty_idx = 0;
    size_t local_idx = 0;
    if (g_enable_non_kernel_time_query_interrupt) {
      for (size_t entry_idx = start_index; entry_idx < end_index;
           entry_idx++, local_idx++) {
        checkInterruption(local_idx);
        do_work(
            non_empty_idx, total_non_empty, local_idx, entry_idx, thread_idx, end_index);
      }
    } else {
      for (size_t entry_idx = start_index; entry_idx < end_index;
           entry_idx++, local_idx++) {
        do_work(
            non_empty_idx, total_non_empty, local_idx, entry_idx, thread_idx, end_index);
      }
    }
  };

  std::vector<std::future<void>> compaction_threads;
  for (size_t thread_idx = 0; thread_idx < num_threads; thread_idx++) {
    const size_t start_entry = thread_idx * size_per_thread;
    const size_t end_entry = std::min(start_entry + size_per_thread, entry_count);
    compaction_threads.push_back(std::async(
        std::launch::async, compact_buffer_func, start_entry, end_entry, thread_idx));
  }

  try {
    for (auto& child : compaction_threads) {
      child.wait();
    }
  } catch (QueryExecutionError& e) {
    if (e.hasErrorCode(ErrorCode::INTERRUPTED)) {
      throw QueryExecutionError(ErrorCode::INTERRUPTED);
    }
    throw e;
  } catch (...) {
    throw;
  }
}

/**
 * This functions takes a bitmap of non-empty entries within the result set's storage
 * and compact and copy those contents back into the output column_buffers_.
 * In this variation, all targets are assumed to be single-slot and thus can be directly
 * columnarized.
 */
void ColumnarResults::compactAndCopyEntriesWithoutTargetSkipping(
    const ResultSet& rows,
    const ColumnBitmap& bitmap,
    const std::vector<size_t>& non_empty_per_thread,
    const std::vector<size_t>& global_offsets,
    const std::vector<size_t>& slot_idx_per_target_idx,
    const size_t num_columns,
    const size_t entry_count,
    const size_t num_threads,
    const size_t size_per_thread) {
  auto timer = DEBUG_TIMER(__func__);
  CHECK(isDirectColumnarConversionPossible());
  CHECK(rows.getQueryDescriptionType() == QueryDescriptionType::GroupByPerfectHash ||
        rows.getQueryDescriptionType() == QueryDescriptionType::GroupByBaselineHash);

  const auto [write_functions, read_functions] =
      initAllConversionFunctions(rows, slot_idx_per_target_idx);
  CHECK_EQ(write_functions.size(), num_columns);
  CHECK_EQ(read_functions.size(), num_columns);
  auto do_work = [&rows,
                  &bitmap,
                  &global_offsets,
                  &num_columns,
                  &slot_idx_per_target_idx,
                  &write_functions = write_functions,
                  &read_functions = read_functions](size_t& entry_idx,
                                                    size_t& non_empty_idx,
                                                    const size_t total_non_empty,
                                                    const size_t local_idx,
                                                    const size_t thread_idx,
                                                    const size_t end_idx) {
    if (non_empty_idx >= total_non_empty) {
      // all non-empty entries has been written back
      entry_idx = end_idx;
      return;
    }
    const size_t output_buffer_row_idx = global_offsets[thread_idx] + non_empty_idx;
    if (bitmap.get(local_idx, thread_idx)) {
      for (size_t column_idx = 0; column_idx < num_columns; column_idx++) {
        write_functions[column_idx](rows,
                                    entry_idx,
                                    output_buffer_row_idx,
                                    column_idx,
                                    slot_idx_per_target_idx[column_idx],
                                    read_functions[column_idx]);
      }
      non_empty_idx++;
    }
  };
  auto compact_buffer_func = [&non_empty_per_thread, &do_work, this](
                                 const size_t start_index,
                                 const size_t end_index,
                                 const size_t thread_idx) {
    const size_t total_non_empty = non_empty_per_thread[thread_idx];
    size_t non_empty_idx = 0;
    size_t local_idx = 0;
    if (g_enable_non_kernel_time_query_interrupt) {
      for (size_t entry_idx = start_index; entry_idx < end_index;
           entry_idx++, local_idx++) {
        checkInterruption(local_idx);
        do_work(
            entry_idx, non_empty_idx, total_non_empty, local_idx, thread_idx, end_index);
      }
    } else {
      for (size_t entry_idx = start_index; entry_idx < end_index;
           entry_idx++, local_idx++) {
        do_work(
            entry_idx, non_empty_idx, total_non_empty, local_idx, thread_idx, end_index);
      }
    }
  };

  std::vector<std::future<void>> compaction_threads;
  for (size_t thread_idx = 0; thread_idx < num_threads; thread_idx++) {
    const size_t start_entry = thread_idx * size_per_thread;
    const size_t end_entry = std::min(start_entry + size_per_thread, entry_count);
    compaction_threads.push_back(std::async(
        std::launch::async, compact_buffer_func, start_entry, end_entry, thread_idx));
  }

  try {
    for (auto& child : compaction_threads) {
      child.wait();
    }
  } catch (QueryExecutionError& e) {
    if (e.hasErrorCode(ErrorCode::INTERRUPTED)) {
      throw QueryExecutionError(ErrorCode::INTERRUPTED);
    }
    throw e;
  } catch (...) {
    throw;
  }
}

/**
 * Initialize a set of write functions per target (i.e., column). Target types' logical
 * size are used to categorize the correct write function per target. These functions are
 * then used for every row in the result set.
 */
std::vector<ColumnarResults::WriteFunction> ColumnarResults::initWriteFunctions(
    const ResultSet& rows,
    const std::vector<bool>& targets_to_skip) {
  CHECK(isDirectColumnarConversionPossible());
  CHECK(rows.getQueryDescriptionType() == QueryDescriptionType::GroupByPerfectHash ||
        rows.getQueryDescriptionType() == QueryDescriptionType::GroupByBaselineHash);

  std::vector<WriteFunction> result;
  result.reserve(target_types_.size());

  for (size_t target_idx = 0; target_idx < target_types_.size(); target_idx++) {
    if (!targets_to_skip.empty() && !targets_to_skip[target_idx]) {
      result.emplace_back([](const ResultSet& rows,
                             const size_t input_buffer_entry_idx,
                             const size_t output_buffer_entry_idx,
                             const size_t target_idx,
                             const size_t slot_idx,
                             const ReadFunction& read_function) {
        UNREACHABLE() << "Invalid write back function used.";
      });
      continue;
    }

    if (target_types_[target_idx].is_fp()) {
      switch (target_types_[target_idx].get_size()) {
        case 8:
          result.emplace_back(std::bind(&ColumnarResults::writeBackCellDirect<double>,
                                        this,
                                        std::placeholders::_1,
                                        std::placeholders::_2,
                                        std::placeholders::_3,
                                        std::placeholders::_4,
                                        std::placeholders::_5,
                                        std::placeholders::_6));
          break;
        case 4:
          result.emplace_back(std::bind(&ColumnarResults::writeBackCellDirect<float>,
                                        this,
                                        std::placeholders::_1,
                                        std::placeholders::_2,
                                        std::placeholders::_3,
                                        std::placeholders::_4,
                                        std::placeholders::_5,
                                        std::placeholders::_6));
          break;
        default:
          UNREACHABLE() << "Invalid target type encountered.";
          break;
      }
    } else {
      switch (target_types_[target_idx].get_size()) {
        case 8:
          result.emplace_back(std::bind(&ColumnarResults::writeBackCellDirect<int64_t>,
                                        this,
                                        std::placeholders::_1,
                                        std::placeholders::_2,
                                        std::placeholders::_3,
                                        std::placeholders::_4,
                                        std::placeholders::_5,
                                        std::placeholders::_6));
          break;
        case 4:
          result.emplace_back(std::bind(&ColumnarResults::writeBackCellDirect<int32_t>,
                                        this,
                                        std::placeholders::_1,
                                        std::placeholders::_2,
                                        std::placeholders::_3,
                                        std::placeholders::_4,
                                        std::placeholders::_5,
                                        std::placeholders::_6));
          break;
        case 2:
          result.emplace_back(std::bind(&ColumnarResults::writeBackCellDirect<int16_t>,
                                        this,
                                        std::placeholders::_1,
                                        std::placeholders::_2,
                                        std::placeholders::_3,
                                        std::placeholders::_4,
                                        std::placeholders::_5,
                                        std::placeholders::_6));
          break;
        case 1:
          result.emplace_back(std::bind(&ColumnarResults::writeBackCellDirect<int8_t>,
                                        this,
                                        std::placeholders::_1,
                                        std::placeholders::_2,
                                        std::placeholders::_3,
                                        std::placeholders::_4,
                                        std::placeholders::_5,
                                        std::placeholders::_6));
          break;
        default:
          UNREACHABLE() << "Invalid target type encountered.";
          break;
      }
    }
  }
  return result;
}

namespace {

int64_t invalid_read_func(const ResultSet& rows,
                          const size_t input_buffer_entry_idx,
                          const size_t target_idx,
                          const size_t slot_idx) {
  UNREACHABLE() << "Invalid read function used, target should have been skipped.";
  return static_cast<int64_t>(0);
}

template <QueryDescriptionType QUERY_TYPE, bool COLUMNAR_OUTPUT>
int64_t read_float_key_baseline(const ResultSet& rows,
                                const size_t input_buffer_entry_idx,
                                const size_t target_idx,
                                const size_t slot_idx) {
  // float keys in baseline hash are written as doubles in the buffer, so
  // the result should properly be casted before being written in the output
  // columns
  auto fval = static_cast<float>(rows.getEntryAt<double, QUERY_TYPE, COLUMNAR_OUTPUT>(
      input_buffer_entry_idx, target_idx, slot_idx));
  return *reinterpret_cast<int32_t*>(may_alias_ptr(&fval));
}

template <QueryDescriptionType QUERY_TYPE, bool COLUMNAR_OUTPUT>
int64_t read_int64_func(const ResultSet& rows,
                        const size_t input_buffer_entry_idx,
                        const size_t target_idx,
                        const size_t slot_idx) {
  return rows.getEntryAt<int64_t, QUERY_TYPE, COLUMNAR_OUTPUT>(
      input_buffer_entry_idx, target_idx, slot_idx);
}

template <QueryDescriptionType QUERY_TYPE, bool COLUMNAR_OUTPUT>
int64_t read_int32_func(const ResultSet& rows,
                        const size_t input_buffer_entry_idx,
                        const size_t target_idx,
                        const size_t slot_idx) {
  return rows.getEntryAt<int32_t, QUERY_TYPE, COLUMNAR_OUTPUT>(
      input_buffer_entry_idx, target_idx, slot_idx);
}

template <QueryDescriptionType QUERY_TYPE, bool COLUMNAR_OUTPUT>
int64_t read_int16_func(const ResultSet& rows,
                        const size_t input_buffer_entry_idx,
                        const size_t target_idx,
                        const size_t slot_idx) {
  return rows.getEntryAt<int16_t, QUERY_TYPE, COLUMNAR_OUTPUT>(
      input_buffer_entry_idx, target_idx, slot_idx);
}

template <QueryDescriptionType QUERY_TYPE, bool COLUMNAR_OUTPUT>
int64_t read_int8_func(const ResultSet& rows,
                       const size_t input_buffer_entry_idx,
                       const size_t target_idx,
                       const size_t slot_idx) {
  return rows.getEntryAt<int8_t, QUERY_TYPE, COLUMNAR_OUTPUT>(
      input_buffer_entry_idx, target_idx, slot_idx);
}

template <QueryDescriptionType QUERY_TYPE, bool COLUMNAR_OUTPUT>
int64_t read_float_func(const ResultSet& rows,
                        const size_t input_buffer_entry_idx,
                        const size_t target_idx,
                        const size_t slot_idx) {
  auto fval = rows.getEntryAt<float, QUERY_TYPE, COLUMNAR_OUTPUT>(
      input_buffer_entry_idx, target_idx, slot_idx);
  return *reinterpret_cast<int32_t*>(may_alias_ptr(&fval));
}

template <QueryDescriptionType QUERY_TYPE, bool COLUMNAR_OUTPUT>
int64_t read_double_func(const ResultSet& rows,
                         const size_t input_buffer_entry_idx,
                         const size_t target_idx,
                         const size_t slot_idx) {
  auto dval = rows.getEntryAt<double, QUERY_TYPE, COLUMNAR_OUTPUT>(
      input_buffer_entry_idx, target_idx, slot_idx);
  return *reinterpret_cast<int64_t*>(may_alias_ptr(&dval));
}

}  // namespace

/**
 * Initializes a set of read funtions to properly access the contents of the result set's
 * storage buffer. Each particular read function is chosen based on the data type and data
 * size used to store that target in the result set's storage buffer. These functions are
 * then used for each row in the result set.
 */
template <QueryDescriptionType QUERY_TYPE, bool COLUMNAR_OUTPUT>
std::vector<ColumnarResults::ReadFunction> ColumnarResults::initReadFunctions(
    const ResultSet& rows,
    const std::vector<size_t>& slot_idx_per_target_idx,
    const std::vector<bool>& targets_to_skip) {
  CHECK(isDirectColumnarConversionPossible());
  CHECK(COLUMNAR_OUTPUT == rows.didOutputColumnar());
  CHECK(QUERY_TYPE == rows.getQueryDescriptionType());

  std::vector<ReadFunction> read_functions;
  read_functions.reserve(target_types_.size());

  for (size_t target_idx = 0; target_idx < target_types_.size(); target_idx++) {
    if (!targets_to_skip.empty() && !targets_to_skip[target_idx]) {
      // for targets that should be skipped, we use a placeholder function that should
      // never be called. The CHECKs inside it make sure that never happens.
      read_functions.emplace_back(invalid_read_func);
      continue;
    }

    if (QUERY_TYPE == QueryDescriptionType::GroupByBaselineHash) {
      if (rows.getPaddedSlotWidthBytes(slot_idx_per_target_idx[target_idx]) == 0) {
        // for key columns only
        CHECK(rows.query_mem_desc_.getTargetGroupbyIndex(target_idx) >= 0);
        if (target_types_[target_idx].is_fp()) {
          CHECK_EQ(size_t(8), rows.query_mem_desc_.getEffectiveKeyWidth());
          switch (target_types_[target_idx].get_type()) {
            case kFLOAT:
              read_functions.emplace_back(
                  read_float_key_baseline<QUERY_TYPE, COLUMNAR_OUTPUT>);
              break;
            case kDOUBLE:
              read_functions.emplace_back(read_double_func<QUERY_TYPE, COLUMNAR_OUTPUT>);
              break;
            default:
              UNREACHABLE()
                  << "Invalid data type encountered (BaselineHash, floating point key).";
              break;
          }
        } else {
          switch (rows.query_mem_desc_.getEffectiveKeyWidth()) {
            case 8:
              read_functions.emplace_back(read_int64_func<QUERY_TYPE, COLUMNAR_OUTPUT>);
              break;
            case 4:
              read_functions.emplace_back(read_int32_func<QUERY_TYPE, COLUMNAR_OUTPUT>);
              break;
            default:
              UNREACHABLE()
                  << "Invalid data type encountered (BaselineHash, integer key).";
          }
        }
        continue;
      }
    }
    if (target_types_[target_idx].is_fp()) {
      switch (rows.getPaddedSlotWidthBytes(slot_idx_per_target_idx[target_idx])) {
        case 8:
          read_functions.emplace_back(read_double_func<QUERY_TYPE, COLUMNAR_OUTPUT>);
          break;
        case 4:
          read_functions.emplace_back(read_float_func<QUERY_TYPE, COLUMNAR_OUTPUT>);
          break;
        default:
          UNREACHABLE() << "Invalid data type encountered (floating point agg column).";
          break;
      }
    } else {
      switch (rows.getPaddedSlotWidthBytes(slot_idx_per_target_idx[target_idx])) {
        case 8:
          read_functions.emplace_back(read_int64_func<QUERY_TYPE, COLUMNAR_OUTPUT>);
          break;
        case 4:
          read_functions.emplace_back(read_int32_func<QUERY_TYPE, COLUMNAR_OUTPUT>);
          break;
        case 2:
          read_functions.emplace_back(read_int16_func<QUERY_TYPE, COLUMNAR_OUTPUT>);
          break;
        case 1:
          read_functions.emplace_back(read_int8_func<QUERY_TYPE, COLUMNAR_OUTPUT>);
          break;
        default:
          UNREACHABLE() << "Invalid data type encountered (integer agg column).";
          break;
      }
    }
  }
  return read_functions;
}

/**
 * This function goes through all target types in the output, and chooses appropriate
 * write and read functions per target. The goal is then to simply use these functions
 * for each row and per target. Read functions are used to read each cell's data content
 * (particular target in a row), and write functions are used to properly write back the
 * cell's content into the output column buffers.
 */
std::tuple<std::vector<ColumnarResults::WriteFunction>,
           std::vector<ColumnarResults::ReadFunction>>
ColumnarResults::initAllConversionFunctions(
    const ResultSet& rows,
    const std::vector<size_t>& slot_idx_per_target_idx,
    const std::vector<bool>& targets_to_skip) {
  CHECK(isDirectColumnarConversionPossible() &&
        (rows.getQueryDescriptionType() == QueryDescriptionType::GroupByPerfectHash ||
         rows.getQueryDescriptionType() == QueryDescriptionType::GroupByBaselineHash));

  const auto write_functions = initWriteFunctions(rows, targets_to_skip);
  if (rows.getQueryDescriptionType() == QueryDescriptionType::GroupByPerfectHash) {
    if (rows.didOutputColumnar()) {
      return std::make_tuple(
          std::move(write_functions),
          initReadFunctions<QueryDescriptionType::GroupByPerfectHash, true>(
              rows, slot_idx_per_target_idx, targets_to_skip));
    } else {
      return std::make_tuple(
          std::move(write_functions),
          initReadFunctions<QueryDescriptionType::GroupByPerfectHash, false>(
              rows, slot_idx_per_target_idx, targets_to_skip));
    }
  } else {
    if (rows.didOutputColumnar()) {
      return std::make_tuple(
          std::move(write_functions),
          initReadFunctions<QueryDescriptionType::GroupByBaselineHash, true>(
              rows, slot_idx_per_target_idx, targets_to_skip));
    } else {
      return std::make_tuple(
          std::move(write_functions),
          initReadFunctions<QueryDescriptionType::GroupByBaselineHash, false>(
              rows, slot_idx_per_target_idx, targets_to_skip));
    }
  }
}

void ColumnarResults::checkInterruption(size_t row_idx) const {
  if (UNLIKELY((row_idx & 0xFFFF) == 0 && g_enable_non_kernel_time_query_interrupt &&
               executor_->checkNonKernelTimeInterrupted())) {
    throw QueryExecutionError(ErrorCode::INTERRUPTED);
  }
}

template <typename T>
void ColumnarResults::fetchAndCheckInterruption(const size_t start,
                                                const size_t end,
                                                const size_t col_idx,
                                                const size_t col_width,
                                                int8_t* col_buffer,
                                                const ResultSet& rows) {
  for (size_t row_idx = start; row_idx < end; ++row_idx) {
    T* target_ptr = reinterpret_cast<T*>(col_buffer + row_idx * col_width);
    rows.fetchLazyColumnValue<T>(row_idx, col_idx, target_ptr);
    checkInterruption(row_idx);
  }
}
