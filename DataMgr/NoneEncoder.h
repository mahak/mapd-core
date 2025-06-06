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

#ifndef NONE_ENCODER_H
#define NONE_ENCODER_H

#include "AbstractBuffer.h"
#include "Encoder.h"

#include <Shared/DatumFetchers.h>
#include <Shared/Iteration.h>

#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>
#include <tuple>

template <typename T>
T none_encoded_null_value() {
  return std::is_integral<T>::value ? inline_int_null_value<T>()
                                    : inline_fp_null_value<T>();
}

template <typename T>
class NoneEncoder : public Encoder {
 public:
  NoneEncoder(Data_Namespace::AbstractBuffer* buffer)
      : Encoder(buffer), encoded_null_value_(none_encoded_null_value<T>()) {
    resetChunkStats();
  }

  size_t getNumElemsForBytesEncodedDataAtIndices(const int8_t* index_data,
                                                 const std::vector<size_t>& selected_idx,
                                                 const size_t byte_limit) override {
    UNREACHABLE()
        << "getNumElemsForBytesEncodedDataAtIndices unexpectedly called for non varlen"
           " encoder";
    return {};
  }

  std::shared_ptr<ChunkMetadata> appendEncodedDataAtIndices(
      const int8_t*,
      int8_t* data,
      const std::vector<size_t>& selected_idx) override {
    std::shared_ptr<ChunkMetadata> chunk_metadata;
    // NOTE: the use of `execute_over_contiguous_indices` is an optimization;
    // it prevents having to copy or move the indexed data and instead performs
    // an append over contiguous sections of indices.
    shared::execute_over_contiguous_indices(
        selected_idx, [&](const size_t start_pos, const size_t end_pos) {
          size_t elem_count = end_pos - start_pos;
          chunk_metadata =
              appendEncodedData(nullptr, data, selected_idx[start_pos], elem_count);
        });

    return chunk_metadata;
  }

  std::shared_ptr<ChunkMetadata> appendEncodedData(const int8_t*,
                                                   int8_t* data,
                                                   const size_t start_idx,
                                                   const size_t num_elements) override {
    auto current_data = data + sizeof(T) * start_idx;
    return appendValidatedOrNonValidatedData(current_data, num_elements, false, -1, true);
  }

  std::shared_ptr<ChunkMetadata> appendData(int8_t*& src_data,
                                            const size_t num_elems_to_append,
                                            const SQLTypeInfo&,
                                            const bool replicating = false,
                                            const int64_t offset = -1) override {
    return appendValidatedOrNonValidatedData(
        src_data, num_elems_to_append, replicating, offset, false);
  }

  ChunkStats getChunkStats() const override {
    CHECK(buffer_);
    return ChunkStats(dataMin, dataMax, has_nulls, buffer_->getSqlType());
  }

  ChunkStats synthesizeChunkStats(const SQLTypeInfo& ti) const override {
    return ChunkStats(dataMin, dataMax, has_nulls, ti);
  }

  // Only called from the executor for synthesized meta-information.
  void updateStats(const int64_t val, const bool is_null) override {
    if (is_null) {
      has_nulls = true;
    } else {
      const auto data = static_cast<T>(val);
      dataMin = std::min(dataMin, data);
      dataMax = std::max(dataMax, data);
    }
  }

  // Only called from the executor for synthesized meta-information.
  void updateStats(const double val, const bool is_null) override {
    if (is_null) {
      has_nulls = true;
    } else {
      const auto data = static_cast<T>(val);
      dataMin = std::min(dataMin, data);
      dataMax = std::max(dataMax, data);
    }
  }

  void updateStats(const int8_t* const src_data, const size_t num_elements) override {
    updateStats(src_data, num_elements, false);
  }

  void updateStatsEncoded(const int8_t* const dst_data,
                          const size_t num_elements) override {
    const T* data = reinterpret_cast<const T*>(dst_data);

    std::tie(dataMin, dataMax, has_nulls) = tbb::parallel_reduce(
        tbb::blocked_range(size_t(0), num_elements),
        std::tuple(dataMin, dataMax, has_nulls),
        [&](const auto& range, auto init) {
          auto [min, max, nulls] = init;
          for (size_t i = range.begin(); i < range.end(); i++) {
            if (data[i] != encoded_null_value_) {
              decimal_overflow_validator_.validate(data[i]);
              min = std::min(min, data[i]);
              max = std::max(max, data[i]);
            } else {
              nulls = true;
            }
          }
          return std::tuple(min, max, nulls);
        },
        [&](auto lhs, auto rhs) {
          const auto [lhs_min, lhs_max, lhs_nulls] = lhs;
          const auto [rhs_min, rhs_max, rhs_nulls] = rhs;
          return std::tuple(std::min(lhs_min, rhs_min),
                            std::max(lhs_max, rhs_max),
                            lhs_nulls || rhs_nulls);
        });
  }

  void updateStats(const std::vector<std::string>* const src_data,
                   const size_t start_idx,
                   const size_t num_elements) override {
    UNREACHABLE();
  }

  void updateStats(const std::string* src_data,
                   const size_t start_idx,
                   const size_t num_elements) override {
    UNREACHABLE();
  }

  void updateStats(const std::vector<ArrayDatum>* const src_data,
                   const size_t start_idx,
                   const size_t num_elements) override {
    UNREACHABLE();
  }

  void updateStats(const ArrayDatum* src_data,
                   const size_t start_idx,
                   const size_t num_elements) override {
    UNREACHABLE();
  }

  // Only called from the executor for synthesized meta-information.
  void reduceStats(const Encoder& that) override {
    const auto that_typed = static_cast<const NoneEncoder&>(that);
    if (that_typed.has_nulls) {
      has_nulls = true;
    }
    dataMin = std::min(dataMin, that_typed.dataMin);
    dataMax = std::max(dataMax, that_typed.dataMax);
  }

  void writeChunkStats(FILE* f) override {
    fwrite((int8_t*)&dataMin, sizeof(T), 1, f);
    fwrite((int8_t*)&dataMax, sizeof(T), 1, f);
    fwrite((int8_t*)&has_nulls, sizeof(bool), 1, f);
  }

  void readChunkStats(FILE* f) override {
    fread((int8_t*)&dataMin, sizeof(T), 1, f);
    fread((int8_t*)&dataMax, sizeof(T), 1, f);
    fread((int8_t*)&has_nulls, sizeof(bool), 1, f);
  }

  bool setChunkStats(const ChunkStats& stats) override {
    const auto new_min = DatumFetcher::getDatumVal<T>(stats.min);
    const auto new_max = DatumFetcher::getDatumVal<T>(stats.max);

    if (dataMin == new_min && dataMax == new_max && has_nulls == stats.has_nulls) {
      return false;
    }

    dataMin = new_min;
    dataMax = new_max;
    has_nulls = stats.has_nulls;
    return true;
  }

  void copyChunkStats(const Encoder* copyFromEncoder) override {
    auto castedEncoder = reinterpret_cast<const NoneEncoder<T>*>(copyFromEncoder);
    dataMin = castedEncoder->dataMin;
    dataMax = castedEncoder->dataMax;
    has_nulls = castedEncoder->has_nulls;
  }

  void resetChunkStats() override {
    dataMin = std::numeric_limits<T>::max();
    dataMax = std::numeric_limits<T>::lowest();
    has_nulls = false;
  }

  T dataMin;
  T dataMax;
  bool has_nulls;

 private:
  std::shared_ptr<ChunkMetadata> appendValidatedOrNonValidatedData(
      int8_t*& src_data,
      const size_t num_elems_to_append,
      const bool replicating,
      const int64_t offset,
      const bool is_validated_data) {
    if (offset == 0 && num_elems_to_append >= num_elems_) {
      resetChunkStats();
    }
    T* unencodedData = reinterpret_cast<T*>(src_data);
    std::vector<T> encoded_data;
    if (replicating) {
      if (num_elems_to_append > 0) {
        encoded_data.resize(num_elems_to_append);
        T data = validateDataAndUpdateStats(unencodedData[0]);
        std::fill(encoded_data.begin(), encoded_data.end(), data);
      }
    } else {
      updateStats(src_data, num_elems_to_append, is_validated_data);
    }
    if (offset == -1) {
      auto append_data_size = num_elems_to_append * sizeof(T);
      buffer_->reserve(buffer_->size() + append_data_size);
      num_elems_ += num_elems_to_append;
      buffer_->append(
          replicating ? reinterpret_cast<int8_t*>(encoded_data.data()) : src_data,
          append_data_size);
      if (!replicating) {
        src_data += num_elems_to_append * sizeof(T);
      }
    } else {
      num_elems_ = offset + num_elems_to_append;
      CHECK(!replicating);
      CHECK_GE(offset, 0);
      buffer_->write(
          src_data, num_elems_to_append * sizeof(T), static_cast<size_t>(offset));
    }
    return std::make_shared<ChunkMetadata>(getMetadata());
  }

  T validateDataAndUpdateStats(const T& unencoded_data,
                               const bool is_validated_data = false) {
    if (unencoded_data == encoded_null_value_) {
      has_nulls = true;
    } else {
      if (!is_validated_data) {  // does not need validation
        decimal_overflow_validator_.validate(unencoded_data);
      }
      dataMin = std::min(dataMin, unencoded_data);
      dataMax = std::max(dataMax, unencoded_data);
    }
    return unencoded_data;
  }

  void updateStats(const int8_t* const src_data,
                   const size_t num_elements,
                   const bool is_validated_data) {
    const T* unencoded_data = reinterpret_cast<const T*>(src_data);
    for (size_t i = 0; i < num_elements; ++i) {
      validateDataAndUpdateStats(unencoded_data[i], is_validated_data);
    }
  }

  const T encoded_null_value_;
};  // class NoneEncoder

#endif  // NONE_ENCODER_H
