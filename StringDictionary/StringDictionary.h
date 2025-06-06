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

#include <functional>
#include <future>
#include <map>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "DictRef.h"
#include "DictionaryCache.hpp"
#include "Shared/Datum.h"
#include "Shared/DbObjectKeys.h"

#include "OSDependent/heavyai_fs.h"

extern bool g_enable_stringdict_parallel;

class StringDictionaryClient;

namespace StringOps_Namespace {
class StringOps;
}

inline size_t translateTransientIdToIndex(const int32_t str_id) {
  // Transient ids start at -2 and descend, -1 is INVALID_STR_ID
  return -(str_id)-2;
}

struct SortedStringPermutation {
  SortedStringPermutation(const bool should_sort_descending)
      : sort_descending(should_sort_descending) {}
  bool sort_descending;

  std::vector<int32_t> persisted_permutation;

  // transient_permutation_ maps transient string ids, starting from -2
  // (-1 is INVALID_STR_ID), such that -2 is represented by element 0 of
  // the vector, -3 by element 1, and so on, to the index in the
  // persisted_permutation_ vector above, such that the first element permutation
  // is set to the sort rank of the persisted string that is after
  // the transient string. For example, if 'BBB' and 'DDD' are persisted
  // elements in the dictionary, and 'AAA', 'AAB' 'CCC', and 'EEE' are transient
  // strings, strings 'AAA' and 'AAB' will map to 0, since they fall before 'BBB',
  // which is rank 0 in the persisted dictionary, 'CCC' will map to 1,
  // since it occurs before 'DDD', and 'EEE' will map to 2.
  // The second element denotes ordering for transient elements with the same persisted
  // rank In the case above, both 'AAA' and 'AAB', falling before the first persisted
  // string ('BBB'), will both have the same persisted rank(first element of the pair) of
  // 0, but 'AAA' will have the transient rank of 0, and 'AAB' will have the transient
  // rank of 1
  std::vector<std::pair<int32_t, int32_t>> transient_permutation;

  inline size_t size() const {
    return persisted_permutation.size() + transient_permutation.size();
  }
  inline std::pair<int32_t, int32_t> get_permutation(const int32_t str_id) const {
    if (str_id < 0) {
      return transient_permutation[translateTransientIdToIndex(str_id)];
    }
    return std::make_pair(persisted_permutation[str_id],
                          std::numeric_limits<int32_t>::max());
  }
  bool operator()(const int32_t lhs, const int32_t rhs) const;
};

class DictPayloadUnavailable : public std::runtime_error {
 public:
  DictPayloadUnavailable() : std::runtime_error("DictPayloadUnavailable") {}

  DictPayloadUnavailable(const std::string& err) : std::runtime_error(err) {}
};

class LeafHostInfo;

using string_dict_hash_t = uint32_t;

using StringLookupCallback = std::function<bool(std::string_view, int32_t string_id)>;

namespace string_dictionary {
// The canary buffer is a block of memory (4MB on most systems by default) which is used
// to allocate additional space in a dictionary.  When the dictionary needs additional
// space, the canary buffer is used to write that space to disk, or memory.  The values
// in the canary buffer (all '1's) are reserved values, so we can separate the
// allocated, but unused, space from valid entries (necessary when reading a dictionary
// from disk).
struct CanaryBuffer {
  CanaryBuffer(size_t new_size) : size(new_size) {
    buffer = static_cast<char*>(malloc(size));
    CHECK(buffer);
    memset(buffer, 0xff, size);  // All '1's is reserved as canary values.
  }
  CanaryBuffer() : CanaryBuffer(1024 * heavyai::get_page_size()) {}
  ~CanaryBuffer() { free(buffer); }

  size_t size{0};
  char* buffer{nullptr};
};
}  // namespace string_dictionary

class StringDictionary {
 public:
  StringDictionary(const shared::StringDictKey& dict_key,
                   const std::string& folder,
                   const bool isTemp,
                   const bool recover,
                   const bool materializeHashes = false,
                   size_t initial_capacity = 256);
  StringDictionary(const LeafHostInfo& host, const shared::StringDictKey& dict_key);
  ~StringDictionary() noexcept;

  const shared::StringDictKey& getDictKey() const noexcept;

  class StringCallback {
   public:
    virtual ~StringCallback() = default;
    virtual void operator()(std::string const&, int32_t const string_id) = 0;
    virtual void operator()(std::string_view const, int32_t const string_id) = 0;
  };

  struct StringDictMemoryUsage {
    size_t mmap_size{0};
    size_t temp_dict_size{0};
    size_t canary_buffer_size{0};

    size_t total() const { return temp_dict_size + canary_buffer_size; }
  };

  // Functors passed to eachStringSerially() must derive from StringCallback.
  // Each std::string const& (if isClient()) or std::string_view (if !isClient())
  // plus string_id is passed to the callback functor.
  void eachStringSerially(int64_t const generation, StringCallback&) const;
  std::function<int32_t(std::string const&)> makeLambdaStringToId() const;
  friend class StringLocalCallback;

  int32_t getOrAdd(const std::string& str) noexcept;
  template <class T, class String>
  size_t getBulk(const std::vector<String>& string_vec, T* encoded_vec) const;
  template <class T, class String>
  size_t getBulk(const std::vector<String>& string_vec,
                 T* encoded_vec,
                 const int64_t generation) const;
  template <class T, class String>
  void getOrAddBulk(const std::vector<String>& string_vec, T* encoded_vec);
  template <class T, class String>
  void getOrAddBulkParallel(const std::vector<String>& string_vec, T* encoded_vec);
  template <class String>
  void getOrAddBulkArray(const std::vector<std::vector<String>>& string_array_vec,
                         std::vector<std::vector<int32_t>>& ids_array_vec);
  template <class String>
  int32_t getIdOfString(const String&) const;
  std::string getString(int32_t string_id) const;
  std::string_view getStringView(int32_t string_id) const;
  std::pair<char*, size_t> getStringBytes(int32_t string_id) const noexcept;
  size_t storageEntryCount() const;

  SortedStringPermutation getSortedPermutation(
      const std::vector<std::pair<std::string, int32_t>>& transient_string_to_ids,
      const bool should_sort_descending);

  std::vector<std::pair<int32_t, int32_t>> getTransientSortPermutation(
      const std::vector<std::pair<std::string, int32_t>>& transient_string_to_id_map)
      const;

  std::vector<int32_t> getPersistedSortedPermutation();

  template <typename T>
  std::vector<T> getLike(const std::string& pattern,
                         const bool icase,
                         const bool is_simple,
                         const char escape,
                         const size_t generation) const;

  template <typename T>
  std::vector<T> getLikeImpl(const std::string& pattern,
                             const bool icase,
                             const bool is_simple,
                             const char escape,
                             const size_t generation) const;

  std::vector<int32_t> getCompare(const std::string& pattern,
                                  const std::string& comp_operator,
                                  const size_t generation);

  std::vector<int32_t> getRegexpLike(const std::string& pattern,
                                     const char escape,
                                     const size_t generation) const;

  std::vector<std::string> copyStrings() const;

  std::vector<std::string_view> getStringViews() const;
  std::vector<std::string_view> getStringViews(const size_t generation) const;

  std::vector<int32_t> buildDictionaryTranslationMap(
      const std::shared_ptr<StringDictionary> dest_dict,
      StringLookupCallback const& dest_transient_lookup_callback) const;

  size_t buildDictionaryTranslationMap(
      const StringDictionary* dest_dict,
      int32_t* translated_ids,
      const int64_t source_generation,
      const int64_t dest_generation,
      const bool dest_has_transients,
      StringLookupCallback const& dest_transient_lookup_callback,
      const StringOps_Namespace::StringOps& string_ops) const;

  void buildDictionaryNumericTranslationMap(
      Datum* translated_ids,
      const int64_t source_generation,
      const StringOps_Namespace::StringOps& string_ops) const;

  bool checkpoint() noexcept;

  bool isClient() const noexcept;

  /**
   * @brief Populates provided \p dest_ids vector with string ids corresponding to given
   * source strings
   *
   * Given a vector of source string ids and corresponding source dictionary, this method
   * populates a vector of destination string ids by either returning the string id of
   * matching strings in the destination dictionary or creating new entries in the
   * dictionary. Source string ids can also be transient if they were created by a
   * function (e.g LOWER/UPPER functions). A map of transient string ids to string values
   * is provided in order to handle this use case.
   *
   * @param dest_ids - vector of destination string ids to be populated
   * @param dest_dict - destination dictionary
   * @param source_ids - vector of source string ids for which destination ids are needed
   * @param source_dict - source dictionary
   * @param transient_string_vec - ordered vector of string value pointers
   */
  static void populate_string_ids(
      std::vector<int32_t>& dest_ids,
      StringDictionary* dest_dict,
      const std::vector<int32_t>& source_ids,
      const StringDictionary* source_dict,
      const std::vector<std::string const*>& transient_string_vec = {});

  static void populate_string_array_ids(
      std::vector<std::vector<int32_t>>& dest_array_ids,
      StringDictionary* dest_dict,
      const std::vector<std::vector<int32_t>>& source_array_ids,
      const StringDictionary* source_dict);

  inline static std::atomic<size_t> total_mmap_size{0};
  inline static std::atomic<size_t> total_temp_size{0};

  static size_t getTotalMmapSize() { return total_mmap_size; }
  static size_t getTotalTempSize() { return total_temp_size; }
  static size_t getTotalCanarySize() { return canary_buffer.size; }
  static StringDictMemoryUsage getStringDictMemoryUsage();

  static constexpr int32_t INVALID_STR_ID = -1;
  static constexpr size_t MAX_STRLEN = (1 << 15) - 1;
  static constexpr size_t MAX_STRCOUNT = (1U << 31) - 1;

  void update_leaf(const LeafHostInfo& host_info);
  size_t computeCacheSize() const;

  std::vector<std::string> getStringsForRange(
      int32_t start_id,
      int32_t end_id,
      const StringOps_Namespace::StringOps& string_ops,
      const std::function<bool(int32_t)>& mask_functor) const;

 private:
  struct StringIdxEntry {
    uint64_t off : 48;
    uint64_t size : 16;
  };

  // In the compare_cache_value_t index represents the index of the sorted cache.
  // The diff component represents whether the index the cache is pointing to is equal to
  // the pattern it is cached for. We want to use diff so we don't have compare string
  // again when we are retrieving it from the cache.
  struct compare_cache_value_t {
    int32_t index;
    int32_t diff;
  };

  struct PayloadString {
    char* c_str_ptr;
    size_t size;
    bool canary;
  };

  void processDictionaryFutures(
      std::vector<std::future<std::vector<std::pair<string_dict_hash_t, unsigned int>>>>&
          dictionary_futures);
  size_t getNumStringsFromStorage(const size_t storage_slots) const noexcept;
  bool fillRateIsHigh(const size_t num_strings) const noexcept;
  void increaseHashTableCapacity() noexcept;
  template <class String>
  void increaseHashTableCapacityFromStorageAndMemory(
      const size_t str_count,
      const size_t storage_high_water_mark,
      const std::vector<String>& input_strings,
      const std::vector<size_t>& string_memory_ids,
      const std::vector<string_dict_hash_t>& input_strings_hashes) noexcept;
  int32_t getOrAddImpl(const std::string_view& str) noexcept;
  template <class String>
  void hashStrings(const std::vector<String>& string_vec,
                   std::vector<string_dict_hash_t>& hashes) const noexcept;

  int32_t getUnlocked(const std::string_view sv) const noexcept;
  std::string getStringUnlocked(int32_t string_id) const noexcept;
  std::string_view getStringViewUnlocked(int32_t string_id) const noexcept;
  std::string getStringChecked(const int string_id) const noexcept;
  std::string_view getStringViewChecked(const int string_id) const noexcept;
  std::pair<char*, size_t> getStringBytesChecked(const int string_id) const noexcept;
  template <class String>
  uint32_t computeBucket(
      const string_dict_hash_t hash,
      const String& input_string,
      const std::vector<int32_t>& string_id_string_dict_hash_table) const noexcept;
  template <class String>
  uint32_t computeBucketFromStorageAndMemory(
      const string_dict_hash_t input_string_hash,
      const String& input_string,
      const std::vector<int32_t>& string_id_string_dict_hash_table,
      const size_t storage_high_water_mark,
      const std::vector<String>& input_strings,
      const std::vector<size_t>& string_memory_ids) const noexcept;
  uint32_t computeUniqueBucketWithHash(
      const string_dict_hash_t hash,
      const std::vector<int32_t>& string_id_string_dict_hash_table) noexcept;
  void checkAndConditionallyIncreasePayloadCapacity(const size_t write_length);
  void checkAndConditionallyIncreaseOffsetCapacity(const size_t write_length);

  template <class String>
  void appendToStorage(const String str) noexcept;
  template <class String>
  void appendToStorageBulk(const std::vector<String>& input_strings,
                           const std::vector<size_t>& string_memory_ids,
                           const size_t sum_new_strings_lengths) noexcept;
  PayloadString getStringFromStorage(const int string_id) const noexcept;
  std::string_view getStringFromStorageFast(const int string_id) const noexcept;
  void addPayloadCapacity(const size_t min_capacity_requested = 0) noexcept;
  void addOffsetCapacity(const size_t min_capacity_requested = 0) noexcept;
  size_t addStorageCapacity(int fd, const size_t min_capacity_requested = 0) noexcept;
  void* addMemoryCapacity(void* addr,
                          size_t& mem_size,
                          const size_t min_capacity_requested = 0) noexcept;
  void invalidateInvertedIndex() noexcept;
  std::vector<int32_t> getEquals(std::string pattern,
                                 std::string comp_operator,
                                 size_t generation);
  void buildSortedCache();
  void insertInSortedCache(std::string str, int32_t str_id);
  void sortCache(std::vector<int32_t>& cache);
  void mergeSortedCache(std::vector<int32_t>& temp_sorted_cache);

  size_t storageEntryCountUnlocked() const;

  std::vector<int32_t> fillIndexVector(const size_t start_idx,
                                       const size_t end_idx) const;
  void permuteSortedCache();

  compare_cache_value_t* binary_search_cache(const std::string& pattern) const;

  const shared::StringDictKey dict_key_;
  const std::string folder_;
  size_t str_count_;
  size_t collisions_;
  std::vector<int32_t> string_id_string_dict_hash_table_;
  std::vector<string_dict_hash_t> hash_cache_;
  std::vector<int32_t> sorted_cache_;
  std::vector<int32_t> sorted_permutation_cache_;
  bool isTemp_;
  bool materialize_hashes_;
  std::string offsets_path_;
  int payload_fd_;
  int offset_fd_;
  StringIdxEntry* offset_map_;
  char* payload_map_;
  size_t offset_file_size_;
  size_t payload_file_size_;
  size_t payload_file_off_;
  mutable std::shared_mutex rw_mutex_;
  mutable std::map<std::tuple<std::string, bool, bool, char>, std::vector<int32_t>>
      like_i32_cache_;
  mutable std::map<std::tuple<std::string, bool, bool, char>, std::vector<int64_t>>
      like_i64_cache_;
  mutable size_t like_cache_size_;
  mutable std::map<std::pair<std::string, char>, std::vector<int32_t>> regex_cache_;
  mutable size_t regex_cache_size_;
  mutable std::map<std::string, int32_t> equal_cache_;
  mutable size_t equal_cache_size_;
  mutable DictionaryCache<std::string, compare_cache_value_t> compare_cache_;
  mutable size_t compare_cache_size_;
  mutable std::shared_ptr<std::vector<std::string>> strings_cache_;
  mutable size_t strings_cache_size_;
  mutable std::unique_ptr<StringDictionaryClient> client_;
  mutable std::unique_ptr<StringDictionaryClient> client_no_timeout_;

  static inline string_dictionary::CanaryBuffer canary_buffer;
};

int32_t truncate_to_generation(const int32_t id, const size_t generation);

void translate_string_ids(std::vector<int32_t>& dest_ids,
                          const LeafHostInfo& dict_server_host,
                          const shared::StringDictKey& dest_dict_key,
                          const std::vector<int32_t>& source_ids,
                          const shared::StringDictKey& source_dict_key,
                          const int32_t dest_generation);

std::ostream& operator<<(std::ostream& os,
                         const StringDictionary::StringDictMemoryUsage&);
