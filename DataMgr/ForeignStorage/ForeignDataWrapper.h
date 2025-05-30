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

#include "DataMgr/ChunkMetadata.h"
#include "ForeignStorageBuffer.h"
#include "Shared/types.h"

#include <map>

struct ColumnDescriptor;

namespace foreign_storage {
struct ForeignServer;
struct ForeignTable;
struct UserMapping;
using ChunkToBufferMap = std::map<ChunkKey, AbstractBuffer*>;
using ChunkToBatchBufferMap = std::map<ChunkKey, std::vector<AbstractBuffer*>>;

/**
 * A helper function to decay a map of batched buffers to an equivalent map of
 * singleton buffers.
 */
inline ChunkToBufferMap decay_batch_chunk_map(const ChunkToBatchBufferMap& src,
                                              const size_t batch_index) {
  ChunkToBufferMap dst;
  for (const auto& [k, v] : src) {
    dst[k] = v[batch_index];
  }
  return dst;
}

/**
 * A helper function to compose a map of batched buffers from an equivalent map of
 * singleton buffers.
 */
inline ChunkToBatchBufferMap compose_batch_chunk_map(const ChunkToBufferMap& src) {
  ChunkToBatchBufferMap dst;
  for (const auto& [k, v] : src) {
    dst[k] = {v};
  }
  return dst;
}

class ForeignDataWrapper {
 public:
  ForeignDataWrapper() = default;
  virtual ~ForeignDataWrapper() = default;

  /**
   * Populates given chunk metadata vector with metadata for all chunks in related
   * foreign table.
   *
   * @param chunk_metadata_vector - vector that will be populated with chunk metadata
   */
  virtual void populateChunkMetadata(ChunkMetadataVector& chunk_metadata_vector) = 0;

  /**
   * Populates given chunk buffers identified by chunk keys. All provided chunk
   * buffers are expected to be for the same fragment.
   *
   * @param required_buffers - chunk buffers that must always be populated
   * @param optional_buffers - chunk buffers that can be optionally populated,
   * if the data wrapper has to scan through chunk data anyways (typically for
   * row wise data formats)
   * @param delete_buffer - chunk buffer for fragment's delete column, if
   * non-null data wrapper is expected to mark deleted rows in buffer and
   * continue processing
   */
  virtual void populateChunkBuffers(const ChunkToBufferMap& required_buffers,
                                    const ChunkToBufferMap& optional_buffers,
                                    AbstractBuffer* delete_buffer = nullptr) = 0;

  /**
   * Mirrors `populateChunkBuffers`, but uses batch buffers instead as an
   * optimization.
   *
   * Populates given chunk batch buffers identified by chunk keys. All provided chunk
   * buffers are expected to be for the same fragment.
   *
   * @param required_buffers - chunk batch buffers that must always be populated
   * @param optional_buffers - chunk batch buffers that can be optionally populated,
   * if the data wrapper has to scan through chunk data anyways (typically for
   * row wise data formats)
   * @param delete_buffer - chunk batch buffer for fragment's delete column, if
   * non-null data wrapper is expected to mark deleted rows in buffer and
   * continue processing
   */
  virtual void populateChunkBatchBuffers(
      const ChunkToBatchBufferMap& required_buffers,
      const ChunkToBatchBufferMap& optional_buffers,
      const std::vector<AbstractBuffer*>& delete_buffer = {}) {
    CHECK_EQ(getOptimalBatchSize(), 1UL)
        << "default implementation of batch buffer population expects exactly one batch";
    populateChunkBuffers(decay_batch_chunk_map(required_buffers, 0),
                         decay_batch_chunk_map(optional_buffers, 0),
                         delete_buffer.empty() ? nullptr : delete_buffer[0]);
  }

  /**
   * Get the size of batches to be used in `BatchBuffer` code paths. This
   * quantity must be a constant and can not change over the course of the
   * lifetime of the data wrapper. Typically, this quantity is chosen to ensure
   * concurrency can be best executed.
   */
  virtual size_t getOptimalBatchSize() { return 1; }

  /**
   * Serialize internal state of wrapper into file at given path if implemented
   */
  virtual std::string getSerializedDataWrapper() const = 0;

  /**
   * Restore internal state of datawrapper
   * @param file_path - location of file created by serializeMetadata
   * @param chunk_metadata_vector - vector of chunk metadata recovered from disk
   */
  virtual void restoreDataWrapperInternals(const std::string& file_path,
                                           const ChunkMetadataVector& chunk_metadata) = 0;

  // For testing, is this data wrapper restored from disk
  virtual bool isRestored() const = 0;

  /**
   * Checks that the options for the given foreign server object are valid.
   * @param foreign_server - foreign server object containing options to be validated
   */
  virtual void validateServerOptions(const ForeignServer* foreign_server) const = 0;

  /**
   * Checks that the options for the given foreign table object are valid.
   * @param foreign_table - foreign table object containing options to be validated
   */
  virtual void validateTableOptions(const ForeignTable* foreign_table) const = 0;

  /**
   * Gets the set of supported table options for the data wrapper.
   */
  virtual const std::set<std::string_view>& getSupportedTableOptions() const = 0;

  /**
   * Gets the subset of table options that can be altered for the data wrapper.
   */
  virtual const std::set<std::string> getAlterableTableOptions() const { return {}; };

  /**
   * Checks that the options for the given user mapping object are valid.
   * @param user_mapping - user mapping object containing options to be validated
   */
  virtual void validateUserMappingOptions(const UserMapping* user_mapping,
                                          const ForeignServer* foreign_server) const = 0;

  /**
   * Gets the set of supported user mapping options for the data wrapper.
   */
  virtual const std::set<std::string_view>& getSupportedUserMappingOptions() const = 0;

  /**
    Verifies the schema is supported by this foreign table
    * @param columns - column descriptors for this table
   */
  virtual void validateSchema(const std::list<ColumnDescriptor>& columns,
                              const ForeignTable* foreign_table) const {};

  /**
   * ParallelismLevel describes the desired level of parallelism of the data
   * wrapper. This level controls which `optional_buffers` are passed to
   * `populateChunkBuffers` with the following behaviour:
   *
   * NONE - no additional optional buffers are passed in
   *
   * INTRA_FRAGMENT - additional optional buffers which are in the same fragment as the
   * required buffers
   *
   * INTER_FRAGMENT - additional optional buffers which may be in
   * different fragments than those of the required buffers
   *
   * Note, the optional buffers are passed in with the intention of
   * allowing the data wrapper to employ parallelism in retrieving them. Each subsequent
   * level allows for a greater degree of parallelism but does not have to be supported.
   */
  enum ParallelismLevel { NONE, INTRA_FRAGMENT, INTER_FRAGMENT };

  /**
   * Gets the desired level of parallelism for the data wrapper when a cache is
   * in use. This affects the optional buffers that the data wrapper is made
   * aware of during data requests.
   */
  virtual ParallelismLevel getCachedParallelismLevel() const { return NONE; }

  /**
   * Gets the desired level of parallelism for the data wrapper when no cache
   * is in use. This affects the optional buffers that the data wrapper is made
   * aware of during data requests.
   */
  virtual ParallelismLevel getNonCachedParallelismLevel() const { return NONE; }

  /**
   * If `true` data wrapper implements a lazy fragment fetching mode. This mode
   * allows requests for fragments to be issued to `populateChunks` *without*
   * the prerequisite that `populateChunkMetadata` has successfully finished
   * execution. This is an optimization that has some specific use-cases and is
   * not required.
   *
   * NOTE: this mode is not guaranteed to work as expected when combined with
   * certain types of refresh modes such as append. This is subject to change
   * in the future, but has no impact on the intended use-cases of this mode.
   */
  virtual bool isLazyFragmentFetchingEnabled() const { return false; }

  /**
   * If `true` data wrapper accepts delete buffers that may be prepoulated with
   * a speculative number of `false` values. This is an minor optimization that
   * prevents unnecessary allocations and deallocations by reusing the buffer
   * when it is large enouhg, and requiring a smaller allocation when it needs
   * to be resized.
   *
   * This option is only used when delete buffers are in use.
   *
   * TODO: remove this method when it will become the default behaviour for all
   * data wrappers going forward.
   */
  virtual bool acceptsPrepopulatedDeleteBuffer() const { return false; }
};
}  // namespace foreign_storage
