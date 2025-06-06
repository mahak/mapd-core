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

#pragma once

#include <boost/noncopyable.hpp>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Analyzer/Analyzer.h"
#include "ApproxQuantileDescriptor.h"
#include "DataMgr/AbstractBuffer.h"
#include "DataMgr/Allocators/ArenaAllocator.h"
#include "DataMgr/Allocators/CpuMgrArenaAllocator.h"
#include "DataMgr/Allocators/FastAllocator.h"
#include "DataMgr/DataMgr.h"
#include "Logger/Logger.h"
#include "QueryEngine/AggMode.h"
#include "QueryEngine/CountDistinct.h"
#include "QueryEngine/StringDictionaryGenerations.h"
#include "QueryEngine/TableFunctionMetadataType.h"
#include "Shared/DbObjectKeys.h"
#include "Shared/quantile.h"
#include "StringDictionary/StringDictionaryProxy.h"
#include "StringOps/StringOps.h"

extern bool g_allow_memory_status_log;
extern bool g_use_cpu_mem_pool_for_output_buffers;

namespace Catalog_Namespace {
class Catalog;
}

class ResultSet;

/**
 * Handles allocations and outputs for all stages in a query, either explicitly or via a
 * managed allocator object
 */
class RowSetMemoryOwner final : public SimpleAllocator, boost::noncopyable {
 public:
  RowSetMemoryOwner(const size_t arena_block_size, const size_t executor_id)
      : arena_block_size_(arena_block_size)
      , executor_id_(executor_id)
      , next_temp_dict_id_(1) {
    // initialize shared allocator (i.e., allocators_[0])
    if (g_use_cpu_mem_pool_for_output_buffers) {
      allocators_.emplace_back(std::make_unique<CpuMgrArenaAllocator>());
    } else {
      allocators_.emplace_back(std::make_unique<DramArena>(arena_block_size_));
    }
    count_distinct_buffer_fast_allocators_.resize(allocators_.size());
  }

  enum class StringTranslationType { SOURCE_INTERSECTION, SOURCE_UNION };

  void setKernelMemoryAllocator(const size_t num_kernels) {
    CHECK_GT(num_kernels, static_cast<size_t>(0));
    CHECK_EQ(non_owned_group_by_buffers_.size(), static_cast<size_t>(0));
    // buffer for kernels starts with one-based indexing
    auto const required_num_kernels = num_kernels + 1;
    non_owned_group_by_buffers_.resize(required_num_kernels, nullptr);
    // sometimes the same RSMO instance handles multiple work units or even multiple query
    // steps (this means the RSMO's owner, an Executor instance, takes a responsibility to
    // proceed them) so, if the first query step has M allocators but if the second query
    // step requires N allocators where N > M, let's allocate M - N allocators instead of
    // recreating M new allocators
    if (required_num_kernels > allocators_.size()) {
      auto const required_num_allocators = required_num_kernels - allocators_.size();
      VLOG(1) << "Prepare " << required_num_allocators
              << " memory allocator(s) (Executor-" << executor_id_
              << ", # existing allocator(s): " << allocators_.size()
              << ", # requested allocator(s): " << required_num_kernels << ")";
      for (size_t i = 0; i < required_num_allocators; i++) {
        if (g_use_cpu_mem_pool_for_output_buffers) {
          allocators_.emplace_back(std::make_unique<CpuMgrArenaAllocator>());
        } else {
          // todo (yoonmin): can we determine better default min_block_size per query?
          allocators_.emplace_back(std::make_unique<DramArena>(arena_block_size_));
        }
      }
    }
    CHECK_GE(allocators_.size(), required_num_kernels);
    count_distinct_buffer_fast_allocators_.resize(allocators_.size());
  }

  // allocate memory via shared allocator
  int8_t* allocate(const size_t num_bytes) override {
    constexpr size_t thread_idx = 0u;
    return allocate(num_bytes, thread_idx);
  }

  // allocate memory via thread's unique allocator
  int8_t* allocate(const size_t num_bytes, const size_t thread_idx) {
    CHECK_LT(thread_idx, allocators_.size());
    std::lock_guard<std::mutex> lock(state_mutex_);
    return allocateUnlocked(num_bytes, thread_idx);
  }

  void initCountDistinctBufferForFastAllocation(int8_t* buffer,
                                                size_t buffer_size,
                                                size_t thread_idx) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    VLOG(1) << "Count distinct buffer allocator initialized for thread_idx: "
            << thread_idx;
    CHECK_LT(thread_idx, count_distinct_buffer_fast_allocators_.size());
    if (count_distinct_buffer_fast_allocators_[thread_idx]) {
      VLOG(1) << "Replacing count_distinct_buffer_allocators_[" << thread_idx << "].";
    }
    count_distinct_buffer_fast_allocators_[thread_idx] =
        std::make_unique<CountDistinctBufferFastAllocator>(buffer, buffer_size);
  }

  void allocAndInitCountDistinctBufferForFastAllocator(size_t buffer_size,
                                                       size_t thread_idx) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    CHECK_LT(thread_idx, count_distinct_buffer_fast_allocators_.size());
    if (count_distinct_buffer_fast_allocators_[thread_idx]) {
      VLOG(1) << "Replacing count_distinct_buffer_allocators_[" << thread_idx
              << "] to a newly allocated buffer.";
    } else {
      VLOG(1) << "Count distinct buffer allocator initialized with newly allocated "
                 "buffer having buffer_size: "
              << buffer_size << ", thread_idx: " << thread_idx;
    }
    count_distinct_buffer_fast_allocators_[thread_idx] =
        std::make_unique<CountDistinctBufferFastAllocator>(
            allocateUnlocked(buffer_size, thread_idx), buffer_size);
  }

  std::optional<size_t> AddCountDistinctBufferFastAllocator(size_t buffer_size) {
    if (buffer_size <= 0) {
      return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto const allocator_idx = count_distinct_buffer_fast_allocators_.size();
    VLOG(1) << "Count distinct buffer allocator initialized with newly allocated buffer "
               "(size: "
            << buffer_size << ", allocator_idx: " << allocator_idx << ")";
    count_distinct_buffer_fast_allocators_.emplace_back(
        std::make_unique<RowSetMemoryOwner::CountDistinctBufferFastAllocator>(
            allocateUnlocked(buffer_size, 0), buffer_size));
    return allocator_idx;
  }

  std::pair<int64_t*, bool> allocateCachedGroupByBuffer(const size_t num_bytes,
                                                        const size_t thread_idx) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    CHECK_LT(thread_idx, non_owned_group_by_buffers_.size());
    // First try cache
    if (non_owned_group_by_buffers_[thread_idx]) {  // not nullptr
      VLOG(1) << "Reuse cached groupby buffer: " << num_bytes << " bytes (THREAD-"
              << thread_idx << ")";
      return std::make_pair(non_owned_group_by_buffers_[thread_idx], true);
    }
    // Was not in cache so must allocate
    auto allocator = allocators_[thread_idx].get();
    if (g_allow_memory_status_log) {
      VLOG(1) << "Try to allocate CPU memory: " << num_bytes << " bytes (THREAD-"
              << thread_idx << ")";
    }
    int64_t* group_by_buffer = reinterpret_cast<int64_t*>(allocator->allocate(num_bytes));
    CHECK(group_by_buffer);
    // Put in cache
    non_owned_group_by_buffers_[thread_idx] = group_by_buffer;
    return std::make_pair(group_by_buffer, false);
  }

  int8_t* fastAllocateCountDistinctBuffer(const size_t num_bytes,
                                          const size_t thread_idx = 0) {
    CHECK_LT(thread_idx, count_distinct_buffer_fast_allocators_.size());
    CHECK(count_distinct_buffer_fast_allocators_[thread_idx]);
    int8_t* buffer =
        count_distinct_buffer_fast_allocators_[thread_idx]->allocate(num_bytes);
    initAndAddCountDistinctBuffer(buffer, num_bytes);
    return buffer;
  }

  int8_t* slowAllocateCountDistinctBuffer(const size_t num_bytes,
                                          const size_t thread_idx = 0) {
    int8_t* buffer = allocate(num_bytes, thread_idx);
    initAndAddCountDistinctBuffer(buffer, num_bytes);
    return buffer;
  }

  void addCountDistinctBuffer(int8_t* count_distinct_buffer,
                              const size_t bytes,
                              const bool physical_buffer) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    count_distinct_bitmaps_.emplace_back(
        CountDistinctBitmapBuffer{count_distinct_buffer, bytes, physical_buffer});
  }

  void addCountDistinctSet(CountDistinctSet* count_distinct_set) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    count_distinct_sets_.push_back(count_distinct_set);
  }

  void clearNonOwnedGroupByBuffers() { non_owned_group_by_buffers_.clear(); }

  void addVarlenBuffer(void* varlen_buffer) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    varlen_buffers_.emplace(varlen_buffer);
  }

  /**
   * Adds a GPU buffer containing a variable length input column. Variable length inputs
   * on GPU are referenced in output projected targets and should not be freed until the
   * query results have been resolved.
   */
  void addVarlenInputBuffer(Data_Namespace::AbstractBuffer* buffer) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    CHECK_EQ(buffer->getType(), Data_Namespace::MemoryLevel::GPU_LEVEL);
    varlen_input_buffers_.push_back(buffer);
  }

  std::string* addString(const std::string& str) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    strings_.emplace_back(str);
    return &strings_.back();
  }

  std::vector<int64_t>* addArray(const std::vector<int64_t>& arr) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    arrays_.emplace_back(arr);
    return &arrays_.back();
  }

  StringDictionaryProxy* addStringDict(std::shared_ptr<StringDictionary> str_dict,
                                       const shared::StringDictKey& dict_key,
                                       const int64_t generation) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = str_dict_proxy_owned_.find(dict_key);
    if (it != str_dict_proxy_owned_.end()) {
      CHECK_EQ(it->second->getDictionary(), str_dict.get());
      it->second->updateGeneration(generation);
      return it->second.get();
    }
    it = str_dict_proxy_owned_
             .emplace(
                 dict_key,
                 std::make_shared<StringDictionaryProxy>(str_dict, dict_key, generation))
             .first;
    return it->second.get();
  }

  std::string generate_translation_map_key(
      const shared::StringDictKey& source_proxy_dict_key,
      const std::vector<StringOps_Namespace::StringOpInfo>& string_op_infos) {
    std::ostringstream oss;
    oss << "{source_dict_key: " << source_proxy_dict_key
        << " StringOps: " << string_op_infos << "}";
    return oss.str();
  }

  std::string generate_translation_map_key(
      const shared::StringDictKey& source_proxy_dict_key,
      const shared::StringDictKey& dest_proxy_dict_key,
      const std::vector<StringOps_Namespace::StringOpInfo>& string_op_infos) {
    std::ostringstream oss;
    oss << "{source_dict_key: " << source_proxy_dict_key
        << ", dest_dict_key: " << dest_proxy_dict_key << " StringOps: " << string_op_infos
        << "}";
    return oss.str();
  }

  const StringDictionaryProxy::IdMap* addStringProxyIntersectionTranslationMap(
      const StringDictionaryProxy* source_proxy,
      const StringDictionaryProxy* dest_proxy,
      const std::vector<StringOps_Namespace::StringOpInfo>& string_op_infos) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto map_key =
        generate_translation_map_key(source_proxy->getDictionary()->getDictKey(),
                                     dest_proxy->getDictionary()->getDictKey(),
                                     string_op_infos);
    auto it = str_proxy_intersection_translation_maps_owned_.find(map_key);
    if (it == str_proxy_intersection_translation_maps_owned_.end()) {
      it = str_proxy_intersection_translation_maps_owned_
               .emplace(map_key,
                        source_proxy->buildIntersectionTranslationMapToOtherProxy(
                            dest_proxy, string_op_infos))
               .first;
    }
    return &it->second;
  }

  const StringDictionaryProxy::TranslationMap<Datum>* addStringProxyNumericTranslationMap(
      const StringDictionaryProxy* source_proxy,
      const std::vector<StringOps_Namespace::StringOpInfo>& string_op_infos) {
    const auto map_key = generate_translation_map_key(
        source_proxy->getDictionary()->getDictKey(), string_op_infos);
    auto it = str_proxy_numeric_translation_maps_owned_.lower_bound(map_key);
    if (it->first != map_key) {
      it = str_proxy_numeric_translation_maps_owned_.emplace_hint(
          it, map_key, source_proxy->buildNumericTranslationMap(string_op_infos));
    }
    return &it->second;
  }

  const StringDictionaryProxy::IdMap* addStringProxyUnionTranslationMap(
      const StringDictionaryProxy* source_proxy,
      StringDictionaryProxy* dest_proxy,
      const std::vector<StringOps_Namespace::StringOpInfo>& string_op_infos) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto map_key =
        generate_translation_map_key(source_proxy->getDictionary()->getDictKey(),
                                     dest_proxy->getDictionary()->getDictKey(),
                                     string_op_infos);
    auto it = str_proxy_union_translation_maps_owned_.find(map_key);
    if (it == str_proxy_union_translation_maps_owned_.end()) {
      it = str_proxy_union_translation_maps_owned_
               .emplace(map_key,
                        source_proxy->buildUnionTranslationMapToOtherProxy(
                            dest_proxy, string_op_infos))
               .first;
    }
    return &it->second;
  }

  const StringOps_Namespace::StringOps* getStringOps(
      const std::vector<StringOps_Namespace::StringOpInfo>& string_op_infos) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto map_key = generate_translation_map_key({}, {}, string_op_infos);
    auto it = string_ops_owned_.find(map_key);
    if (it == string_ops_owned_.end()) {
      it = string_ops_owned_
               .emplace(map_key,
                        std::make_shared<StringOps_Namespace::StringOps>(string_op_infos))
               .first;
    }
    return it->second.get();
  }

  StringDictionaryProxy* getStringDictProxy(const shared::StringDictKey& dict_key) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = str_dict_proxy_owned_.find(dict_key);
    CHECK(it != str_dict_proxy_owned_.end());
    return it->second.get();
  }

  StringDictionaryProxy* getOrAddStringDictProxy(const shared::StringDictKey& dict_key,
                                                 const bool with_generation);

  void addLiteralStringDictProxy(
      std::shared_ptr<StringDictionaryProxy> lit_str_dict_proxy) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    lit_str_dict_proxy_ = lit_str_dict_proxy;
  }

  StringDictionaryProxy* getLiteralStringDictProxy() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return lit_str_dict_proxy_.get();
  }

  const StringDictionaryProxy::IdMap* getOrAddStringProxyTranslationMap(
      const shared::StringDictKey& source_dict_id_in,
      const shared::StringDictKey& dest_dict_id_in,
      const bool with_generation,
      const StringTranslationType translation_map_type,
      const std::vector<StringOps_Namespace::StringOpInfo>& string_op_infos);

  const StringDictionaryProxy::TranslationMap<Datum>*
  getOrAddStringProxyNumericTranslationMap(
      const shared::StringDictKey& source_dict_id_in,
      const bool with_generation,
      const std::vector<StringOps_Namespace::StringOpInfo>& string_op_infos);

  void addColBuffer(const void* col_buffer) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    col_buffers_.push_back(const_cast<void*>(col_buffer));
  }

  ~RowSetMemoryOwner() {
    std::ostringstream oss;
    oss << "Destruct RowSetMemoryOwner attached to Executor-" << executor_id_ << "{\t";
    int allocator_id = 0;
    for (auto const& allocator : allocators_) {
      auto const usedBytes = allocator->bytesUsed();
      if (usedBytes > 0) {
        oss << "allocator-" << allocator_id << ", byteUsed: " << usedBytes << "/"
            << allocator->totalBytes() << "\t";
      }
      ++allocator_id;
    }
    oss << "}";
    allocators_.clear();
    VLOG(1) << oss.str();
    for (auto count_distinct_set : count_distinct_sets_) {
      delete count_distinct_set;
    }
    for (auto varlen_buffer : varlen_buffers_) {
      free(varlen_buffer);
    }
    for (auto varlen_input_buffer : varlen_input_buffers_) {
      CHECK(varlen_input_buffer);
      varlen_input_buffer->unPin();
    }
    for (auto col_buffer : col_buffers_) {
      free(col_buffer);
    }
  }

  std::shared_ptr<RowSetMemoryOwner> cloneStrDictDataOnly() {
    auto rtn = std::make_shared<RowSetMemoryOwner>(arena_block_size_, executor_id_);
    rtn->str_dict_proxy_owned_ = str_dict_proxy_owned_;
    rtn->lit_str_dict_proxy_ = lit_str_dict_proxy_;
    return rtn;
  }

  void setDictionaryGenerations(StringDictionaryGenerations generations) {
    string_dictionary_generations_ = generations;
  }

  StringDictionaryGenerations& getStringDictionaryGenerations() {
    return string_dictionary_generations_;
  }

  quantile::TDigest* initTDigest(size_t thread_idx, ApproxQuantileDescriptor, double q);
  void reserveTDigestMemory(size_t thread_idx, size_t capacity);

  //
  // key/value store for table function intercommunication
  //

  void setTableFunctionMetadata(const char* key,
                                const uint8_t* raw_data,
                                const size_t num_bytes,
                                const TableFunctionMetadataType value_type) {
    MetadataValue metadata_value(num_bytes, value_type);
    std::memcpy(metadata_value.first.data(), raw_data, num_bytes);
    std::lock_guard<std::mutex> lock(table_function_metadata_store_mutex_);
    table_function_metadata_store_[key] = std::move(metadata_value);
  }

  void getTableFunctionMetadata(const char* key,
                                const uint8_t*& raw_data,
                                size_t& num_bytes,
                                TableFunctionMetadataType& value_type) const {
    std::lock_guard<std::mutex> lock(table_function_metadata_store_mutex_);
    auto const itr = table_function_metadata_store_.find(key);
    if (itr == table_function_metadata_store_.end()) {
      throw std::runtime_error("Failed to find Table Function Metadata with key '" +
                               std::string(key) + "'");
    }
    raw_data = itr->second.first.data();
    num_bytes = itr->second.first.size();
    value_type = itr->second.second;
  }

  // Return (index_plus_one, AggMode*)
  // index_plus_one is one-based to avoid conflict with 64-bit null sentinel.
  std::pair<size_t, AggMode*> allocateMode() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    AggMode* const agg_mode = &agg_modes_.emplace_back();
    return {agg_modes_.size(), agg_mode};
  }

  AggMode const* getAggMode(int64_t const ival) const {
    if (ival < 0) {
      constexpr uint64_t mask = (uint64_t(1) << 32) - 1u;
      uint64_t const index_plus_one = mask & ival;  // set in allocateMode() above.
      uint64_t const index = index_plus_one - 1u;
      CHECK_LT(index, agg_modes_.size()) << ival;
      return &agg_modes_[index];
    } else {
      return reinterpret_cast<AggMode*>(ival);
    }
  }

  AggMode* getAggMode(int64_t const ival) {
    return const_cast<AggMode*>(std::as_const(*this).getAggMode(ival));
  }

  void allocateSourceSDToTempSDTransMap(size_t key, int32_t const size) {
    auto [it, success] = source_sd_to_temp_sd_trans_map_.emplace(
        key, std::make_unique<std::vector<int32_t>>(size, -1));
    CHECK(success);
  }

  bool hasSourceSDToTempSDTransMap(size_t key) const {
    return source_sd_to_temp_sd_trans_map_.find(key) !=
           source_sd_to_temp_sd_trans_map_.end();
  }

  const size_t getSourceSDToTempSDTransMapSize(size_t key) const {
    auto it = source_sd_to_temp_sd_trans_map_.find(key);
    CHECK(it != source_sd_to_temp_sd_trans_map_.end());
    return it->second->size();
  }

  int32_t* getSourceSDToTempSDTransMap(size_t key) const {
    auto it = source_sd_to_temp_sd_trans_map_.find(key);
    CHECK(it != source_sd_to_temp_sd_trans_map_.end());
    return it->second->data();
  }

  shared::StringDictKey getTempDictionaryKey(int32_t db_id,
                                             const Analyzer::StringOper& string_oper) {
    const auto string_oper_str = string_oper.toString();
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = string_dict_key_by_string_oper_.find(string_oper_str);
    if (it != string_dict_key_by_string_oper_.end()) {
      return it->second;
    }
    shared::StringDictKey dict_key{db_id > 0 ? -db_id : -1, next_temp_dict_id_++};
    string_dict_key_by_string_oper_[string_oper_str] = dict_key;
    return dict_key;
  }

 private:
  int8_t* allocateUnlocked(const size_t num_bytes,
                           const size_t thread_idx,
                           bool skip_allocation_log = false) {
    if (g_allow_memory_status_log && !skip_allocation_log) {
      VLOG(1) << "Try to allocate CPU memory: " << num_bytes << " bytes (THREAD-"
              << thread_idx << ")";
    }
    auto allocator = allocators_[thread_idx].get();
    return reinterpret_cast<int8_t*>(allocator->allocate(num_bytes));
  }

  void initAndAddCountDistinctBuffer(int8_t* buffer, size_t num_bytes) {
    std::memset(buffer, 0, num_bytes);
    addCountDistinctBuffer(buffer, num_bytes, /*physical_buffer=*/true);
  }

  struct CountDistinctBitmapBuffer {
    int8_t* ptr;
    const size_t size;
    const bool physical_buffer;
  };

  std::vector<CountDistinctBitmapBuffer> count_distinct_bitmaps_;
  std::vector<CountDistinctSet*> count_distinct_sets_;
  std::vector<int64_t*> non_owned_group_by_buffers_;
  std::unordered_set<void*> varlen_buffers_;
  std::list<std::string> strings_;
  std::list<std::vector<int64_t>> arrays_;
  std::unordered_map<shared::StringDictKey, std::shared_ptr<StringDictionaryProxy>>
      str_dict_proxy_owned_;
  std::map<std::string, StringDictionaryProxy::IdMap>
      str_proxy_intersection_translation_maps_owned_;
  std::map<std::string, StringDictionaryProxy::IdMap>
      str_proxy_union_translation_maps_owned_;
  std::map<std::string, StringDictionaryProxy::TranslationMap<Datum>>
      str_proxy_numeric_translation_maps_owned_;
  std::shared_ptr<StringDictionaryProxy> lit_str_dict_proxy_;
  StringDictionaryGenerations string_dictionary_generations_;

  std::vector<void*> col_buffers_;
  std::vector<Data_Namespace::AbstractBuffer*> varlen_input_buffers_;

  using TDigestAllocator = FastAllocator<int8_t>;
  std::deque<TDigestAllocator> t_digest_allocators_;
  std::vector<std::unique_ptr<quantile::TDigest>> t_digests_;

  std::map<std::string, std::shared_ptr<StringOps_Namespace::StringOps>>
      string_ops_owned_;
  std::deque<AggMode> agg_modes_;  // references must remain valid on emplace_back().

  size_t arena_block_size_;  // for cloning
  std::vector<std::unique_ptr<Arena>> allocators_;

  using CountDistinctBufferFastAllocator = FastAllocator<int8_t>;
  std::vector<std::unique_ptr<CountDistinctBufferFastAllocator>>
      count_distinct_buffer_fast_allocators_;
  std::unordered_map<size_t, std::unique_ptr<std::vector<int32_t>>>
      source_sd_to_temp_sd_trans_map_;

  size_t executor_id_;
  int32_t next_temp_dict_id_;
  std::map<std::string, shared::StringDictKey> string_dict_key_by_string_oper_;

  mutable std::mutex state_mutex_;

  using MetadataValue = std::pair<std::vector<uint8_t>, TableFunctionMetadataType>;
  std::map<std::string, MetadataValue> table_function_metadata_store_;
  mutable std::mutex table_function_metadata_store_mutex_;

  friend class ResultSet;
  friend class QueryExecutionContext;
};
