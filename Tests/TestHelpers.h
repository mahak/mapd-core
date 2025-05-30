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
#include "LeafHostInfo.h"
#include "Logger/Logger.h"
#include "QueryEngine/ExecutorDeviceType.h"
#include "QueryEngine/TargetValue.h"

#include <gtest/gtest.h>
#include <tbb/version.h>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/variant.hpp>

#include <fstream>
#include <regex>

#define PRINT_TBB_TASK_SCHEDULER_HANDLE_DIAGNOSTICS 0

#ifdef TBB_PREVIEW_WAITING_FOR_WORKERS
#include <tbb/global_control.h>
#if PRINT_TBB_TASK_SCHEDULER_HANDLE_DIAGNOSTICS
#include <iostream>
#endif
#endif

// per-component comparison with epsilon tolerance for floating-point values
// EXPECT_EQ of the whole object does not tolerate x86/ARM differences
// Part of the reason we want this in a macro intead of a function is so that it correctly
// identifies the source line during a test failure.
#define EXPECT_CHUNK_METADATA_EQ(a, b)                                                  \
  {                                                                                     \
    auto const& found = (a);                                                            \
    auto const& expected = (b);                                                         \
    ASSERT_EQ(found.sqlType, expected.sqlType);                                         \
    EXPECT_EQ(found.numBytes, expected.numBytes);                                       \
    EXPECT_EQ(found.numElements, expected.numElements);                                 \
    switch (found.sqlType.get_type()) {                                                 \
      case kBOOLEAN:                                                                    \
        EXPECT_EQ(found.chunkStats.min.tinyintval, expected.chunkStats.min.tinyintval); \
        EXPECT_EQ(found.chunkStats.max.tinyintval, expected.chunkStats.max.tinyintval); \
        break;                                                                          \
      case kTINYINT:                                                                    \
        EXPECT_EQ(found.chunkStats.min.tinyintval, expected.chunkStats.min.tinyintval); \
        EXPECT_EQ(found.chunkStats.max.tinyintval, expected.chunkStats.max.tinyintval); \
        break;                                                                          \
      case kSMALLINT:                                                                   \
        EXPECT_EQ(found.chunkStats.min.smallintval,                                     \
                  expected.chunkStats.min.smallintval);                                 \
        EXPECT_EQ(found.chunkStats.max.smallintval,                                     \
                  expected.chunkStats.max.smallintval);                                 \
        break;                                                                          \
      case kINT:                                                                        \
        EXPECT_EQ(found.chunkStats.min.intval, expected.chunkStats.min.intval);         \
        EXPECT_EQ(found.chunkStats.max.intval, expected.chunkStats.max.intval);         \
        break;                                                                          \
      case kBIGINT:                                                                     \
      case kNUMERIC:                                                                    \
      case kDECIMAL:                                                                    \
        EXPECT_EQ(found.chunkStats.min.bigintval, expected.chunkStats.min.bigintval);   \
        EXPECT_EQ(found.chunkStats.max.bigintval, expected.chunkStats.max.bigintval);   \
        break;                                                                          \
      case kFLOAT:                                                                      \
        EXPECT_FLOAT_EQ(found.chunkStats.min.floatval,                                  \
                        expected.chunkStats.min.floatval);                              \
        EXPECT_FLOAT_EQ(found.chunkStats.max.floatval,                                  \
                        expected.chunkStats.max.floatval);                              \
        break;                                                                          \
      case kDOUBLE:                                                                     \
        EXPECT_DOUBLE_EQ(found.chunkStats.min.doubleval,                                \
                         expected.chunkStats.min.doubleval);                            \
        EXPECT_DOUBLE_EQ(found.chunkStats.max.doubleval,                                \
                         expected.chunkStats.max.doubleval);                            \
        break;                                                                          \
      case kVARCHAR:                                                                    \
      case kCHAR:                                                                       \
      case kTEXT:                                                                       \
        if (found.sqlType.get_compression() == kENCODING_DICT) {                        \
          EXPECT_EQ(found.chunkStats.min.intval, expected.chunkStats.min.intval);       \
          EXPECT_EQ(found.chunkStats.max.intval, expected.chunkStats.max.intval);       \
        }                                                                               \
        break;                                                                          \
      default:                                                                          \
        UNREACHABLE() << "Unknown metadata type";                                       \
    }                                                                                   \
    EXPECT_EQ(found.chunkStats.has_nulls, expected.chunkStats.has_nulls);               \
    EXPECT_EQ(found.rasterTile, expected.rasterTile);                                   \
  }

namespace TestHelpers {

class TbbPrivateServerKiller : public ::testing::Test {
#ifdef TBB_PREVIEW_WAITING_FOR_WORKERS  // set when ENABLE_TSAN
 protected:
#if PRINT_TBB_TASK_SCHEDULER_HANDLE_DIAGNOSTICS
  void SetUp() override {
#if TBB_VERSION_MAJOR == 2021 && TBB_VERSION_MINOR < 6
    auto handle = tbb::task_scheduler_handle::get();
#else
    auto handle = oneapi::tbb::task_scheduler_handle{oneapi::tbb::attach {}};
#endif
    bool const handle_as_bool = static_cast<bool>(handle);
    bool const finalized = tbb::finalize(handle, std::nothrow_t{});
    std::cout << __FILE__ << " +" << __LINE__ << ' ' << __func__
              << " handle_as_bool=" << handle_as_bool << " finalized=" << finalized
              << std::endl;
  }
#endif
  void TearDown() override {
    // Expected to kill tbb::detail::r1::rml::private_server after each test,
    // which can otherwise trigger false positive tsan data race warnings.
#if TBB_VERSION_MAJOR == 2021 && TBB_VERSION_MINOR < 6
    auto handle = tbb::task_scheduler_handle::get();
#else
    auto handle = oneapi::tbb::task_scheduler_handle{oneapi::tbb::attach {}};
#endif
#if PRINT_TBB_TASK_SCHEDULER_HANDLE_DIAGNOSTICS
    bool const handle_as_bool = static_cast<bool>(handle);
    bool const finalized = tbb::finalize(handle, std::nothrow_t{});
    std::cout << __FILE__ << " +" << __LINE__ << ' ' << __func__
              << " handle_as_bool=" << handle_as_bool << " finalized=" << finalized
              << std::endl;
#else
    tbb::finalize(handle, std::nothrow_t{});  // Returns false on error.
#endif
  }
#endif
};

/// Temporary file that is deleted upon destruction.
class TempFile {
  std::string path_;
  std::ofstream stream_;

 public:
  TempFile()
      : path_(
            (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path())
                .string())
      , stream_(path_) {
    if (!stream_) {
      throw std::runtime_error("Failed to create TempFile: " + path_);
    }
  }

  ~TempFile() {
    stream_.close();
    if (!path_.empty()) {
      boost::filesystem::remove(path_);
    }
  }

  void close() { stream_.close(); }

  std::string const& path() const { return path_; }

  template <typename T>
  inline TempFile& operator<<(T const& data) {
    stream_ << data;
    return *this;
  }

  TempFile& operator<<(std::ostream& (*manip)(std::ostream&)) {
    manip(stream_);
    return *this;
  }
};

class ExecutorDeviceParameterizedTest
    : public ::testing::TestWithParam<ExecutorDeviceType> {
 protected:
  void SetUp() override {
    device_type_ = GetParam();
#ifdef HAVE_CUDA
    bool skip_test = (device_type_ == ExecutorDeviceType::GPU && !gpusPresent());
#else
    bool skip_test = (device_type_ == ExecutorDeviceType::GPU);
#endif
    if (skip_test) {
      GTEST_SKIP() << "Unsupported device type: " << device_type_;
    }
  }

  virtual bool gpusPresent() = 0;

  ExecutorDeviceType device_type_;
};

template <class T>
void compare_array(const TargetValue& r,
                   const std::vector<T>& arr,
                   const double tol = -1.) {
  auto array_tv = boost::get<ArrayTargetValue>(&r);
  CHECK(array_tv);
  if (!array_tv->is_initialized()) {
    ASSERT_EQ(size_t(0), arr.size());
    return;
  }
  const auto& scalar_tv_vector = array_tv->get();
  ASSERT_EQ(scalar_tv_vector.size(), arr.size());
  size_t ctr = 0;
  for (const ScalarTargetValue& scalar_tv : scalar_tv_vector) {
    T value;
    if constexpr (std::is_integral_v<T>) {
      auto p = boost::get<int64_t>(&scalar_tv);
      CHECK(p);
      value = *p;
    } else {
      auto p = boost::get<T>(&scalar_tv);
      CHECK(p);
      value = *p;
    }
    if (tol < 0.) {
      ASSERT_EQ(value, arr[ctr++]);
    } else {
      ASSERT_NEAR(value, arr[ctr++], tol);
    }
  }
}

template <>
void compare_array(const TargetValue& r,
                   const std::vector<std::string>& arr,
                   const double tol) {
  auto array_tv = boost::get<ArrayTargetValue>(&r);
  CHECK(array_tv);
  if (!array_tv->is_initialized()) {
    ASSERT_EQ(size_t(0), arr.size());
    return;
  }
  const auto& scalar_tv_vector = array_tv->get();
  ASSERT_EQ(scalar_tv_vector.size(), arr.size());
  size_t ctr = 0;
  for (const ScalarTargetValue& scalar_tv : scalar_tv_vector) {
    auto ns = boost::get<NullableString>(&scalar_tv);
    CHECK(ns);
    auto str = boost::get<std::string>(ns);
    CHECK(str);
    ASSERT_TRUE(*str == arr[ctr++]);
  }
}

template <class T>
void compare_array(const std::vector<T>& a,
                   const std::vector<T>& b,
                   const double tol = -1.) {
  CHECK_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); i++) {
    if (tol < 0.) {
      ASSERT_EQ(a[i], b[i]);
    } else {
      ASSERT_NEAR(a[i], b[i], tol);
    }
  }
}

struct GeoTargetComparator {
  static void compare(const GeoPointTargetValue& a,
                      const GeoPointTargetValue& b,
                      const double tol = -1.) {
    compare_array(*a.coords, *b.coords, tol);
  }
  static void compare(const GeoMultiPointTargetValue& a,
                      const GeoMultiPointTargetValue& b,
                      const double tol = -1.) {
    compare_array(*a.coords, *b.coords, tol);
  }
  static void compare(const GeoLineStringTargetValue& a,
                      const GeoLineStringTargetValue& b,
                      const double tol = -1.) {
    compare_array(*a.coords, *b.coords, tol);
  }
  static void compare(const GeoMultiLineStringTargetValue& a,
                      const GeoMultiLineStringTargetValue& b,
                      const double tol = -1.) {
    compare_array(*a.coords, *b.coords, tol);
    compare_array(*a.linestring_sizes, *b.linestring_sizes);
  }
  static void compare(const GeoPolyTargetValue& a,
                      const GeoPolyTargetValue& b,
                      const double tol = -1.) {
    compare_array(*a.coords, *b.coords, tol);
    compare_array(*a.ring_sizes, *b.ring_sizes);
  }
  static void compare(const GeoMultiPolyTargetValue& a,
                      const GeoMultiPolyTargetValue& b,
                      const double tol = -1.) {
    compare_array(*a.coords, *b.coords, tol);
    compare_array(*a.ring_sizes, *b.ring_sizes);
    compare_array(*a.poly_rings, *b.poly_rings);
  }
};

template <class T>
T g(const TargetValue& r) {
  auto geo_r = boost::get<GeoTargetValue>(&r);
  CHECK(geo_r);
  CHECK(geo_r->is_initialized());
  return boost::get<T>(geo_r->get());
}

template <class T>
void compare_geo_target(const TargetValue& r,
                        const T& geo_truth_target,
                        const double tol = -1.) {
  const auto geo_value = g<T>(r);
  GeoTargetComparator::compare(geo_value, geo_truth_target, tol);
}

template <class T>
T v(const TargetValue& r) {
  auto scalar_r = boost::get<ScalarTargetValue>(&r);
  CHECK(scalar_r);
  auto p = boost::get<T>(scalar_r);
  CHECK(p);
  return *p;
}

template <typename T>
inline std::string convert(const T& t) {
  return std::to_string(t);
}

template <std::size_t N>
inline std::string convert(const char (&t)[N]) {
  return std::string(t);
}

template <>
inline std::string convert(const std::string& t) {
  return t;
}

bool is_null_tv(const TargetValue& tv, const SQLTypeInfo& ti) {
  if (ti.get_notnull()) {
    return false;
  }
  const auto scalar_tv = boost::get<ScalarTargetValue>(&tv);
  if (!scalar_tv) {
    CHECK(ti.is_array());
    const auto array_tv = boost::get<ArrayTargetValue>(&tv);
    CHECK(array_tv);
    return !array_tv->is_initialized();
  }
  if (boost::get<int64_t>(scalar_tv)) {
    int64_t data = *(boost::get<int64_t>(scalar_tv));
    switch (ti.get_type()) {
      case kBOOLEAN:
        return data == NULL_BOOLEAN;
      case kTINYINT:
        return data == NULL_TINYINT;
      case kSMALLINT:
        return data == NULL_SMALLINT;
      case kINT:
        return data == NULL_INT;
      case kDECIMAL:
      case kNUMERIC:
      case kBIGINT:
        return data == NULL_BIGINT;
      case kTIME:
      case kTIMESTAMP:
      case kDATE:
      case kINTERVAL_DAY_TIME:
      case kINTERVAL_YEAR_MONTH:
        return data == NULL_BIGINT;
      default:
        CHECK(false);
    }
  } else if (boost::get<double>(scalar_tv)) {
    double data = *(boost::get<double>(scalar_tv));
    if (ti.get_type() == kFLOAT) {
      return data == NULL_FLOAT;
    } else {
      return data == NULL_DOUBLE;
    }
  } else if (boost::get<float>(scalar_tv)) {
    CHECK_EQ(kFLOAT, ti.get_type());
    float data = *(boost::get<float>(scalar_tv));
    return data == NULL_FLOAT;
  } else if (boost::get<NullableString>(scalar_tv)) {
    auto s_n = boost::get<NullableString>(scalar_tv);
    auto s = boost::get<std::string>(s_n);
    return !s;
  }
  CHECK(false);
  return false;
}

// Example: "{1, 2, 3}" -> "_1_2_3_" for replacement='_'.
void replace_consecutive_non_alphaum(std::string& str, char const replacement) {
  std::regex const non_alphanum_regex(R"(\W+)");
  std::string const replacement_str(1, replacement);
  str = std::regex_replace(str, non_alphanum_regex, replacement_str);
}

struct ValuesGenerator {
  ValuesGenerator(const std::string& table_name) : table_name_(table_name) {}

  template <typename... COL_ARGS>
  std::string operator()(COL_ARGS&&... args) const {
    std::vector<std::string> vals({convert(std::forward<COL_ARGS>(args))...});
    return std::string("INSERT INTO " + table_name_ + " VALUES(" +
                       boost::algorithm::join(vals, ",") + ");");
  }

  const std::string table_name_;
};

LeafHostInfo to_leaf_host_info(std::string& server_info, NodeRole role) {
  size_t pos = server_info.find(':');
  if (pos == std::string::npos) {
    throw std::runtime_error("Invalid host:port -> " + server_info);
  }

  auto host = server_info.substr(0, pos);
  auto port = server_info.substr(pos + 1);

  return LeafHostInfo(host, std::stoi(port), role);
}

std::vector<LeafHostInfo> to_leaf_host_info(std::vector<std::string>& server_infos,
                                            NodeRole role) {
  std::vector<LeafHostInfo> host_infos;

  for (auto& server_info : server_infos) {
    host_infos.push_back(to_leaf_host_info(server_info, role));
  }

  return host_infos;
}

void init_logger_stderr_only(int argc, char const* const* argv) {
  logger::LogOptions log_options(argv[0]);
  log_options.max_files_ = 0;  // stderr only by default
#ifdef BASE_PATH
  log_options.set_base_path(BASE_PATH);
#endif
  log_options.parse_command_line(argc, argv);
  logger::init(log_options);
}

void init_logger_stderr_only() {
  logger::LogOptions log_options(nullptr);
  log_options.max_files_ = 0;  // stderr only by default
  log_options.max_files_ = 0;  // stderr only by default
#ifdef BASE_PATH
  log_options.set_base_path(BASE_PATH);
#endif
  logger::init(log_options);
}

struct ShardInfo {
  const std::string shard_col;
  const size_t shard_count;
};

struct SharedDictionaryInfo {
  const std::string col;
  const std::string ref_table;
  const std::string ref_col;
};

std::string build_create_table_statement(
    const std::string& columns_definition,
    const std::string& table_name,
    const ShardInfo& shard_info,
    const std::vector<SharedDictionaryInfo>& shared_dict_info,
    const size_t fragment_size,
    const bool use_temporary_tables,
    const bool delete_support = true,
    const bool replicated = false) {
  const std::string shard_key_def{
      shard_info.shard_col.empty() ? "" : ", SHARD KEY (" + shard_info.shard_col + ")"};

  std::vector<std::string> shared_dict_def;
  if (shared_dict_info.size() > 0) {
    for (size_t idx = 0; idx < shared_dict_info.size(); ++idx) {
      shared_dict_def.push_back(", SHARED DICTIONARY (" + shared_dict_info[idx].col +
                                ") REFERENCES " + shared_dict_info[idx].ref_table + "(" +
                                shared_dict_info[idx].ref_col + ")");
    }
  }

  std::ostringstream with_statement_assembly;
  if (!shard_info.shard_col.empty()) {
    with_statement_assembly << "shard_count=" << shard_info.shard_count << ", ";
  }
  with_statement_assembly << "fragment_size=" << fragment_size;

  if (delete_support) {
    with_statement_assembly << ", vacuum='delayed'";
  } else {
    with_statement_assembly << ", vacuum='immediate'";
  }

  const std::string replicated_def{
      (!replicated || !shard_info.shard_col.empty()) ? "" : ", PARTITIONS='REPLICATED' "};

  const std::string create_def{use_temporary_tables ? "CREATE TEMPORARY TABLE "
                                                    : "CREATE TABLE "};

  return create_def + table_name + "(" + columns_definition + shard_key_def +
         boost::algorithm::join(shared_dict_def, "") + ") WITH (" +
         with_statement_assembly.str() + replicated_def + ");";
}

}  // namespace TestHelpers
