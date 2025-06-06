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

#include "QueryMemoryInitializer.h"
#include "Execute.h"
#include "GpuInitGroups.h"
#include "Logger/Logger.h"
#include "OutputBufferInitialization.h"
#include "QueryEngine/QueryEngine.h"
#include "Shared/checked_alloc.h"
#include "StreamingTopN.h"
#include "Utils/FlatBuffer.h"

// 8 GB, the limit of perfect hash group by under normal conditions
int64_t g_bitmap_memory_limit{8LL * 1000 * 1000 * 1000};

namespace {

struct AddNbytes {
  size_t const entry_count;
  size_t operator()(size_t const sum, ApproxQuantileDescriptor const aqd) const {
    return sum +
           entry_count * quantile::TDigest::nbytes(aqd.buffer_size, aqd.centroids_size);
  }
};

inline void check_total_bitmap_memory(const QueryMemoryDescriptor& query_mem_desc) {
  const size_t groups_buffer_entry_count = query_mem_desc.getEntryCount();
  checked_int64_t total_bytes_per_group = 0;
  const size_t num_count_distinct_descs =
      query_mem_desc.getCountDistinctDescriptorsSize();
  for (size_t i = 0; i < num_count_distinct_descs; i++) {
    const auto count_distinct_desc = query_mem_desc.getCountDistinctDescriptor(i);
    if (count_distinct_desc.impl_type_ != CountDistinctImplType::Bitmap) {
      continue;
    }
    total_bytes_per_group += count_distinct_desc.bitmapPaddedSizeBytes();
  }
  int64_t total_bytes{0};
  // Using OutOfHostMemory until we can verify that SlabTooBig would also be properly
  // caught
  try {
    total_bytes = static_cast<int64_t>(total_bytes_per_group * groups_buffer_entry_count);
  } catch (...) {
    // Absurd amount of memory, merely computing the number of bits overflows int64_t.
    // Don't bother to report the real amount, this is unlikely to ever happen.
    throw OutOfHostMemory(std::numeric_limits<int64_t>::max() / 8);
  }
  if (total_bytes >= g_bitmap_memory_limit) {
    throw OutOfHostMemory(total_bytes);
  }
}

std::pair<int64_t*, bool> alloc_group_by_buffer(
    const size_t numBytes,
    RenderAllocatorMap* render_allocator_map,
    const size_t thread_idx,
    RowSetMemoryOwner* mem_owner,
    const bool reuse_existing_buffer_for_thread) {
  if (render_allocator_map) {
    // NOTE(adb): If we got here, we are performing an in-situ rendering query and are not
    // using CUDA buffers. Therefore we need to allocate result set storage using CPU
    // memory.
    const auto gpu_idx = 0;  // Only 1 GPU supported in CUDA-disabled rendering mode
    auto render_allocator_ptr = render_allocator_map->getRenderAllocator(gpu_idx);
    return std::make_pair(
        reinterpret_cast<int64_t*>(render_allocator_ptr->alloc(numBytes)), false);
  } else if (reuse_existing_buffer_for_thread) {
    return mem_owner->allocateCachedGroupByBuffer(numBytes, thread_idx);
  }
  return std::make_pair(
      reinterpret_cast<int64_t*>(mem_owner->allocate(numBytes, thread_idx)), false);
}

inline int64_t get_consistent_frag_size(const std::vector<uint64_t>& frag_offsets) {
  if (frag_offsets.size() < 2) {
    return int64_t(-1);
  }
  const auto frag_size = frag_offsets[1] - frag_offsets[0];
  for (size_t i = 2; i < frag_offsets.size(); ++i) {
    const auto curr_size = frag_offsets[i] - frag_offsets[i - 1];
    if (curr_size != frag_size) {
      return int64_t(-1);
    }
  }
  return !frag_size ? std::numeric_limits<int64_t>::max()
                    : static_cast<int64_t>(frag_size);
}

inline std::vector<int64_t> get_consistent_frags_sizes(
    const std::vector<std::vector<uint64_t>>& frag_offsets) {
  if (frag_offsets.empty()) {
    return {};
  }
  std::vector<int64_t> frag_sizes;
  for (size_t tab_idx = 0; tab_idx < frag_offsets[0].size(); ++tab_idx) {
    std::vector<uint64_t> tab_offs;
    for (auto& offsets : frag_offsets) {
      tab_offs.push_back(offsets[tab_idx]);
    }
    frag_sizes.push_back(get_consistent_frag_size(tab_offs));
  }
  return frag_sizes;
}

inline std::vector<int64_t> get_consistent_frags_sizes(
    const std::vector<Analyzer::Expr*>& target_exprs,
    const std::vector<int64_t>& table_frag_sizes) {
  std::vector<int64_t> col_frag_sizes;
  for (auto expr : target_exprs) {
    if (const auto col_var = dynamic_cast<Analyzer::ColumnVar*>(expr)) {
      if (col_var->get_rte_idx() < 0) {
        CHECK_EQ(-1, col_var->get_rte_idx());
        col_frag_sizes.push_back(int64_t(-1));
      } else {
        col_frag_sizes.push_back(table_frag_sizes[col_var->get_rte_idx()]);
      }
    } else {
      col_frag_sizes.push_back(int64_t(-1));
    }
  }
  return col_frag_sizes;
}

inline std::vector<std::vector<int64_t>> get_col_frag_offsets(
    const std::vector<Analyzer::Expr*>& target_exprs,
    const std::vector<std::vector<uint64_t>>& table_frag_offsets) {
  std::vector<std::vector<int64_t>> col_frag_offsets;
  for (auto& table_offsets : table_frag_offsets) {
    std::vector<int64_t> col_offsets;
    for (auto expr : target_exprs) {
      if (const auto col_var = dynamic_cast<Analyzer::ColumnVar*>(expr)) {
        if (col_var->get_rte_idx() < 0) {
          CHECK_EQ(-1, col_var->get_rte_idx());
          col_offsets.push_back(int64_t(-1));
        } else {
          CHECK_LT(static_cast<size_t>(col_var->get_rte_idx()), table_offsets.size());
          col_offsets.push_back(
              static_cast<int64_t>(table_offsets[col_var->get_rte_idx()]));
        }
      } else {
        col_offsets.push_back(int64_t(-1));
      }
    }
    col_frag_offsets.push_back(col_offsets);
  }
  return col_frag_offsets;
}

// Return the RelAlg input index of outer_table_id based on ra_exe_unit.input_descs.
// Used by UNION queries to get the target_exprs corresponding to the current subquery.
int get_input_idx(RelAlgExecutionUnit const& ra_exe_unit,
                  const shared::TableKey& outer_table_key) {
  auto match_table_key = [=](auto& desc) {
    return outer_table_key == desc.getTableKey();
  };
  auto& input_descs = ra_exe_unit.input_descs;
  auto itr = std::find_if(input_descs.begin(), input_descs.end(), match_table_key);
  return itr == input_descs.end() ? 0 : itr->getNestLevel();
}

void check_count_distinct_expr_metadata(const QueryMemoryDescriptor& query_mem_desc,
                                        const RelAlgExecutionUnit& ra_exe_unit) {
  const size_t agg_col_count{query_mem_desc.getSlotCount()};
  CHECK_GE(agg_col_count, ra_exe_unit.target_exprs.size());
  for (size_t target_idx = 0; target_idx < ra_exe_unit.target_exprs.size();
       ++target_idx) {
    const auto target_expr = ra_exe_unit.target_exprs[target_idx];
    const auto agg_info = get_target_info(target_expr, g_bigint_count);
    if (is_distinct_target(agg_info)) {
      CHECK(agg_info.is_agg &&
            (agg_info.agg_kind == kCOUNT || agg_info.agg_kind == kCOUNT_IF ||
             agg_info.agg_kind == kAPPROX_COUNT_DISTINCT));
      CHECK(!agg_info.sql_type.is_varlen());
      const size_t agg_col_idx = query_mem_desc.getSlotIndexForSingleSlotCol(target_idx);
      CHECK_LT(static_cast<size_t>(agg_col_idx), agg_col_count);
      CHECK_EQ(static_cast<size_t>(query_mem_desc.getLogicalSlotWidthBytes(agg_col_idx)),
               sizeof(int64_t));
      const auto& count_distinct_desc =
          query_mem_desc.getCountDistinctDescriptor(target_idx);
      CHECK(count_distinct_desc.impl_type_ != CountDistinctImplType::Invalid);
    }
  }
}

QueryMemoryInitializer::TargetAggOpsMetadata collect_target_expr_metadata(
    const QueryMemoryDescriptor& query_mem_desc,
    const RelAlgExecutionUnit& ra_exe_unit) {
  QueryMemoryInitializer::TargetAggOpsMetadata agg_op_metadata;
  if (!query_mem_desc.countDistinctDescriptorsLogicallyEmpty()) {
    agg_op_metadata.has_count_distinct = true;
  }
  std::for_each(
      ra_exe_unit.target_exprs.begin(),
      ra_exe_unit.target_exprs.end(),
      [&agg_op_metadata](const Analyzer::Expr* expr) {
        if (auto const* agg_expr = dynamic_cast<Analyzer::AggExpr const*>(expr)) {
          if (agg_expr->get_aggtype() == kMODE) {
            agg_op_metadata.has_mode = true;
          } else if (agg_expr->get_aggtype() == kAPPROX_QUANTILE) {
            agg_op_metadata.has_tdigest = true;
          }
        }
      });
  return agg_op_metadata;
}

}  // namespace

// Row-based execution constructor
QueryMemoryInitializer::QueryMemoryInitializer(
    const RelAlgExecutionUnit& ra_exe_unit,
    const QueryMemoryDescriptor& query_mem_desc,
    const int device_id,
    const ExecutorDeviceType device_type,
    const ExecutorDispatchMode dispatch_mode,
    const bool output_columnar,
    const bool sort_on_gpu,
    const shared::TableKey& outer_table_key,
    const int64_t num_rows,
    const std::vector<std::vector<const int8_t*>>& col_buffers,
    const std::vector<std::vector<uint64_t>>& frag_offsets,
    RenderAllocatorMap* render_allocator_map,
    RenderInfo* render_info,
    std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
    DeviceAllocator* device_allocator,
    const size_t thread_idx,
    const Executor* executor)
    : num_rows_(num_rows)
    , row_set_mem_owner_(row_set_mem_owner)
    , init_agg_vals_(executor->plan_state_->init_agg_vals_)
    , num_buffers_(computeNumberOfBuffers(query_mem_desc, device_type, executor))
    , varlen_output_buffer_(0)
    , varlen_output_buffer_host_ptr_(nullptr)
    , count_distinct_bitmap_device_mem_ptr_(0)
    , count_distinct_bitmap_mem_size_(0)
    , count_distinct_bitmap_host_crt_ptr_(nullptr)
    , count_distinct_bitmap_host_mem_ptr_(nullptr)
    , device_allocator_(device_allocator)
    , thread_idx_(thread_idx) {
  CHECK(!sort_on_gpu || output_columnar);
  executor->logSystemCPUMemoryStatus("Before Query Memory Initialization", thread_idx);

  const auto& consistent_frag_sizes = get_consistent_frags_sizes(frag_offsets);
  if (consistent_frag_sizes.empty()) {
    // No fragments in the input, no underlying buffers will be needed.
    return;
  }

  TargetAggOpsMetadata agg_op_metadata =
      collect_target_expr_metadata(query_mem_desc, ra_exe_unit);
  if (agg_op_metadata.has_count_distinct) {
    check_count_distinct_expr_metadata(query_mem_desc, ra_exe_unit);
    if (!ra_exe_unit.use_bump_allocator) {
      check_total_bitmap_memory(query_mem_desc);
    }
    agg_op_metadata.count_distinct_buf_size =
        calculateCountDistinctBufferSize(query_mem_desc, ra_exe_unit);
    size_t total_buffer_size{0};
    for (auto buffer_size : agg_op_metadata.count_distinct_buf_size) {
      if (buffer_size > 0) {
        total_buffer_size += buffer_size;
      }
    }
    total_buffer_size *= query_mem_desc.getEntryCount();
    if (device_type == ExecutorDeviceType::GPU) {
      allocateCountDistinctGpuMem(query_mem_desc);
      row_set_mem_owner_->initCountDistinctBufferForFastAllocation(
          count_distinct_bitmap_host_crt_ptr_, total_buffer_size, thread_idx_);
    } else {
      row_set_mem_owner_->allocAndInitCountDistinctBufferForFastAllocator(
          total_buffer_size, thread_idx_);
    }
  }

  if (agg_op_metadata.has_tdigest) {
    auto const& descs = query_mem_desc.getApproxQuantileDescriptors();
    // Pre-allocate all TDigest memory for this thread.
    AddNbytes const add_nbytes{query_mem_desc.getEntryCount()};
    size_t const capacity =
        std::accumulate(descs.begin(), descs.end(), size_t(0), add_nbytes);
    VLOG(2) << "row_set_mem_owner_->reserveTDigestMemory(" << thread_idx_ << ','
            << capacity << ") query_mem_desc.getEntryCount()("
            << query_mem_desc.getEntryCount() << ')';
    row_set_mem_owner_->reserveTDigestMemory(thread_idx_, capacity);
  }
  allocateModeMem(device_type, query_mem_desc, executor, device_id);

  if (render_allocator_map || !query_mem_desc.isGroupBy()) {
    if (agg_op_metadata.has_count_distinct) {
      fastAllocateCountDistinctBuffers(query_mem_desc, ra_exe_unit);
    }
    if (agg_op_metadata.has_mode) {
      allocateModeBuffer(query_mem_desc, ra_exe_unit);
    }
    if (agg_op_metadata.has_tdigest) {
      allocateTDigestsBuffer(query_mem_desc, ra_exe_unit);
    }
    if (render_info && render_info->useCudaBuffers()) {
      return;
    }
  }

  if (query_mem_desc.isGroupBy()) {
    if (agg_op_metadata.has_mode) {
      agg_op_metadata.mode_index_set =
          initializeModeIndexSet(query_mem_desc, ra_exe_unit);
    }
    if (agg_op_metadata.has_tdigest) {
      agg_op_metadata.quantile_params =
          initializeQuantileParams(query_mem_desc, ra_exe_unit);
    }
  }

  if (ra_exe_unit.estimator) {
    return;
  }

  const auto thread_count = device_type == ExecutorDeviceType::GPU
                                ? executor->blockSize() * executor->gridSize()
                                : 1;

  size_t group_buffer_size{0};
  if (ra_exe_unit.use_bump_allocator) {
    // For kernel per fragment execution, just allocate a buffer equivalent to the size of
    // the fragment
    if (dispatch_mode == ExecutorDispatchMode::KernelPerFragment) {
      group_buffer_size = num_rows * query_mem_desc.getRowSize();
    } else {
      // otherwise, allocate a GPU buffer equivalent to the maximum GPU allocation size
      group_buffer_size = g_max_memory_allocation_size / query_mem_desc.getRowSize();
    }
  } else {
    group_buffer_size =
        query_mem_desc.getBufferSizeBytes(ra_exe_unit, thread_count, device_type);
  }
  CHECK_GE(group_buffer_size, size_t(0));

  const auto group_buffers_count = !query_mem_desc.isGroupBy() ? 1 : num_buffers_;
  int64_t* group_by_buffer_template{nullptr};

  if (!query_mem_desc.lazyInitGroups(device_type) && group_buffers_count > 1) {
    group_by_buffer_template = reinterpret_cast<int64_t*>(
        row_set_mem_owner_->allocate(group_buffer_size, thread_idx_));
    initGroupByBuffer(group_by_buffer_template,
                      ra_exe_unit,
                      query_mem_desc,
                      agg_op_metadata,
                      device_type,
                      output_columnar,
                      executor);
  }

  if (query_mem_desc.interleavedBins(device_type)) {
    CHECK(query_mem_desc.hasKeylessHash());
  }

  const auto step = device_type == ExecutorDeviceType::GPU &&
                            query_mem_desc.threadsShareMemory() &&
                            query_mem_desc.isGroupBy()
                        ? executor->blockSize()
                        : size_t(1);
  const auto index_buffer_qw = device_type == ExecutorDeviceType::GPU && sort_on_gpu &&
                                       query_mem_desc.hasKeylessHash()
                                   ? query_mem_desc.getEntryCount()
                                   : size_t(0);
  const auto actual_group_buffer_size =
      group_buffer_size + index_buffer_qw * sizeof(int64_t);
  CHECK_GE(actual_group_buffer_size, group_buffer_size);

  if (query_mem_desc.hasVarlenOutput()) {
    const auto varlen_buffer_elem_size_opt = query_mem_desc.varlenOutputBufferElemSize();
    CHECK(varlen_buffer_elem_size_opt);  // TODO(adb): relax
    auto const varlen_buffer_sz =
        query_mem_desc.getEntryCount() * varlen_buffer_elem_size_opt.value();
    auto varlen_output_buffer =
        reinterpret_cast<int64_t*>(row_set_mem_owner_->allocate(varlen_buffer_sz));
    num_buffers_ += 1;
    group_by_buffers_.push_back(varlen_output_buffer);
  }

  if (query_mem_desc.threadsCanReuseGroupByBuffers()) {
    // Sanity checks, intra-thread buffer reuse should only
    // occur on CPU for group-by queries, which also means
    // that only one group-by buffer should be allocated
    // (multiple-buffer allocation only occurs for GPU)
    CHECK(device_type == ExecutorDeviceType::CPU);
    CHECK(query_mem_desc.isGroupBy());
    CHECK_EQ(group_buffers_count, size_t(1));
  }

  // Group-by buffer reuse assumes 1 group-by-buffer per query step
  // Multiple group-by-buffers should only be used on GPU,
  // whereas buffer reuse only is done on CPU
  CHECK(group_buffers_count <= 1 || !query_mem_desc.threadsCanReuseGroupByBuffers());
  for (size_t i = 0; i < group_buffers_count; i += step) {
    auto group_by_info =
        alloc_group_by_buffer(actual_group_buffer_size,
                              render_allocator_map,
                              thread_idx_,
                              row_set_mem_owner_.get(),
                              query_mem_desc.threadsCanReuseGroupByBuffers());

    auto group_by_buffer = group_by_info.first;
    const bool was_cached = group_by_info.second;
    if (!was_cached) {
      if (!query_mem_desc.lazyInitGroups(device_type)) {
        if (group_by_buffer_template) {
          memcpy(group_by_buffer + index_buffer_qw,
                 group_by_buffer_template,
                 group_buffer_size);
        } else {
          initGroupByBuffer(group_by_buffer + index_buffer_qw,
                            ra_exe_unit,
                            query_mem_desc,
                            agg_op_metadata,
                            device_type,
                            output_columnar,
                            executor);
        }
      }
    }

    size_t old_size = group_by_buffers_.size();
    group_by_buffers_.resize(old_size + std::max(size_t(1), step), nullptr);
    group_by_buffers_[old_size] = group_by_buffer;

    const bool use_target_exprs_union =
        ra_exe_unit.union_all && get_input_idx(ra_exe_unit, outer_table_key);
    const auto& target_exprs = use_target_exprs_union ? ra_exe_unit.target_exprs_union
                                                      : ra_exe_unit.target_exprs;
    const auto column_frag_offsets = get_col_frag_offsets(target_exprs, frag_offsets);
    const auto column_frag_sizes =
        get_consistent_frags_sizes(target_exprs, consistent_frag_sizes);

    old_size = result_sets_.size();
    result_sets_.resize(old_size + std::max(size_t(1), step));
    result_sets_[old_size] =
        std::make_unique<ResultSet>(target_exprs_to_infos(target_exprs, query_mem_desc),
                                    executor->getColLazyFetchInfo(target_exprs),
                                    col_buffers,
                                    column_frag_offsets,
                                    column_frag_sizes,
                                    device_type,
                                    device_id,
                                    thread_idx,
                                    ResultSet::fixupQueryMemoryDescriptor(query_mem_desc),
                                    row_set_mem_owner_,
                                    executor->blockSize(),
                                    executor->gridSize());
    if (device_type == ExecutorDeviceType::GPU) {
      result_sets_[old_size]->setCudaAllocator(executor, device_id);
      result_sets_[old_size]->setCudaStream(executor, device_id);
    }
    result_sets_[old_size]->allocateStorage(reinterpret_cast<int8_t*>(group_by_buffer),
                                            executor->plan_state_->init_agg_vals_,
                                            getVarlenOutputInfo());
  }
}

// Table functions execution constructor
QueryMemoryInitializer::QueryMemoryInitializer(
    const TableFunctionExecutionUnit& exe_unit,
    const QueryMemoryDescriptor& query_mem_desc,
    const int device_id,
    const ExecutorDeviceType device_type,
    const int64_t num_rows,
    const std::vector<std::vector<const int8_t*>>& col_buffers,
    const std::vector<std::vector<uint64_t>>& frag_offsets,
    std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
    DeviceAllocator* device_allocator,
    const Executor* executor)
    : num_rows_(num_rows)
    , row_set_mem_owner_(row_set_mem_owner)
    , init_agg_vals_(init_agg_val_vec(exe_unit.target_exprs, {}, query_mem_desc))
    , num_buffers_(1)
    , varlen_output_buffer_(0)
    , varlen_output_buffer_host_ptr_(nullptr)
    , count_distinct_bitmap_device_mem_ptr_(0)
    , count_distinct_bitmap_mem_size_(0)
    , count_distinct_bitmap_host_crt_ptr_(nullptr)
    , count_distinct_bitmap_host_mem_ptr_(nullptr)
    , device_allocator_(device_allocator)
    , thread_idx_(0) {
  // Table functions output columnar, basically treat this as a projection
  const auto& consistent_frag_sizes = get_consistent_frags_sizes(frag_offsets);
  if (consistent_frag_sizes.empty()) {
    // No fragments in the input, no underlying buffers will be needed.
    return;
  }

  const size_t num_columns =
      query_mem_desc.getBufferColSlotCount();  // shouldn't we use getColCount() ???
  size_t total_group_by_buffer_size{0};
  for (size_t i = 0; i < num_columns; ++i) {
    auto ti = exe_unit.target_exprs[i]->get_type_info();
    if (ti.usesFlatBuffer()) {
      // See TableFunctionManager.h for info regarding flatbuffer
      // memory managment.
      auto slot_idx = query_mem_desc.getSlotIndexForSingleSlotCol(i);
      CHECK(query_mem_desc.checkSlotUsesFlatBufferFormat(slot_idx));
      checked_int64_t flatbuffer_size = query_mem_desc.getFlatBufferSize(slot_idx);
      try {
        total_group_by_buffer_size = align_to_int64(
            static_cast<int64_t>(total_group_by_buffer_size + flatbuffer_size));
      } catch (...) {
        throw OutOfHostMemory(std::numeric_limits<int64_t>::max() / 8);
      }
    } else {
      const checked_int64_t col_width = ti.get_size();
      try {
        const checked_int64_t group_buffer_size = col_width * num_rows_;
        total_group_by_buffer_size = align_to_int64(
            static_cast<int64_t>(group_buffer_size + total_group_by_buffer_size));
      } catch (...) {
        throw OutOfHostMemory(std::numeric_limits<int64_t>::max() / 8);
      }
    }
  }

#ifdef __SANITIZE_ADDRESS__
  // AddressSanitizer will reject allocation sizes above 1 TiB
#define MAX_BUFFER_SIZE 0x10000000000ll
#else
  // otherwise, we'll set the limit to 16 TiB, feel free to increase
  // the limit if needed
#define MAX_BUFFER_SIZE 0x100000000000ll
#endif

  if (total_group_by_buffer_size >= MAX_BUFFER_SIZE) {
    throw OutOfHostMemory(total_group_by_buffer_size);
  }

  CHECK_EQ(num_buffers_, size_t(1));
  auto group_by_buffer = alloc_group_by_buffer(total_group_by_buffer_size,
                                               nullptr,
                                               thread_idx_,
                                               row_set_mem_owner.get(),
                                               false)
                             .first;
  group_by_buffers_.push_back(group_by_buffer);

  const auto column_frag_offsets =
      get_col_frag_offsets(exe_unit.target_exprs, frag_offsets);
  const auto column_frag_sizes =
      get_consistent_frags_sizes(exe_unit.target_exprs, consistent_frag_sizes);
  result_sets_.emplace_back(
      new ResultSet(target_exprs_to_infos(exe_unit.target_exprs, query_mem_desc),
                    /*col_lazy_fetch_info=*/{},
                    col_buffers,
                    column_frag_offsets,
                    column_frag_sizes,
                    device_type,
                    device_id,
                    -1, /*thread_idx*/
                    ResultSet::fixupQueryMemoryDescriptor(query_mem_desc),
                    row_set_mem_owner_,
                    executor->blockSize(),
                    executor->gridSize()));
  result_sets_.back()->allocateStorage(reinterpret_cast<int8_t*>(group_by_buffer),
                                       init_agg_vals_);
}

void QueryMemoryInitializer::initGroupByBuffer(
    int64_t* buffer,
    const RelAlgExecutionUnit& ra_exe_unit,
    const QueryMemoryDescriptor& query_mem_desc,
    TargetAggOpsMetadata& agg_op_metadata,
    const ExecutorDeviceType device_type,
    const bool output_columnar,
    const Executor* executor) {
  if (output_columnar) {
    initColumnarGroups(query_mem_desc, buffer, init_agg_vals_, executor, ra_exe_unit);
  } else {
    auto rows_ptr = buffer;
    auto actual_entry_count = query_mem_desc.getEntryCount();
    const auto thread_count = device_type == ExecutorDeviceType::GPU
                                  ? executor->blockSize() * executor->gridSize()
                                  : 1;
    auto warp_size =
        query_mem_desc.interleavedBins(device_type) ? executor->warpSize() : 1;
    if (query_mem_desc.useStreamingTopN()) {
      const auto node_count_size = thread_count * sizeof(int64_t);
      memset(rows_ptr, 0, node_count_size);
      const auto n =
          ra_exe_unit.sort_info.offset + ra_exe_unit.sort_info.limit.value_or(0);
      const auto rows_offset = streaming_top_n::get_rows_offset_of_heaps(n, thread_count);
      memset(rows_ptr + thread_count, -1, rows_offset - node_count_size);
      rows_ptr += rows_offset / sizeof(int64_t);
      actual_entry_count = n * thread_count;
      warp_size = 1;
    }
    initRowGroups(query_mem_desc,
                  rows_ptr,
                  init_agg_vals_,
                  agg_op_metadata,
                  actual_entry_count,
                  warp_size,
                  executor,
                  ra_exe_unit);
  }
}

void QueryMemoryInitializer::initRowGroups(const QueryMemoryDescriptor& query_mem_desc,
                                           int64_t* groups_buffer,
                                           const std::vector<int64_t>& init_vals,
                                           TargetAggOpsMetadata& agg_op_metadata,
                                           const int32_t groups_buffer_entry_count,
                                           const size_t warp_size,
                                           const Executor* executor,
                                           const RelAlgExecutionUnit& ra_exe_unit) {
  const size_t key_count{query_mem_desc.getGroupbyColCount()};
  const size_t row_size{query_mem_desc.getRowSize()};
  const size_t col_base_off{query_mem_desc.getColOffInBytes(0)};

  auto buffer_ptr = reinterpret_cast<int8_t*>(groups_buffer);
  const auto query_mem_desc_fixedup =
      ResultSet::fixupQueryMemoryDescriptor(query_mem_desc);
  auto const key_sz = query_mem_desc.getEffectiveKeyWidth();
  // not COUNT DISTINCT / APPROX_COUNT_DISTINCT / APPROX_QUANTILE
  // we use the default implementation in those agg ops
  if (!(agg_op_metadata.has_count_distinct || agg_op_metadata.has_mode ||
        agg_op_metadata.has_tdigest) &&
      g_optimize_row_initialization) {
    std::vector<int8_t> sample_row(row_size - col_base_off);
    auto const num_available_cpu_threads =
        std::min(query_mem_desc.getAvailableCpuThreads(),
                 static_cast<size_t>(std::max(cpu_threads(), 1)));
    tbb::task_arena initialization_arena(num_available_cpu_threads);

    initColumnsPerRow(
        query_mem_desc_fixedup, sample_row.data(), init_vals, agg_op_metadata);

    if (query_mem_desc.hasKeylessHash()) {
      CHECK(warp_size >= 1);
      CHECK(key_count == 1 || warp_size == 1);
      initialization_arena.execute([&] {
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, groups_buffer_entry_count * warp_size),
            [&](const tbb::blocked_range<size_t>& r) {
              auto cur_row_buf = buffer_ptr + (row_size * r.begin());
              for (size_t i = r.begin(); i != r.end(); ++i, cur_row_buf += row_size) {
                memcpy(cur_row_buf + col_base_off, sample_row.data(), sample_row.size());
              }
            });
      });
      return;
    }
    initialization_arena.execute([&] {
      tbb::parallel_for(
          tbb::blocked_range<size_t>(0, groups_buffer_entry_count),
          [&](const tbb::blocked_range<size_t>& r) {
            auto cur_row_buf = buffer_ptr + (row_size * r.begin());
            for (size_t i = r.begin(); i != r.end(); ++i, cur_row_buf += row_size) {
              memcpy(cur_row_buf + col_base_off, sample_row.data(), sample_row.size());
              result_set::fill_empty_key(cur_row_buf, key_count, key_sz);
            }
          });
    });
  } else {
    // todo(yoonmin): allow parallelization of `initColumnsPerRow`
    if (query_mem_desc.hasKeylessHash()) {
      CHECK(warp_size >= 1);
      CHECK(key_count == 1 || warp_size == 1);
      for (size_t warp_idx = 0; warp_idx < warp_size; ++warp_idx) {
        for (size_t bin = 0; bin < static_cast<size_t>(groups_buffer_entry_count);
             ++bin, buffer_ptr += row_size) {
          initColumnsPerRow(query_mem_desc_fixedup,
                            &buffer_ptr[col_base_off],
                            init_vals,
                            agg_op_metadata);
        }
      }
      return;
    }

    for (size_t bin = 0; bin < static_cast<size_t>(groups_buffer_entry_count);
         ++bin, buffer_ptr += row_size) {
      result_set::fill_empty_key(
          buffer_ptr, key_count, query_mem_desc.getEffectiveKeyWidth());
      initColumnsPerRow(
          query_mem_desc_fixedup, &buffer_ptr[col_base_off], init_vals, agg_op_metadata);
    }
  }
}

namespace {

template <typename T>
int8_t* initColumnarBuffer(T* buffer_ptr, const T init_val, const uint32_t entry_count) {
  static_assert(sizeof(T) <= sizeof(int64_t), "Unsupported template type");
  for (uint32_t i = 0; i < entry_count; ++i) {
    buffer_ptr[i] = init_val;
  }
  return reinterpret_cast<int8_t*>(buffer_ptr + entry_count);
}

}  // namespace

void QueryMemoryInitializer::initColumnarGroups(
    const QueryMemoryDescriptor& query_mem_desc,
    int64_t* groups_buffer,
    const std::vector<int64_t>& init_vals,
    const Executor* executor,
    const RelAlgExecutionUnit& ra_exe_unit) {
  CHECK(groups_buffer);

  for (const auto target_expr : ra_exe_unit.target_exprs) {
    const auto agg_info = get_target_info(target_expr, g_bigint_count);
    CHECK(!is_distinct_target(agg_info));
  }
  const int32_t agg_col_count = query_mem_desc.getSlotCount();
  auto buffer_ptr = reinterpret_cast<int8_t*>(groups_buffer);

  const auto groups_buffer_entry_count = query_mem_desc.getEntryCount();
  if (!query_mem_desc.hasKeylessHash()) {
    const size_t key_count{query_mem_desc.getGroupbyColCount()};
    for (size_t i = 0; i < key_count; ++i) {
      buffer_ptr = initColumnarBuffer<int64_t>(reinterpret_cast<int64_t*>(buffer_ptr),
                                               EMPTY_KEY_64,
                                               groups_buffer_entry_count);
    }
  }

  if (query_mem_desc.getQueryDescriptionType() != QueryDescriptionType::Projection) {
    // initializing all aggregate columns:
    int32_t init_val_idx = 0;
    for (int32_t i = 0; i < agg_col_count; ++i) {
      if (query_mem_desc.getPaddedSlotWidthBytes(i) > 0) {
        CHECK_LT(static_cast<size_t>(init_val_idx), init_vals.size());
        switch (query_mem_desc.getPaddedSlotWidthBytes(i)) {
          case 1:
            buffer_ptr = initColumnarBuffer<int8_t>(
                buffer_ptr, init_vals[init_val_idx++], groups_buffer_entry_count);
            break;
          case 2:
            buffer_ptr =
                initColumnarBuffer<int16_t>(reinterpret_cast<int16_t*>(buffer_ptr),
                                            init_vals[init_val_idx++],
                                            groups_buffer_entry_count);
            break;
          case 4:
            buffer_ptr =
                initColumnarBuffer<int32_t>(reinterpret_cast<int32_t*>(buffer_ptr),
                                            init_vals[init_val_idx++],
                                            groups_buffer_entry_count);
            break;
          case 8:
            buffer_ptr =
                initColumnarBuffer<int64_t>(reinterpret_cast<int64_t*>(buffer_ptr),
                                            init_vals[init_val_idx++],
                                            groups_buffer_entry_count);
            break;
          case 0:
            break;
          default:
            CHECK(false);
        }

        buffer_ptr = align_to_int64(buffer_ptr);
      }
    }
  }
}

void QueryMemoryInitializer::initColumnsPerRow(
    const QueryMemoryDescriptor& query_mem_desc,
    int8_t* row_ptr,
    const std::vector<int64_t>& init_vals,
    const TargetAggOpsMetadata& agg_op_metadata) {
  int8_t* col_ptr = row_ptr;
  size_t init_vec_idx = 0;
  size_t approx_quantile_descriptors_idx = 0;
  for (size_t col_idx = 0; col_idx < query_mem_desc.getSlotCount();
       col_ptr += query_mem_desc.getNextColOffInBytesRowOnly(col_ptr, col_idx++)) {
    int64_t init_val{0};
    if (query_mem_desc.isGroupBy()) {
      if (agg_op_metadata.has_count_distinct &&
          agg_op_metadata.count_distinct_buf_size[col_idx]) {
        // COUNT DISTINCT / APPROX_COUNT_DISTINCT
        // create a data structure for count_distinct operator per entries
        const int64_t bm_sz{agg_op_metadata.count_distinct_buf_size[col_idx]};
        CHECK_EQ(static_cast<size_t>(query_mem_desc.getPaddedSlotWidthBytes(col_idx)),
                 sizeof(int64_t));
        init_val =
            bm_sz > 0 ? allocateCountDistinctBitmap(bm_sz) : allocateCountDistinctSet();
        CHECK_NE(init_val, 0);
        ++init_vec_idx;
      } else if (agg_op_metadata.has_tdigest &&
                 agg_op_metadata.quantile_params[col_idx]) {
        auto const q = *agg_op_metadata.quantile_params[col_idx];
        auto const& descs = query_mem_desc.getApproxQuantileDescriptors();
        auto const& desc = descs.at(approx_quantile_descriptors_idx++);
        init_val = reinterpret_cast<int64_t>(
            row_set_mem_owner_->initTDigest(thread_idx_, desc, q));
        CHECK_NE(init_val, 0);
        ++init_vec_idx;
      } else if (agg_op_metadata.has_mode &&
                 agg_op_metadata.mode_index_set.count(col_idx)) {
        init_val = allocateAggMode();
        CHECK_NE(init_val, 0);
        ++init_vec_idx;
      }
    }
    auto const col_slot_width = query_mem_desc.getPaddedSlotWidthBytes(col_idx);
    if (init_val == 0 && col_slot_width > 0) {
      CHECK_LT(init_vec_idx, init_vals.size());
      init_val = init_vals[init_vec_idx++];
    }
    switch (col_slot_width) {
      case 1:
        *col_ptr = static_cast<int8_t>(init_val);
        break;
      case 2:
        *reinterpret_cast<int16_t*>(col_ptr) = (int16_t)init_val;
        break;
      case 4:
        *reinterpret_cast<int32_t*>(col_ptr) = (int32_t)init_val;
        break;
      case 8:
        *reinterpret_cast<int64_t*>(col_ptr) = init_val;
        break;
      case 0:
        continue;
      default:
        CHECK(false);
    }
  }
}

void QueryMemoryInitializer::allocateModeMem(ExecutorDeviceType const device_type,
                                             const QueryMemoryDescriptor& query_mem_desc,
                                             const Executor* executor,
                                             int device_id) {
  if (size_t const ncolumns = query_mem_desc.getNumModeTargets()) {
    size_t const nmodes = ncolumns * query_mem_desc.getEntryCount();
    VLOG(2) << "device_type(" << device_type << ") nmodes = " << nmodes << " = "
            << ncolumns << " * " << query_mem_desc.getEntryCount();
    agg_mode_hash_tables_cpu_.reserve(nmodes);
    if (device_type == ExecutorDeviceType::GPU) {
#ifdef HAVE_CUDA
      // agg_mode_hash_tables_gpu_ also uses the cudaStream from device_allocator_.
      auto* const allocator = dynamic_cast<CudaAllocator*>(device_allocator_);
      CHECK(allocator) << "CudaAllocator expected for ExecutorDeviceType::GPU.";
      agg_mode_hash_tables_gpu_.init(
          allocator, executor->getCudaStream(device_id), nmodes);
#else
      UNREACHABLE();
#endif
    }
  }
}

void QueryMemoryInitializer::allocateCountDistinctGpuMem(
    const QueryMemoryDescriptor& query_mem_desc) {
  if (query_mem_desc.countDistinctDescriptorsLogicallyEmpty()) {
    return;
  }
  CHECK(device_allocator_);

  size_t total_bytes_per_entry{0};
  const size_t num_count_distinct_descs =
      query_mem_desc.getCountDistinctDescriptorsSize();
  for (size_t i = 0; i < num_count_distinct_descs; i++) {
    const auto count_distinct_desc = query_mem_desc.getCountDistinctDescriptor(i);
    if (count_distinct_desc.impl_type_ == CountDistinctImplType::Invalid) {
      continue;
    }
    CHECK(count_distinct_desc.impl_type_ == CountDistinctImplType::Bitmap);
    total_bytes_per_entry += count_distinct_desc.bitmapPaddedSizeBytes();
  }

  count_distinct_bitmap_mem_size_ =
      total_bytes_per_entry * query_mem_desc.getEntryCount();
  auto cuda_allocator = dynamic_cast<CudaAllocator*>(device_allocator_);
  CHECK(cuda_allocator);
  VLOG(1) << "Allocate count distinct buffer on GPU " << cuda_allocator->getDeviceId()
          << " (size: " << count_distinct_bitmap_mem_size_ << " bytes)";
  count_distinct_bitmap_device_mem_ptr_ = reinterpret_cast<CUdeviceptr>(
      cuda_allocator->alloc(count_distinct_bitmap_mem_size_));
  cuda_allocator->zeroDeviceMem(
      reinterpret_cast<int8_t*>(count_distinct_bitmap_device_mem_ptr_),
      count_distinct_bitmap_mem_size_);
  VLOG(1) << "Allocate count distinct buffer on CPU (size: "
          << count_distinct_bitmap_mem_size_ << " bytes)";
  count_distinct_bitmap_host_crt_ptr_ = count_distinct_bitmap_host_mem_ptr_ =
      row_set_mem_owner_->allocate(count_distinct_bitmap_mem_size_, thread_idx_);
}

std::vector<int64_t> QueryMemoryInitializer::calculateCountDistinctBufferSize(
    const QueryMemoryDescriptor& query_mem_desc,
    const RelAlgExecutionUnit& ra_exe_unit) const {
  const size_t agg_col_count{query_mem_desc.getSlotCount()};
  std::vector<int64_t> agg_bitmap_size(agg_col_count);
  for (size_t target_idx = 0; target_idx < ra_exe_unit.target_exprs.size();
       ++target_idx) {
    const auto target_expr = ra_exe_unit.target_exprs[target_idx];
    const auto agg_info = get_target_info(target_expr, g_bigint_count);
    if (is_distinct_target(agg_info)) {
      const size_t agg_col_idx = query_mem_desc.getSlotIndexForSingleSlotCol(target_idx);
      const auto& count_distinct_desc =
          query_mem_desc.getCountDistinctDescriptor(target_idx);
      if (count_distinct_desc.impl_type_ == CountDistinctImplType::Bitmap) {
        const auto bitmap_byte_sz = count_distinct_desc.bitmapPaddedSizeBytes();
        agg_bitmap_size[agg_col_idx] = bitmap_byte_sz;
      } else {
        CHECK(count_distinct_desc.impl_type_ == CountDistinctImplType::UnorderedSet);
        agg_bitmap_size[agg_col_idx] = -1;
      }
    }
  }
  return agg_bitmap_size;
}

void QueryMemoryInitializer::fastAllocateCountDistinctBuffers(
    const QueryMemoryDescriptor& query_mem_desc,
    const RelAlgExecutionUnit& ra_exe_unit) {
  for (size_t target_idx = 0; target_idx < ra_exe_unit.target_exprs.size();
       ++target_idx) {
    const auto target_expr = ra_exe_unit.target_exprs[target_idx];
    const auto agg_info = get_target_info(target_expr, g_bigint_count);
    if (is_distinct_target(agg_info)) {
      const size_t agg_col_idx = query_mem_desc.getSlotIndexForSingleSlotCol(target_idx);
      const auto& count_distinct_desc =
          query_mem_desc.getCountDistinctDescriptor(target_idx);
      if (count_distinct_desc.impl_type_ == CountDistinctImplType::Bitmap) {
        const auto bitmap_byte_sz = count_distinct_desc.bitmapPaddedSizeBytes();
        init_agg_vals_[agg_col_idx] = allocateCountDistinctBitmap(bitmap_byte_sz);
      } else {
        CHECK(count_distinct_desc.impl_type_ == CountDistinctImplType::UnorderedSet);
        init_agg_vals_[agg_col_idx] = allocateCountDistinctSet();
      }
    }
  }
}

// Increments count_distinct_bitmap_host_crt_ptr_ to point to the next cell in
// count_distinct_bitmap_host_mem_ptr_.
int64_t QueryMemoryInitializer::allocateCountDistinctBitmap(const size_t bitmap_byte_sz) {
  if (count_distinct_bitmap_host_mem_ptr_) {
    CHECK(count_distinct_bitmap_host_crt_ptr_);
    auto ptr = count_distinct_bitmap_host_crt_ptr_;
    count_distinct_bitmap_host_crt_ptr_ += bitmap_byte_sz;
    row_set_mem_owner_->addCountDistinctBuffer(
        ptr, bitmap_byte_sz, /*physial_buffer=*/false);
    return reinterpret_cast<int64_t>(ptr);
  }
  return reinterpret_cast<int64_t>(
      row_set_mem_owner_->fastAllocateCountDistinctBuffer(bitmap_byte_sz, thread_idx_));
}

int64_t QueryMemoryInitializer::allocateCountDistinctSet() {
  auto count_distinct_set = new CountDistinctSet();
  row_set_mem_owner_->addCountDistinctSet(count_distinct_set);
  return reinterpret_cast<int64_t>(count_distinct_set);
}

// Return CPU: AggMode* or GPU: (1<<63 | i<<32 | j+1)
// When run on GPU the bit fields are:
// 1<<63 : used to distinguish the packed 32-bit indices vs pointer,
//         since a pointer uses only the lower 48 bits.
// i : used in agg_mode_func_gpu() as index into gpu hash tables array.
// j : used in ResultSet::makeTargetValue() as index into
//     std::deque<AggMode> RowSetMemoryOwner::agg_modes_.
int64_t QueryMemoryInitializer::allocateAggMode() {
  constexpr size_t high_bit = size_t(1) << 31;
  bool const is_gpu = static_cast<bool>(getNumAggModeHashTablesGpu());
  if (is_gpu && high_bit <= agg_mode_hash_tables_cpu_.size()) {
    throw QueryMustRunOnCpu();
  }
  auto const idx_ptr = row_set_mem_owner_->allocateMode();  // (index, AggMode*)
  size_t const cpu_idx = agg_mode_hash_tables_cpu_.size();
  agg_mode_hash_tables_cpu_.push_back(idx_ptr.second);
  return is_gpu ? static_cast<int64_t>((high_bit | cpu_idx) << 32) |
                      static_cast<int64_t>(idx_ptr.first)
                : reinterpret_cast<int64_t>(idx_ptr.second);
}

namespace {

void eachAggregateTargetIdxOfType(
    std::vector<Analyzer::Expr*> const& target_exprs,
    SQLAgg const agg_type,
    std::function<void(Analyzer::AggExpr const*, size_t)> lambda) {
  for (size_t target_idx = 0; target_idx < target_exprs.size(); ++target_idx) {
    auto const target_expr = target_exprs[target_idx];
    if (auto const* agg_expr = dynamic_cast<Analyzer::AggExpr const*>(target_expr)) {
      if (agg_expr->get_aggtype() == agg_type) {
        lambda(agg_expr, target_idx);
      }
    }
  }
}

}  // namespace

QueryMemoryInitializer::ModeIndexSet QueryMemoryInitializer::initializeModeIndexSet(
    const QueryMemoryDescriptor& query_mem_desc,
    const RelAlgExecutionUnit& ra_exe_unit) {
  size_t const slot_count = query_mem_desc.getSlotCount();
  CHECK_LE(ra_exe_unit.target_exprs.size(), slot_count);
  ModeIndexSet mode_index_set;
  eachAggregateTargetIdxOfType(
      ra_exe_unit.target_exprs,
      kMODE,
      [&](Analyzer::AggExpr const*, size_t const target_idx) {
        size_t const agg_col_idx =
            query_mem_desc.getSlotIndexForSingleSlotCol(target_idx);
        CHECK_LT(agg_col_idx, slot_count);
        mode_index_set.emplace(agg_col_idx);
      });
  return mode_index_set;
}

void QueryMemoryInitializer::allocateModeBuffer(
    const QueryMemoryDescriptor& query_mem_desc,
    const RelAlgExecutionUnit& ra_exe_unit) {
  size_t const slot_count = query_mem_desc.getSlotCount();
  CHECK_LE(ra_exe_unit.target_exprs.size(), slot_count);
  ra_exe_unit.eachAggTarget<kMODE>([&](Analyzer::AggExpr const*,
                                       size_t const target_idx) {
    size_t const agg_col_idx = query_mem_desc.getSlotIndexForSingleSlotCol(target_idx);
    CHECK_LT(agg_col_idx, slot_count);
    init_agg_vals_[agg_col_idx] = allocateAggMode();
  });
}

std::vector<QueryMemoryInitializer::QuantileParam>
QueryMemoryInitializer::initializeQuantileParams(
    const QueryMemoryDescriptor& query_mem_desc,
    const RelAlgExecutionUnit& ra_exe_unit) {
  size_t const slot_count = query_mem_desc.getSlotCount();
  CHECK_LE(ra_exe_unit.target_exprs.size(), slot_count);
  std::vector<QuantileParam> quantile_params(slot_count);
  ra_exe_unit.eachAggTarget<kAPPROX_QUANTILE>([&](Analyzer::AggExpr const* const agg_expr,
                                                  size_t const target_idx) {
    size_t const agg_col_idx = query_mem_desc.getSlotIndexForSingleSlotCol(target_idx);
    CHECK_LT(agg_col_idx, slot_count);
    CHECK_EQ(static_cast<int8_t>(sizeof(int64_t)),
             query_mem_desc.getLogicalSlotWidthBytes(agg_col_idx));
    auto const q_expr =
        dynamic_cast<Analyzer::Constant const*>(agg_expr->get_arg1().get());
    CHECK(q_expr);
    quantile_params[agg_col_idx] = q_expr->get_constval().doubleval;
  });
  return quantile_params;
}

void QueryMemoryInitializer::allocateTDigestsBuffer(
    const QueryMemoryDescriptor& query_mem_desc,
    const RelAlgExecutionUnit& ra_exe_unit) {
  size_t const slot_count = query_mem_desc.getSlotCount();
  CHECK_LE(ra_exe_unit.target_exprs.size(), slot_count);

  auto const& descs = query_mem_desc.getApproxQuantileDescriptors();
  size_t approx_quantile_descriptors_idx = 0u;
  ra_exe_unit.eachAggTarget<kAPPROX_QUANTILE>([&](Analyzer::AggExpr const* const agg_expr,
                                                  size_t const target_idx) {
    size_t const agg_col_idx = query_mem_desc.getSlotIndexForSingleSlotCol(target_idx);
    CHECK_LT(agg_col_idx, slot_count);
    CHECK_EQ(static_cast<int8_t>(sizeof(int64_t)),
             query_mem_desc.getLogicalSlotWidthBytes(agg_col_idx));
    auto const q_expr =
        dynamic_cast<Analyzer::Constant const*>(agg_expr->get_arg1().get());
    CHECK(q_expr);
    auto const q = q_expr->get_constval().doubleval;
    auto const& desc = descs.at(approx_quantile_descriptors_idx++);
    init_agg_vals_[agg_col_idx] =
        reinterpret_cast<int64_t>(row_set_mem_owner_->initTDigest(thread_idx_, desc, q));
  });
}

GpuGroupByBuffers QueryMemoryInitializer::prepareTopNHeapsDevBuffer(
    const QueryMemoryDescriptor& query_mem_desc,
    const int8_t* init_agg_vals_dev_ptr,
    const size_t n,
    const int device_id,
    const unsigned block_size_x,
    const unsigned grid_size_x,
    CUstream cuda_stream) {
#ifdef HAVE_CUDA
  CHECK(device_allocator_);
  const auto thread_count = block_size_x * grid_size_x;
  const auto total_buff_size =
      streaming_top_n::get_heap_size(query_mem_desc.getRowSize(), n, thread_count);
  int8_t* dev_buffer = device_allocator_->alloc(total_buff_size);

  std::vector<int8_t*> dev_buffers(thread_count);

  for (size_t i = 0; i < thread_count; ++i) {
    dev_buffers[i] = dev_buffer;
  }

  auto dev_ptr = device_allocator_->alloc(thread_count * sizeof(int8_t*));
  device_allocator_->copyToDevice(dev_ptr,
                                  dev_buffers.data(),
                                  thread_count * sizeof(int8_t*),
                                  "Streaming Top-N buffer ptrs");

  CHECK(query_mem_desc.lazyInitGroups(ExecutorDeviceType::GPU));

  device_allocator_->zeroDeviceMem(reinterpret_cast<int8_t*>(dev_buffer),
                                   thread_count * sizeof(int64_t));

  device_allocator_->setDeviceMem(
      reinterpret_cast<int8_t*>(dev_buffer + thread_count * sizeof(int64_t)),
      (unsigned char)-1,
      thread_count * n * sizeof(int64_t));

  init_group_by_buffer_on_device(
      reinterpret_cast<int64_t*>(
          dev_buffer + streaming_top_n::get_rows_offset_of_heaps(n, thread_count)),
      reinterpret_cast<const int64_t*>(init_agg_vals_dev_ptr),
      n * thread_count,
      query_mem_desc.getGroupbyColCount(),
      query_mem_desc.getEffectiveKeyWidth(),
      query_mem_desc.getRowSize() / sizeof(int64_t),
      query_mem_desc.hasKeylessHash(),
      1,
      block_size_x,
      grid_size_x,
      cuda_stream);

  return {dev_ptr, dev_buffer};
#else
  UNREACHABLE();
  return {};
#endif
}

GpuGroupByBuffers QueryMemoryInitializer::createAndInitializeGroupByBufferGpu(
    const RelAlgExecutionUnit& ra_exe_unit,
    const QueryMemoryDescriptor& query_mem_desc,
    const int8_t* init_agg_vals_dev_ptr,
    const int device_id,
    CUstream cuda_stream,
    const ExecutorDispatchMode dispatch_mode,
    const unsigned block_size_x,
    const unsigned grid_size_x,
    const int8_t warp_size,
    const bool can_sort_on_gpu,
    const bool output_columnar,
    RenderAllocator* render_allocator) {
#ifdef HAVE_CUDA
  if (query_mem_desc.useStreamingTopN()) {
    if (render_allocator) {
      throw StreamingTopNNotSupportedInRenderQuery();
    }
    const auto n = ra_exe_unit.sort_info.offset + ra_exe_unit.sort_info.limit.value_or(0);
    CHECK(!output_columnar);

    return prepareTopNHeapsDevBuffer(query_mem_desc,
                                     init_agg_vals_dev_ptr,
                                     n,
                                     device_id,
                                     block_size_x,
                                     grid_size_x,
                                     cuda_stream);
  }

  auto dev_group_by_buffers =
      create_dev_group_by_buffers(device_allocator_,
                                  group_by_buffers_,
                                  query_mem_desc,
                                  block_size_x,
                                  grid_size_x,
                                  device_id,
                                  dispatch_mode,
                                  num_rows_,
                                  can_sort_on_gpu,
                                  false,
                                  ra_exe_unit.use_bump_allocator,
                                  query_mem_desc.hasVarlenOutput(),
                                  render_allocator);
  if (query_mem_desc.hasVarlenOutput()) {
    CHECK(dev_group_by_buffers.varlen_output_buffer);
    varlen_output_buffer_ =
        reinterpret_cast<CUdeviceptr>(dev_group_by_buffers.varlen_output_buffer);
    CHECK(query_mem_desc.varlenOutputBufferElemSize());
    const size_t varlen_output_buf_bytes =
        query_mem_desc.getEntryCount() *
        query_mem_desc.varlenOutputBufferElemSize().value();
    varlen_output_buffer_host_ptr_ =
        row_set_mem_owner_->allocate(varlen_output_buf_bytes, thread_idx_);
    CHECK(varlen_output_info_);
    varlen_output_info_->gpu_start_address = static_cast<int64_t>(varlen_output_buffer_);
    varlen_output_info_->cpu_buffer_ptr = varlen_output_buffer_host_ptr_;
  }
  if (render_allocator) {
    CHECK_EQ(size_t(0), render_allocator->getAllocatedSize() % 8);
  }
  if (query_mem_desc.lazyInitGroups(ExecutorDeviceType::GPU)) {
    CHECK(!render_allocator);

    const size_t step{query_mem_desc.threadsShareMemory() ? block_size_x : 1};
    size_t groups_buffer_size{query_mem_desc.getBufferSizeBytes(
        ExecutorDeviceType::GPU, dev_group_by_buffers.entry_count)};
    auto group_by_dev_buffer = dev_group_by_buffers.data;
    const size_t col_count = query_mem_desc.getSlotCount();
    int8_t* col_widths_dev_ptr{nullptr};
    if (output_columnar) {
      std::vector<int8_t> compact_col_widths(col_count);
      for (size_t idx = 0; idx < col_count; ++idx) {
        compact_col_widths[idx] = query_mem_desc.getPaddedSlotWidthBytes(idx);
      }
      col_widths_dev_ptr = device_allocator_->alloc(col_count * sizeof(int8_t));
      device_allocator_->copyToDevice(col_widths_dev_ptr,
                                      compact_col_widths.data(),
                                      col_count * sizeof(int8_t),
                                      "Compact column widths");
    }
    const int8_t warp_count =
        query_mem_desc.interleavedBins(ExecutorDeviceType::GPU) ? warp_size : 1;
    const auto num_group_by_buffers =
        getGroupByBuffersSize() - (query_mem_desc.hasVarlenOutput() ? 1 : 0);
    for (size_t i = 0; i < num_group_by_buffers; i += step) {
      if (output_columnar) {
        init_columnar_group_by_buffer_on_device(
            reinterpret_cast<int64_t*>(group_by_dev_buffer),
            reinterpret_cast<const int64_t*>(init_agg_vals_dev_ptr),
            dev_group_by_buffers.entry_count,
            query_mem_desc.getGroupbyColCount(),
            col_count,
            col_widths_dev_ptr,
            /*need_padding = */ true,
            query_mem_desc.hasKeylessHash(),
            sizeof(int64_t),
            block_size_x,
            grid_size_x,
            cuda_stream);
      } else {
        init_group_by_buffer_on_device(
            reinterpret_cast<int64_t*>(group_by_dev_buffer),
            reinterpret_cast<const int64_t*>(init_agg_vals_dev_ptr),
            dev_group_by_buffers.entry_count,
            query_mem_desc.getGroupbyColCount(),
            query_mem_desc.getEffectiveKeyWidth(),
            query_mem_desc.getRowSize() / sizeof(int64_t),
            query_mem_desc.hasKeylessHash(),
            warp_count,
            block_size_x,
            grid_size_x,
            cuda_stream);
      }
      group_by_dev_buffer += groups_buffer_size;
    }
  }
  return dev_group_by_buffers;
#else
  UNREACHABLE();
  return {};
#endif
}

GpuGroupByBuffers QueryMemoryInitializer::setupTableFunctionGpuBuffers(
    const QueryMemoryDescriptor& query_mem_desc,
    const int device_id,
    const unsigned block_size_x,
    const unsigned grid_size_x,
    const bool zero_initialize_buffers) {
  const size_t num_columns = query_mem_desc.getBufferColSlotCount();
  CHECK_GT(num_columns, size_t(0));
  size_t total_group_by_buffer_size{0};
  const auto col_slot_context = query_mem_desc.getColSlotContext();

  std::vector<size_t> col_byte_offsets;
  col_byte_offsets.reserve(num_columns);

  for (size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
    const size_t col_width = col_slot_context.getSlotInfo(col_idx).logical_size;
    size_t group_buffer_size = num_rows_ * col_width;
    col_byte_offsets.emplace_back(total_group_by_buffer_size);
    total_group_by_buffer_size =
        align_to_int64(total_group_by_buffer_size + group_buffer_size);
  }

  int8_t* dev_buffers_allocation{nullptr};
  dev_buffers_allocation = device_allocator_->alloc(total_group_by_buffer_size);
  CHECK(dev_buffers_allocation);
  if (zero_initialize_buffers) {
    device_allocator_->zeroDeviceMem(dev_buffers_allocation, total_group_by_buffer_size);
  }

  auto dev_buffers_mem = dev_buffers_allocation;
  std::vector<int8_t*> dev_buffers(num_columns);
  for (size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
    dev_buffers[col_idx] = dev_buffers_allocation + col_byte_offsets[col_idx];
  }
  auto dev_ptrs = device_allocator_->alloc(num_columns * sizeof(CUdeviceptr));
  device_allocator_->copyToDevice(dev_ptrs,
                                  dev_buffers.data(),
                                  num_columns * sizeof(CUdeviceptr),
                                  "Table function input column ptrs");

  return {dev_ptrs, dev_buffers_mem, (size_t)num_rows_};
}

void QueryMemoryInitializer::copyFromTableFunctionGpuBuffers(
    DeviceAllocator* device_allocator,
    const QueryMemoryDescriptor& query_mem_desc,
    const size_t entry_count,
    const GpuGroupByBuffers& gpu_group_by_buffers,
    const int device_id,
    const unsigned block_size_x,
    const unsigned grid_size_x) {
  const size_t num_columns = query_mem_desc.getBufferColSlotCount();

  int8_t* dev_buffer = gpu_group_by_buffers.data;
  int8_t* host_buffer = reinterpret_cast<int8_t*>(group_by_buffers_[0]);

  const size_t original_entry_count = gpu_group_by_buffers.entry_count;
  CHECK_LE(entry_count, original_entry_count);
  size_t output_device_col_offset{0};
  size_t output_host_col_offset{0};

  const auto col_slot_context = query_mem_desc.getColSlotContext();

  for (size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
    const size_t col_width = col_slot_context.getSlotInfo(col_idx).logical_size;
    const size_t output_device_col_size = original_entry_count * col_width;
    const size_t output_host_col_size = entry_count * col_width;
    device_allocator->copyFromDevice(host_buffer + output_host_col_offset,
                                     dev_buffer + output_device_col_offset,
                                     output_host_col_size,
                                     "Table function output column buffer");
    output_device_col_offset =
        align_to_int64(output_device_col_offset + output_device_col_size);
    output_host_col_offset =
        align_to_int64(output_host_col_offset + output_host_col_size);
  }
}

size_t QueryMemoryInitializer::computeNumberOfBuffers(
    const QueryMemoryDescriptor& query_mem_desc,
    const ExecutorDeviceType device_type,
    const Executor* executor) const {
  return device_type == ExecutorDeviceType::CPU
             ? 1
             : executor->blockSize() *
                   (query_mem_desc.blocksShareMemory() ? 1 : executor->gridSize());
}

namespace {

// in-place compaction of output buffer
void compact_projection_buffer_for_cpu_columnar(
    const QueryMemoryDescriptor& query_mem_desc,
    int8_t* projection_buffer,
    const size_t projection_count) {
  // the first column (row indices) remains unchanged.
  CHECK(projection_count <= query_mem_desc.getEntryCount());
  constexpr size_t row_index_width = sizeof(int64_t);
  size_t buffer_offset1{projection_count * row_index_width};
  // other columns are actual non-lazy columns for the projection:
  for (size_t i = 0; i < query_mem_desc.getSlotCount(); i++) {
    if (query_mem_desc.getPaddedSlotWidthBytes(i) > 0) {
      auto column_proj_size =
          projection_count * query_mem_desc.getPaddedSlotWidthBytes(i);
      auto buffer_offset2 = query_mem_desc.getColOffInBytes(i);
      if (buffer_offset1 + column_proj_size >= buffer_offset2) {
        // overlapping
        std::memmove(projection_buffer + buffer_offset1,
                     projection_buffer + buffer_offset2,
                     column_proj_size);
      } else {
        std::memcpy(projection_buffer + buffer_offset1,
                    projection_buffer + buffer_offset2,
                    column_proj_size);
      }
      buffer_offset1 += align_to_int64(column_proj_size);
    }
  }
}

}  // namespace

void QueryMemoryInitializer::compactProjectionBuffersCpu(
    const QueryMemoryDescriptor& query_mem_desc,
    const size_t projection_count) {
  const auto num_allocated_rows =
      std::min(projection_count, query_mem_desc.getEntryCount());
  const size_t buffer_start_idx = query_mem_desc.hasVarlenOutput() ? 1 : 0;

  // copy the results from the main buffer into projection_buffer
  compact_projection_buffer_for_cpu_columnar(
      query_mem_desc,
      reinterpret_cast<int8_t*>(group_by_buffers_[buffer_start_idx]),
      num_allocated_rows);

  // update the entry count for the result set, and its underlying storage
  CHECK(!result_sets_.empty());
  result_sets_.front()->updateStorageEntryCount(num_allocated_rows);
}

void QueryMemoryInitializer::compactProjectionBuffersGpu(
    const QueryMemoryDescriptor& query_mem_desc,
    DeviceAllocator* device_allocator,
    const GpuGroupByBuffers& gpu_group_by_buffers,
    const size_t projection_count,
    const int device_id) {
  // store total number of allocated rows:
  const auto num_allocated_rows =
      std::min(projection_count, query_mem_desc.getEntryCount());

  // copy the results from the main buffer into projection_buffer
  const size_t buffer_start_idx = query_mem_desc.hasVarlenOutput() ? 1 : 0;
  copy_projection_buffer_from_gpu_columnar(
      device_allocator,
      gpu_group_by_buffers,
      query_mem_desc,
      reinterpret_cast<int8_t*>(group_by_buffers_[buffer_start_idx]),
      num_allocated_rows,
      device_id);

  // update the entry count for the result set, and its underlying storage
  CHECK(!result_sets_.empty());
  result_sets_.front()->updateStorageEntryCount(num_allocated_rows);
}

void QueryMemoryInitializer::copyGroupByBuffersFromGpu(
    DeviceAllocator& device_allocator,
    const QueryMemoryDescriptor& query_mem_desc,
    const size_t entry_count,
    const GpuGroupByBuffers& gpu_group_by_buffers,
    const RelAlgExecutionUnit* ra_exe_unit,
    const unsigned block_size_x,
    const unsigned grid_size_x,
    const int device_id,
    const bool prepend_index_buffer) const {
  const auto thread_count = block_size_x * grid_size_x;

  size_t total_buff_size{0};
  if (ra_exe_unit && query_mem_desc.useStreamingTopN()) {
    const size_t n =
        ra_exe_unit->sort_info.offset + ra_exe_unit->sort_info.limit.value_or(0);
    total_buff_size =
        streaming_top_n::get_heap_size(query_mem_desc.getRowSize(), n, thread_count);
  } else {
    total_buff_size =
        query_mem_desc.getBufferSizeBytes(ExecutorDeviceType::GPU, entry_count);
  }
  copy_group_by_buffers_from_gpu(device_allocator,
                                 group_by_buffers_,
                                 total_buff_size,
                                 gpu_group_by_buffers.data,
                                 query_mem_desc,
                                 block_size_x,
                                 grid_size_x,
                                 device_id,
                                 prepend_index_buffer,
                                 query_mem_desc.hasVarlenOutput());
}

void QueryMemoryInitializer::copyFromDeviceForAggMode() {
#ifdef HAVE_CUDA
  CHECK_EQ(agg_mode_hash_tables_cpu_.size(), agg_mode_hash_tables_gpu_.size());
  try {
    for (size_t i = 0; i < agg_mode_hash_tables_gpu_.size(); ++i) {
      *agg_mode_hash_tables_cpu_[i] = agg_mode_hash_tables_gpu_.moveToHost(i);
    }
  } catch (std::runtime_error const& ex) {
    LOG(INFO) << ex.what();  // E.g. too many keys for fixed gpu hash table
    throw QueryMustRunOnCpu();
  }
#else
  UNREACHABLE();
#endif
}

void QueryMemoryInitializer::applyStreamingTopNOffsetCpu(
    const QueryMemoryDescriptor& query_mem_desc,
    const RelAlgExecutionUnit& ra_exe_unit) {
  const size_t buffer_start_idx = query_mem_desc.hasVarlenOutput() ? 1 : 0;
  CHECK_EQ(group_by_buffers_.size(), buffer_start_idx + 1);

  const auto rows_copy = streaming_top_n::get_rows_copy_from_heaps(
      group_by_buffers_[buffer_start_idx],
      query_mem_desc.getBufferSizeBytes(ra_exe_unit, 1, ExecutorDeviceType::CPU),
      ra_exe_unit.sort_info.offset + ra_exe_unit.sort_info.limit.value_or(0),
      1);
  CHECK_EQ(rows_copy.size(),
           query_mem_desc.getEntryCount() * query_mem_desc.getRowSize());
  memcpy(group_by_buffers_[buffer_start_idx], &rows_copy[0], rows_copy.size());
}

void QueryMemoryInitializer::applyStreamingTopNOffsetGpu(
    Data_Namespace::DataMgr* data_mgr,
    CudaAllocator* cuda_allocator,
    const QueryMemoryDescriptor& query_mem_desc,
    const GpuGroupByBuffers& gpu_group_by_buffers,
    const RelAlgExecutionUnit& ra_exe_unit,
    const unsigned total_thread_count,
    const int device_id,
    CUstream cuda_stream) {
#ifdef HAVE_CUDA
  CHECK(cuda_allocator);
  CHECK_EQ(group_by_buffers_.size(), num_buffers_);
  const size_t buffer_start_idx = query_mem_desc.hasVarlenOutput() ? 1 : 0;

  const auto rows_copy = pick_top_n_rows_from_dev_heaps(
      data_mgr,
      cuda_allocator,
      reinterpret_cast<int64_t*>(gpu_group_by_buffers.data),
      ra_exe_unit,
      query_mem_desc,
      total_thread_count,
      device_id,
      cuda_stream);
  CHECK_EQ(
      rows_copy.size(),
      static_cast<size_t>(query_mem_desc.getEntryCount() * query_mem_desc.getRowSize()));
  memcpy(group_by_buffers_[buffer_start_idx], &rows_copy[0], rows_copy.size());
#else
  UNREACHABLE();
#endif
}

std::vector<int8_t> QueryMemoryInitializer::getAggModeHashTablesGpu() const {
#ifdef HAVE_CUDA
  return agg_mode_hash_tables_gpu_.serialize();
#else
  UNREACHABLE();
  return {};
#endif
}

std::shared_ptr<VarlenOutputInfo> QueryMemoryInitializer::getVarlenOutputInfo() {
  if (varlen_output_info_) {
    return varlen_output_info_;
  }

  // shared_ptr so that both the ResultSet and QMI can hold on to the varlen info object
  // and update it as needed
  varlen_output_info_ = std::make_shared<VarlenOutputInfo>(VarlenOutputInfo{
      static_cast<int64_t>(varlen_output_buffer_), varlen_output_buffer_host_ptr_});
  return varlen_output_info_;
}
