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

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "DateTruncate.h"
#include "ExtractFromTime.h"
#include "TableFunctionMetadataType.h"
#include "Utils/FlatBuffer.h"

#include "DateAdd.h"

#include "../Shared/sqltypes_lite.h"

#if !(defined(__CUDACC__) || defined(NO_BOOST))
#include "../Shared/DateTimeParser.h"
#endif

/* `../` is required for UDFCompiler */
#include "../Shared/InlineNullValues.h"
#include "../Shared/funcannotations.h"

#ifndef __CUDACC__
#ifndef UDF_COMPILED
#include "Shared/toString.h"
#include "StringDictionary/StringDictionaryProxy.h"
#endif  // #ifndef UDF_COMPILED
#endif  // #ifndef __CUDACC__

#include "../Geospatial/CompressionRuntime.h"

namespace gfx {
class GfxContext;
class CommandExecutionContext;
}  // namespace gfx

// declaring CPU functions as __host__ can help catch erroneous compilation of
// these being done by the CUDA compiler at build time
#define EXTENSION_INLINE_HOST extern "C" RUNTIME_EXPORT ALWAYS_INLINE HOST
#define EXTENSION_NOINLINE_HOST extern "C" RUNTIME_EXPORT NEVER_INLINE HOST

#define EXTENSION_INLINE extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE
#define EXTENSION_NOINLINE extern "C" RUNTIME_EXPORT NEVER_INLINE DEVICE
#define TEMPLATE_INLINE ALWAYS_INLINE DEVICE
#define TEMPLATE_NOINLINE NEVER_INLINE DEVICE

EXTENSION_NOINLINE int8_t* allocate_varlen_buffer(int64_t element_count,
                                                  int64_t element_size);

/*
  Table function management functions and macros:
 */
#define FUNC_NAME (std::string(__func__).substr(0, std::string(__func__).find("__")))
// TODO: support windows path format
#define ERROR_STRING(MSG)                                                     \
  (std::string(__FILE__).substr(std::string(__FILE__).rfind("/") + 1) + ":" + \
   std::to_string(__LINE__) + " " + FUNC_NAME + ": " + MSG)                   \
      .c_str()
#define TABLE_FUNCTION_ERROR(MSG) table_function_error(ERROR_STRING(MSG))
#define ERROR_MESSAGE(MSG) error_message(ERROR_STRING(MSG))
#define SUCCESS table_function_success_code()

EXTENSION_NOINLINE_HOST void set_output_item_values_total_number(
    int32_t index,
    int64_t output_item_values_total_number);
EXTENSION_NOINLINE_HOST void set_output_array_values_total_number(
    int32_t index,
    int64_t output_array_values_total_number);
EXTENSION_NOINLINE_HOST void set_output_row_size(int64_t num_rows);
EXTENSION_NOINLINE_HOST void TableFunctionManager_set_output_array_values_total_number(
    int8_t* mgr_ptr,
    int32_t index,
    int64_t output_array_values_total_number);
EXTENSION_NOINLINE_HOST void TableFunctionManager_set_output_item_values_total_number(
    int8_t* mgr_ptr,
    int32_t index,
    int64_t output_item_values_total_number);
EXTENSION_NOINLINE_HOST void TableFunctionManager_set_output_row_size(int8_t* mgr_ptr,
                                                                      int64_t num_rows);
EXTENSION_NOINLINE_HOST int8_t* TableFunctionManager_get_singleton();
EXTENSION_NOINLINE_HOST int32_t table_function_error(const char* message);
EXTENSION_NOINLINE_HOST int32_t table_function_success_code();
EXTENSION_NOINLINE_HOST int32_t TableFunctionManager_error_message(int8_t* mgr_ptr,
                                                                   const char* message);
EXTENSION_NOINLINE_HOST void TableFunctionManager_set_metadata(
    int8_t* mgr_ptr,
    const char* key,
    const uint8_t* raw_bytes,
    const size_t num_bytes,
    const TableFunctionMetadataType value_type);
EXTENSION_NOINLINE_HOST void TableFunctionManager_get_metadata(
    int8_t* mgr_ptr,
    const char* key,
    const uint8_t*& raw_bytes,
    size_t& num_bytes,
    TableFunctionMetadataType& value_type);

EXTENSION_NOINLINE_HOST int32_t TableFunctionManager_getNewDictDbId(int8_t* mgr_ptr);

EXTENSION_NOINLINE_HOST int32_t TableFunctionManager_getNewDictId(int8_t* mgr_ptr);

EXTENSION_NOINLINE_HOST int8_t* TableFunctionManager_getStringDictionaryProxy(
    int8_t* mgr_ptr,
    int32_t db_id,
    int32_t dict_id);

std::string TableFunctionManager_getString(int8_t* mgr_ptr,
                                           int32_t db_id,
                                           int32_t dict_id,
                                           int32_t string_id);

EXTENSION_NOINLINE_HOST int32_t TableFunctionManager_getOrAddTransient(int8_t* mgr_ptr,
                                                                       int32_t db_id,
                                                                       int32_t dict_id,
                                                                       std::string str);

EXTENSION_NOINLINE_HOST int8_t* TableFunctionManager_makeBuffer(int8_t* mgr_ptr,
                                                                int64_t count,
                                                                int64_t size);

EXTENSION_NOINLINE_HOST const gfx::GfxContext* TableFunctionManager_getGfxContext(
    int8_t* mgr_ptr);

EXTENSION_NOINLINE_HOST gfx::CommandExecutionContext*
TableFunctionManager_getGfxCommandExecutionContext(int8_t* mgr_ptr);

/*
  Row function management functions and macros:
 */

#define GET_DICT_DB_ID(mgr, arg_idx) (mgr.getDictDbId(__func__, arg_idx))

#define GET_DICT_ID(mgr, arg_idx) (mgr.getDictId(__func__, arg_idx))

RUNTIME_EXPORT NEVER_INLINE HOST std::string RowFunctionManager_getString(
    int8_t* mgr_ptr,
    int32_t db_id,
    int32_t dict_id,
    int32_t string_id);

EXTENSION_NOINLINE_HOST int32_t RowFunctionManager_getDictDbId(int8_t* mgr_ptr,
                                                               const char* func_name,
                                                               size_t arg_idx);

EXTENSION_NOINLINE_HOST int32_t RowFunctionManager_getDictId(int8_t* mgr_ptr,
                                                             const char* func_name,
                                                             size_t arg_idx);

EXTENSION_NOINLINE_HOST int32_t RowFunctionManager_getOrAddTransient(int8_t* mgr_ptr,
                                                                     int32_t db_id,
                                                                     int32_t dict_id,
                                                                     std::string str);
EXTENSION_NOINLINE_HOST int8_t* RowFunctionManager_makeBuffer(int8_t* mgr_ptr,
                                                              int64_t count,
                                                              int64_t size);
/*
  Column<Array<T>> methods
*/

EXTENSION_NOINLINE_HOST void ColumnArray_getArray(int8_t* flatbuffer,
                                                  const int64_t index,
                                                  const int64_t expected_numel,
                                                  int8_t*& ptr,
                                                  int64_t& size,  // items count
                                                  bool& is_null);

EXTENSION_NOINLINE_HOST bool ColumnArray_isNull(int8_t* flatbuffer, int64_t index);

EXTENSION_NOINLINE_HOST void ColumnArray_setNull(int8_t* flatbuffer, int64_t index);

EXTENSION_NOINLINE_HOST void ColumnArray_setArray(int8_t* flatbuffer,
                                                  int64_t index,
                                                  const int8_t* ptr,
                                                  int64_t size,
                                                  bool is_null);

EXTENSION_NOINLINE_HOST void ColumnArray_concatArray(int8_t* flatbuffer,
                                                     int64_t index,
                                                     const int8_t* ptr,
                                                     int64_t size,
                                                     bool is_null);

// For BC:
EXTENSION_NOINLINE_HOST void ColumnArray_getItem(int8_t* flatbuffer,
                                                 const int64_t index,
                                                 const int64_t expected_numel,
                                                 int8_t*& ptr,
                                                 int64_t& size,  // in bytes
                                                 bool& is_null,
                                                 int64_t sizeof_T);

EXTENSION_NOINLINE_HOST void ColumnArray_setItem(int8_t* flatbuffer,
                                                 int64_t index,
                                                 const int8_t* ptr,
                                                 int64_t size,
                                                 bool is_null,
                                                 int64_t sizeof_T);

EXTENSION_NOINLINE_HOST void ColumnArray_concatItem(int8_t* flatbuffer,
                                                    int64_t index,
                                                    const int8_t* ptr,
                                                    int64_t size,
                                                    bool is_null,
                                                    int64_t sizeof_T);

// https://www.fluentcpp.com/2018/04/06/strong-types-by-struct/
struct TextEncodingDict {
  int32_t value;

#ifndef __CUDACC__
  TextEncodingDict(const int32_t other) : value(other) {}
  TextEncodingDict() : value(0) {}
#endif

  operator int32_t() const {
    return value;
  }

  TextEncodingDict operator=(const int32_t other) {
    value = other;
    return *this;
  }

  DEVICE ALWAYS_INLINE bool isNull() const {
    return value == inline_int_null_value<int32_t>();
  }

  DEVICE ALWAYS_INLINE bool operator==(const TextEncodingDict& other) const {
    return value == other.value;
  }

  DEVICE ALWAYS_INLINE bool operator==(const int32_t& other) const {
    return value == other;
  }

  DEVICE ALWAYS_INLINE bool operator==(const int64_t& other) const {
    return value == other;
  }

  DEVICE ALWAYS_INLINE bool operator!=(const TextEncodingDict& other) const {
    return !operator==(other);
  }
  DEVICE ALWAYS_INLINE bool operator!=(const int32_t& other) const {
    return !operator==(other);
  }

  DEVICE ALWAYS_INLINE bool operator!=(const int64_t& other) const {
    return !operator==(other);
  }

  DEVICE ALWAYS_INLINE bool operator<(const TextEncodingDict& other) const {
    return value < other.value;
  }

  DEVICE ALWAYS_INLINE bool operator<(const int32_t& other) const {
    return value < other;
  }

  DEVICE ALWAYS_INLINE bool operator<(const int64_t& other) const {
    return value < other;
  }

#ifdef HAVE_TOSTRING
  std::string toString() const {
    return ::typeName(this) + "(value=" + ::toString(value) + ")";
  }
#endif
};

template <>
DEVICE inline TextEncodingDict inline_null_value() {
#ifndef __CUDACC__
  return TextEncodingDict(inline_int_null_value<int32_t>());
#else
  TextEncodingDict null_val;
  null_val.value = inline_int_null_value<int32_t>();
  return null_val;
#endif
}

#ifndef __CUDACC__

/*
  This RowFunctionManager struct is a minimal proxy to the
  RowFunctionManager defined in RowFunctionManager.h. The
  corresponding instances share `this` but have different virtual
  tables for methods.
*/

struct RowFunctionManager {
  std::string getString(int32_t db_id, int32_t dict_id, int32_t string_id) {
    return RowFunctionManager_getString(
        reinterpret_cast<int8_t*>(this), db_id, dict_id, string_id);
  }

  int32_t getDictDbId(const char* func_name, size_t arg_idx) {
    return RowFunctionManager_getDictDbId(
        reinterpret_cast<int8_t*>(this), func_name, arg_idx);
  }

  int32_t getDictId(const char* func_name, size_t arg_idx) {
    return RowFunctionManager_getDictId(
        reinterpret_cast<int8_t*>(this), func_name, arg_idx);
  }

  int32_t getOrAddTransient(int32_t db_id, int32_t dict_id, std::string str) {
    return RowFunctionManager_getOrAddTransient(
        reinterpret_cast<int8_t*>(this), db_id, dict_id, str);
  }

  int8_t* makeBuffer(int64_t element_count, int64_t element_size) {
    return RowFunctionManager_makeBuffer(
        reinterpret_cast<int8_t*>(this), element_count, element_size);
  }
};

#ifndef UDF_COMPILED

namespace {
template <typename T>
TableFunctionMetadataType get_metadata_type() {
  if constexpr (std::is_same<T, int8_t>::value) {
    return TableFunctionMetadataType::kInt8;
  } else if constexpr (std::is_same<T, int16_t>::value) {
    return TableFunctionMetadataType::kInt16;
  } else if constexpr (std::is_same<T, int32_t>::value) {
    return TableFunctionMetadataType::kInt32;
  } else if constexpr (std::is_same<T, int64_t>::value) {
    return TableFunctionMetadataType::kInt64;
  } else if constexpr (std::is_same<T, float>::value) {
    return TableFunctionMetadataType::kFloat;
  } else if constexpr (std::is_same<T, double>::value) {
    return TableFunctionMetadataType::kDouble;
  } else if constexpr (std::is_same<T, bool>::value) {
    return TableFunctionMetadataType::kBool;
  }
  throw std::runtime_error("Unsupported TableFunctionMetadataType");
}
}  // namespace

/*
  This TableFunctionManager struct is a minimal proxy to the
  TableFunctionManager defined in TableFunctionManager.h. The
  corresponding instances share `this` but have different virtual
  tables for methods.
*/

struct TableFunctionManager {
  static TableFunctionManager* get_singleton() {
    return reinterpret_cast<TableFunctionManager*>(TableFunctionManager_get_singleton());
  }

  void set_output_array_values_total_number(int32_t index,
                                            int64_t output_array_values_total_number) {
    TableFunctionManager_set_output_array_values_total_number(
        reinterpret_cast<int8_t*>(this), index, output_array_values_total_number);
  }

  void set_output_item_values_total_number(int32_t index,
                                           int64_t output_item_values_total_number) {
    TableFunctionManager_set_output_item_values_total_number(
        reinterpret_cast<int8_t*>(this), index, output_item_values_total_number);
  }

  void set_output_row_size(int64_t num_rows) {
    if (!output_allocations_disabled) {
      TableFunctionManager_set_output_row_size(reinterpret_cast<int8_t*>(this), num_rows);
    }
  }

  void disable_output_allocations() { output_allocations_disabled = true; }

  void enable_output_allocations() { output_allocations_disabled = false; }

  int32_t error_message(const char* message) {
    return TableFunctionManager_error_message(reinterpret_cast<int8_t*>(this), message);
  }

  template <typename T>
  void set_metadata(const std::string& key, const T& value) {
    TableFunctionManager_set_metadata(reinterpret_cast<int8_t*>(this),
                                      key.c_str(),
                                      reinterpret_cast<const uint8_t*>(&value),
                                      sizeof(value),
                                      get_metadata_type<T>());
  }

  template <typename T>
  void get_metadata(const std::string& key, T& value) {
    const uint8_t* raw_data{};
    size_t num_bytes{};
    TableFunctionMetadataType value_type;
    TableFunctionManager_get_metadata(
        reinterpret_cast<int8_t*>(this), key.c_str(), raw_data, num_bytes, value_type);
    if (sizeof(T) != num_bytes) {
      throw std::runtime_error("Size mismatch for Table Function Metadata '" + key + "'");
    }
    if (get_metadata_type<T>() != value_type) {
      throw std::runtime_error("Type mismatch for Table Function Metadata '" + key + "'");
    }
    std::memcpy(&value, raw_data, num_bytes);
  }
  int32_t getNewDictDbId() {
    return TableFunctionManager_getNewDictDbId(reinterpret_cast<int8_t*>(this));
  }
  int32_t getNewDictId() {
    return TableFunctionManager_getNewDictId(reinterpret_cast<int8_t*>(this));
  }
  StringDictionaryProxy* getStringDictionaryProxy(int32_t db_id, int32_t dict_id) {
    return reinterpret_cast<StringDictionaryProxy*>(
        TableFunctionManager_getStringDictionaryProxy(
            reinterpret_cast<int8_t*>(this), db_id, dict_id));
  }
  std::string getString(int32_t db_id, int32_t dict_id, int32_t string_id) {
    return TableFunctionManager_getString(
        reinterpret_cast<int8_t*>(this), db_id, dict_id, string_id);
  }
  int32_t getOrAddTransient(int32_t db_id, int32_t dict_id, std::string str) {
    return TableFunctionManager_getOrAddTransient(
        reinterpret_cast<int8_t*>(this), db_id, dict_id, str);
  }
  int8_t* makeBuffer(int64_t element_count, int64_t element_size) {
    return TableFunctionManager_makeBuffer(
        reinterpret_cast<int8_t*>(this), element_count, element_size);
  }

#ifdef HAVE_TOSTRING
  std::string toString() const {
    std::string result = ::typeName(this) + "(";
    if (!(void*)this) {  // cast to void* to avoid warnings
      result += "UNINITIALIZED";
    }
    result += ")";
    return result;
  }
#endif  // HAVE_TOSTRING
  bool output_allocations_disabled{false};
};

#endif  // #ifndef UDF_COMPILED
#endif  // #ifndef __CUDACC__

// Defines the maximal dimensionality of nested array objects.
#define NESTED_ARRAY_NDIM 4

#define IS_VALUE_TYPE(T)                                                   \
  (std::is_scalar<T>::value || std::is_same<T, TextEncodingDict>::value || \
   std::is_same<T, Geo::Point2D>::value)

namespace flatbuffer {
template <typename T>
struct Array;
}

namespace Geo {
struct Point2D;
}

struct TextEncodingNone;

template <typename T>
struct Array {
  T* ptr_;
  int64_t size_;
  int8_t is_null_;

  DEVICE Array(T* ptr, const int64_t size, const bool is_null = false)
      : ptr_(is_null ? nullptr : ptr), size_(size), is_null_(is_null) {}
  DEVICE Array() : ptr_(nullptr), size_(0), is_null_(true) {}

#ifndef __CUDACC__
  template <typename M>
  HOST Array(M& mgr, const int64_t size, const bool is_null = false)
      : size_(size), is_null_(is_null) {
    static_assert(
#ifndef UDF_COMPILED
        (std::is_same<TableFunctionManager, M>::value) ||
#endif  // #ifndef UDF_COMPILED
            (std::is_same<RowFunctionManager, M>::value),
        "M must be a TableFunctionManager or RowFunctionManager");
    if (!is_null_) {
      ptr_ = reinterpret_cast<T*>(mgr.makeBuffer(size, static_cast<int64_t>(sizeof(T))));
    } else {
      ptr_ = nullptr;
    }
  }
#endif  // #ifndef __CUDACC__

  DEVICE Array(const int64_t size, const bool is_null = false)
      : size_(size), is_null_(is_null) {
    if (!is_null) {
      // Memory must be manually released, otherwise leaks can happen.
      // On UDFs, if it is the return argument, memory is released with
      // register_buffer_with_executor_rsm
      ptr_ = reinterpret_cast<T*>(
          allocate_varlen_buffer(size, static_cast<int64_t>(sizeof(T))));
    } else {
      ptr_ = nullptr;
    }
  }

  DEVICE Array(const flatbuffer::Array<T>& arr) {
    bool _is_null;
    arr.getValuesBuffer(ptr_, size_, _is_null);
    is_null_ = _is_null;
  }

  DEVICE ALWAYS_INLINE T* data() const {
    return ptr_;
  }

  DEVICE ALWAYS_INLINE size_t size() const {
    return size_;
  }

  DEVICE T operator()(const unsigned int index) const {
#ifndef UDF_COMPILED
    // TODO: implement Array<TextEncodingNone> indexing support
    static_assert(!std::is_same<T, TextEncodingNone>::value);
#endif
    if (index < static_cast<unsigned int>(size_)) {
      return ptr_[index];
    } else {
      return 0;  // see array_at
    }
  }

  DEVICE T& operator[](const unsigned int index) {
#ifndef UDF_COMPILED
    // TODO: implement Array<TextEncodingNone> indexing support
    static_assert(!std::is_same<T, TextEncodingNone>::value);
#endif
    return ptr_[index];
  }

  DEVICE const T& operator[](const unsigned int index) const {
#ifndef UDF_COMPILED
    // TODO: implement Array<TextEncodingNone> indexing support
    static_assert(!std::is_same<T, TextEncodingNone>::value);
#endif
    return ptr_[index];
  }

  DEVICE int64_t getSize() const {
    return size_;
  }

  DEVICE bool isNull() const {
    return is_null_;
  }

  DEVICE constexpr inline T null_value() const {
    if constexpr (std::is_same<T, TextEncodingNone>::value) {
      return {};
    } else {
      return inline_null_value<T>();
    }
  }

  DEVICE bool isNull(const unsigned int index) const {
    return (is_null_ ? false : ptr_[index] == null_value());
  }

  DEVICE bool operator==(const Array& other) const {
    if (isNull() || other.isNull() || size() != other.size()) {
      return false;
    }
    for (int64_t i = 0; i < size_; i++) {
      if ((*this)[i] != other[i]) {
        return false;
      }
    }
    return true;
  }

#ifdef HAVE_TOSTRING
  std::string toString() const {
    std::string result = ::typeName(this) +
                         "(ptr=" + ::toString(reinterpret_cast<void*>(ptr_)) +
                         ", size=" + std::to_string(size_) +
                         ", is_null=" + std::to_string(is_null_) + ")[";
    for (int64_t i = 0; i < size_; i++) {
      if (size_ > 10) {
        // show the first 8 and the last 2 values in the array:
        if (i == 8) {
          result += "..., ";
        } else if (i > 8 && i < size_ - 2) {
          continue;
        }
      }
      result += ::toString((*this)[i]) + ", ";
    }
    result += "]";
    return result;
  }
#endif
};

struct TextEncodingNone {
  char* ptr_;
  int64_t size_;
  // previous comment related to `is_null_` variable (which was defined as int8_t
  // padding): padding is required to prevent clang/gcc from expanding arguments
  // https://stackoverflow.com/questions/27386912/prevent-clang-from-expanding-arguments-that-are-aggregate-types/27387908#27387908
  // let's change its name from `padding` to `is_null_` since we use this field to mark
  // whether the string is null but keep its type, not changing it as bool explicitly
  int8_t is_null_;

#ifndef __CUDACC__
  TextEncodingNone() = default;
  // Requires external ownership.
  explicit TextEncodingNone(char const* const c_str)
      : ptr_(const_cast<char*>(c_str))
      , size_(strlen(c_str))
      , is_null_(static_cast<int8_t>(size_ == 0)) {}

  template <typename M>
  explicit DEVICE ALWAYS_INLINE TextEncodingNone(M& mgr, const std::string& str) {
    static_assert(
#ifndef UDF_COMPILED
        (std::is_same<TableFunctionManager, M>::value) ||
#endif  // #ifndef UDF_COMPILED
            (std::is_same<RowFunctionManager, M>::value),
        "M must be a TableFunctionManager or RowFunctionManager");
    size_ = str.length();
    if (str.empty()) {
      ptr_ = nullptr;
    } else {
      int8_t* buffer = mgr.makeBuffer(size_, static_cast<int64_t>(sizeof(char)));
      ptr_ = reinterpret_cast<char*>(buffer);
      strcpy(ptr_, str.c_str());
    }
    is_null_ = static_cast<int8_t>(size_ == 0);
  }

  operator std::string() const {
    return std::string(ptr_, size_);
  }
  std::string getString() const {
    return std::string(ptr_, size_);
  }
#endif

  DEVICE ALWAYS_INLINE char& operator[](const unsigned int index) {
    return index < size_ ? ptr_[index] : ptr_[size_ - 1];
  }
  DEVICE ALWAYS_INLINE bool operator==(const char* rhs) const {
#ifdef __CUDACC__
    for (int i = 0; i < size_; i++) {
      if (rhs[i] == '\0' || ptr_[i] != rhs[i]) {
        return false;
      }
    }
    return rhs[size_] == '\0';
#else
    return strcmp(ptr_, rhs) == 0;
#endif
  }
  DEVICE ALWAYS_INLINE bool operator==(const TextEncodingNone& rhs) const {
    if (isNull() || rhs.isNull() || size_ != rhs.size()) {
      return false;
    }
#ifdef __CUDACC__
    for (int i = 0; i < size_; i++) {
      if ((*this)[i] != rhs[i]) {
        return false;
      }
    }
    return true;
#else
    return strncmp(ptr_, rhs.data(), size_) == 0;
#endif
  }
  DEVICE ALWAYS_INLINE bool operator!=(const char* rhs) const {
    return !(this->operator==(rhs));
  }
  DEVICE ALWAYS_INLINE bool operator!=(const TextEncodingNone& rhs) const {
    return !(this->operator==(rhs));
  }
  DEVICE ALWAYS_INLINE operator char*() const {
    return ptr_;
  }
  DEVICE ALWAYS_INLINE char* data() const {
    return ptr_;
  }
  DEVICE ALWAYS_INLINE int64_t size() const {
    return size_;
  }
  DEVICE ALWAYS_INLINE bool isNull() const {
    return is_null_;
  }
};

struct DayTimeInterval;
struct YearMonthTimeInterval;
struct Timestamp {
  int64_t time;

  Timestamp() = default;

  DEVICE Timestamp(int64_t timeval) : time(timeval) {}

  DEVICE ALWAYS_INLINE Timestamp operator+(const DayTimeInterval& interval) const;
  DEVICE ALWAYS_INLINE Timestamp operator+(const YearMonthTimeInterval& interval) const;

#if !(defined(__CUDACC__) || defined(NO_BOOST))
  DEVICE Timestamp(std::string_view const str) {
    time = dateTimeParse<kTIMESTAMP>(str, 9);
  }
#endif

  DEVICE ALWAYS_INLINE const Timestamp operator+(const Timestamp& other) const {
#ifndef __CUDACC__
    if (other.time > 0) {
      if (time > (std::numeric_limits<int64_t>::max() - other.time)) {
        throw std::underflow_error("Underflow in Timestamp addition!");
      }
    } else {
      if (time < (std::numeric_limits<int64_t>::min() - other.time)) {
        throw std::overflow_error("Overflow in Timestamp addition!");
      }
    }
#endif
    return Timestamp(time + other.time);
  }

  DEVICE ALWAYS_INLINE const Timestamp operator-(const Timestamp& other) const {
#ifndef __CUDACC__
    if (other.time > 0) {
      if (time < (std::numeric_limits<int64_t>::min() + other.time)) {
        throw std::underflow_error("Underflow in Timestamp substraction!");
      }
    } else {
      if (time > (std::numeric_limits<int64_t>::max() + other.time)) {
        throw std::overflow_error("Overflow in Timestamp substraction!");
      }
    }
#endif
    return Timestamp(time - other.time);
  }

  DEVICE ALWAYS_INLINE int64_t operator/(const Timestamp& other) const {
#ifndef __CUDACC__
    if (other.time == 0) {
      throw std::runtime_error("Timestamp division by zero!");
    }
#endif
    return time / other.time;
  }

  DEVICE ALWAYS_INLINE const Timestamp operator*(const int64_t multiplier) const {
#ifndef __CUDACC__
    int64_t overflow_test = static_cast<int64_t>(time) * static_cast<int64_t>(multiplier);
    if (time != 0 && overflow_test / time != static_cast<int64_t>(multiplier)) {
      throw std::overflow_error("Overflow in Timestamp multiplication!");
    }
#endif
    return Timestamp(time * multiplier);
  }

  DEVICE ALWAYS_INLINE bool operator==(const Timestamp& other) const {
    return time == other.time;
  }

  DEVICE ALWAYS_INLINE bool operator!=(const Timestamp& other) const {
    return !operator==(other);
  }

  DEVICE ALWAYS_INLINE bool operator<(const Timestamp& other) const {
    return time < other.time;
  }

  DEVICE ALWAYS_INLINE Timestamp truncateToMicroseconds() const {
    return Timestamp((time / kMilliSecsPerSec) * kMilliSecsPerSec);
  }

  DEVICE ALWAYS_INLINE Timestamp truncateToMilliseconds() const {
    return Timestamp((time / kMicroSecsPerSec) * kMicroSecsPerSec);
  }

  DEVICE ALWAYS_INLINE Timestamp truncateToSeconds() const {
    return Timestamp((time / kNanoSecsPerSec) * kNanoSecsPerSec);
  }

  DEVICE ALWAYS_INLINE Timestamp truncateToMinutes() const {
    return Timestamp(DateTruncate(dtMINUTE, (time / kNanoSecsPerSec)) * kNanoSecsPerSec);
  }

  DEVICE ALWAYS_INLINE Timestamp truncateToHours() const {
    return Timestamp(DateTruncate(dtHOUR, (time / kNanoSecsPerSec)) * kNanoSecsPerSec);
  }

  DEVICE ALWAYS_INLINE Timestamp truncateToDay() const {
    return Timestamp(DateTruncate(dtDAY, (time / kNanoSecsPerSec)) * kNanoSecsPerSec);
  }

  DEVICE ALWAYS_INLINE Timestamp truncateToMonth() const {
    return Timestamp(DateTruncate(dtMONTH, (time / kNanoSecsPerSec)) * kNanoSecsPerSec);
  }

  DEVICE ALWAYS_INLINE Timestamp truncateToYear() const {
    return Timestamp(DateTruncate(dtYEAR, (time / kNanoSecsPerSec)) * kNanoSecsPerSec);
  }

  DEVICE ALWAYS_INLINE int64_t getNanoseconds() const {
    return ExtractFromTime(kNANOSECOND, time);
  }
  // Should always be safe as we're downcasting to lower precisions
  DEVICE ALWAYS_INLINE int64_t getMicroseconds() const {
    return ExtractFromTime(kMICROSECOND, time / (kNanoSecsPerSec / kMicroSecsPerSec));
  }
  // Should always be safe as we're downcasting to lower precisions
  DEVICE ALWAYS_INLINE int64_t getMilliseconds() const {
    return ExtractFromTime(kMILLISECOND, time / (kNanoSecsPerSec / kMilliSecsPerSec));
  }
  // Should always be safe as we're downcasting to lower precisions
  DEVICE ALWAYS_INLINE int64_t getSeconds() const {
    return ExtractFromTime(kSECOND, time / kNanoSecsPerSec);
  }
  DEVICE ALWAYS_INLINE int64_t getMinutes() const {
    return ExtractFromTime(kMINUTE, time / kNanoSecsPerSec);
  }
  DEVICE ALWAYS_INLINE int64_t getHours() const {
    return ExtractFromTime(kHOUR, time / kNanoSecsPerSec);
  }
  DEVICE ALWAYS_INLINE int64_t getDay() const {
    return ExtractFromTime(kDAY, time / kNanoSecsPerSec);
  }
  DEVICE ALWAYS_INLINE int64_t getMonth() const {
    return ExtractFromTime(kMONTH, time / kNanoSecsPerSec);
  }
  DEVICE ALWAYS_INLINE int64_t getYear() const {
    return ExtractFromTime(kYEAR, time / kNanoSecsPerSec);
  }
#ifdef HAVE_TOSTRING
  std::string toString() const {
    Datum d;
    d.bigintval = time;
    return ::typeName(this) + "(time=" + ::toString(time) + ")"
#if !(defined(__CUDACC__) || defined(NO_BOOST))
           + ", (formatted= " + DatumToString(d, SQLTypeInfo(kTIMESTAMP, 9, 0, false)) +
           ")"
#endif
        ;
  }
#endif
};

template <>
DEVICE inline Timestamp inline_null_value() {
  return Timestamp(inline_int_null_value<int64_t>());
}

struct DayTimeInterval {
  int64_t timeval;

  DEVICE DayTimeInterval(int64_t init) : timeval(init) {}

  DEVICE ALWAYS_INLINE bool operator==(const DayTimeInterval& other) const {
    return timeval == other.timeval;
  }

  DEVICE ALWAYS_INLINE bool operator!=(const DayTimeInterval& other) const {
    return !operator==(other);
  }

  DEVICE ALWAYS_INLINE Timestamp operator+(const Timestamp& t) const {
    return Timestamp(DateAddHighPrecisionNullable(
        daMILLISECOND, timeval, t.time, 9, inline_int_null_value<int64_t>()));
  }

  DEVICE ALWAYS_INLINE DayTimeInterval operator*(const int64_t multiplier) const {
    return DayTimeInterval(timeval * multiplier);
  }

  DEVICE ALWAYS_INLINE int64_t numStepsBetween(const Timestamp& begin,
                                               const Timestamp& end) const {
#ifndef __CUDACC__
    if (timeval == 0) {
      throw std::runtime_error("Timestamp division by zero!");
    }
#endif

    if ((timeval > 0 && end.time < begin.time) ||
        (timeval < 0 && end.time > begin.time)) {
      return -1;
    }

    Timestamp diff = end.time - begin.time;
    int64_t asNanoSecs =
        static_cast<int64_t>(timeval) * static_cast<int64_t>(kMicroSecsPerSec);
#ifndef __CUDACC__
    if (timeval != 0 && asNanoSecs / timeval != static_cast<int64_t>(kMicroSecsPerSec)) {
      throw std::overflow_error("Overflow in INTERVAL precision conversion!");
    }
#endif

    return diff / asNanoSecs;
  }
};

struct YearMonthTimeInterval {
  int64_t timeval;

  DEVICE YearMonthTimeInterval(int64_t init) : timeval(init) {}

  DEVICE ALWAYS_INLINE bool operator==(const YearMonthTimeInterval& other) const {
    return timeval == other.timeval;
  }

  DEVICE ALWAYS_INLINE bool operator!=(const YearMonthTimeInterval& other) const {
    return !operator==(other);
  }

  DEVICE ALWAYS_INLINE Timestamp operator+(const Timestamp& t) const {
    return Timestamp(DateAddHighPrecisionNullable(
        daMONTH, timeval, t.time, 9, inline_int_null_value<int64_t>()));
  }

  DEVICE ALWAYS_INLINE YearMonthTimeInterval operator*(const int64_t multiplier) const {
    return YearMonthTimeInterval(timeval * multiplier);
  }

  DEVICE ALWAYS_INLINE int64_t numStepsBetween(const Timestamp& begin,
                                               const Timestamp& end) const {
    if ((timeval > 0 && end.time < begin.time) ||
        (timeval < 0 && end.time > begin.time)) {
      return -1;
    }

    int64_t ret = ((end.getYear() * 12 + end.getMonth()) -
                   (begin.getYear() * 12 + begin.getMonth())) /
                  timeval;
    return ret;
  }
};

DEVICE ALWAYS_INLINE inline Timestamp Timestamp::operator+(
    const DayTimeInterval& interval) const {
  return interval + *this;
}

DEVICE ALWAYS_INLINE inline Timestamp Timestamp::operator+(
    const YearMonthTimeInterval& interval) const {
  return interval + *this;
}

struct GeoPointStruct {
  int8_t* ptr;
  int32_t sz;
  int32_t compression;
  int32_t input_srid;
  int32_t output_srid;

  DEVICE int64_t getSize() const { return sz; }

  DEVICE int32_t getCompression() const { return compression; }

  DEVICE int32_t getInputSrid() const { return input_srid; }

  DEVICE int32_t getOutputSrid() const { return output_srid; }
};

typedef struct GeoPointStruct GeoPoint;

struct GeoMultiPointStruct {
  int8_t* ptr;
  int32_t sz;
  int32_t compression;
  int32_t input_srid;
  int32_t output_srid;

  DEVICE int32_t getSize() const { return sz; }

  DEVICE int32_t getCompression() const { return compression; }

  DEVICE int32_t getInputSrid() const { return input_srid; }

  DEVICE int32_t getOutputSrid() const { return output_srid; }
};

typedef struct GeoMultiPointStruct GeoMultiPoint;

struct GeoLineStringStruct {
  int8_t* ptr;
  int32_t sz;
  int32_t compression;
  int32_t input_srid;
  int32_t output_srid;

  DEVICE int32_t getSize() const { return sz; }

  DEVICE int32_t getCompression() const { return compression; }

  DEVICE int32_t getInputSrid() const { return input_srid; }

  DEVICE int32_t getOutputSrid() const { return output_srid; }
};

typedef struct GeoLineStringStruct GeoLineString;

struct GeoMultiLineStringStruct {
  int8_t* ptr;
  int32_t sz;
  int8_t* linestring_sizes;
  int32_t num_linestrings;
  int32_t compression;
  int32_t input_srid;
  int32_t output_srid;

  DEVICE int8_t* getCoords() const { return ptr; }

  DEVICE int32_t getCoordsSize() const { return sz; }

  DEVICE int8_t* getLineStringSizes() { return linestring_sizes; }

  DEVICE int32_t getNumLineStrings() const { return num_linestrings; }

  DEVICE int32_t getCompression() const { return compression; }

  DEVICE int32_t getInputSrid() const { return input_srid; }

  DEVICE int32_t getOutputSrid() const { return output_srid; }
};

typedef struct GeoMultiLineStringStruct GeoMultiLineString;

struct GeoPolygonStruct {
  int8_t* ptr_coords;
  int32_t coords_size;
  int8_t* ring_sizes;
  int32_t num_rings;
  int32_t compression;
  int32_t input_srid;
  int32_t output_srid;

  DEVICE int8_t* getRingSizes() { return ring_sizes; }
  DEVICE int32_t getCoordsSize() const { return coords_size; }

  DEVICE int32_t getNumRings() const { return num_rings; }

  DEVICE int32_t getCompression() const { return compression; }

  DEVICE int32_t getInputSrid() const { return input_srid; }

  DEVICE int32_t getOutputSrid() const { return output_srid; }
};

typedef struct GeoPolygonStruct GeoPolygon;

struct GeoMultiPolygonStruct {
  int8_t* ptr_coords;
  int32_t coords_size;
  int8_t* ring_sizes;
  int32_t num_rings;
  int8_t* poly_sizes;
  int32_t num_polys;
  int32_t compression;
  int32_t input_srid;
  int32_t output_srid;

  DEVICE int8_t* getRingSizes() { return ring_sizes; }
  DEVICE int32_t getCoordsSize() const { return coords_size; }

  DEVICE int32_t getNumRings() const { return num_rings; }

  DEVICE int8_t* getPolygonSizes() { return poly_sizes; }

  DEVICE int32_t getNumPolygons() const { return num_polys; }

  DEVICE int32_t getCompression() const { return compression; }

  DEVICE int32_t getInputSrid() const { return input_srid; }

  DEVICE int32_t getOutputSrid() const { return output_srid; }
};

typedef struct GeoMultiPolygonStruct GeoMultiPolygon;

// There are redundant #ifndef UDF_COMPILED inside
// ifguard for StringDictionaryProxy to flag that
// if we decide to adapt C++ UDF Compiler for table
// functions, the linking issue we encountered with
// the shared_mutex include in StringDicitonaryProxy
// will need to be resolved separately.

#ifndef UDF_COMPILED

#ifdef __CUDACC__
template <typename T>
static DEVICE __constant__ T Column_null_value;
#endif

template <typename T>
struct Column {
  T* ptr_;            // row data
  int64_t num_rows_;  // row count

  DEVICE Column(T* ptr, const int64_t num_rows) : ptr_(ptr), num_rows_(num_rows) {}

#ifndef __CUDACC__
#ifndef UDF_COMPILED
  DEVICE Column(const Column& other) : ptr_(other.ptr_), num_rows_(other.num_rows_) {}
  DEVICE Column(std::vector<T>& input_vec)
      : ptr_(input_vec.data()), num_rows_(static_cast<int64_t>(input_vec.size())) {}
#endif  // #ifndef UDF_COMPILED
#endif  // #ifndef __CUDACC__

  DEVICE T& operator[](const unsigned int index) const {
    if (index >= num_rows_) {
#ifndef __CUDACC__
      throw std::runtime_error("column buffer index is out of range");
#else
      auto& null_value = Column_null_value<T>;
      set_null(null_value);
      return null_value;
#endif
    }
    return ptr_[index];
  }
  DEVICE inline T* getPtr() const {
    return ptr_;
  }
  DEVICE inline int64_t size() const {
    return num_rows_;
  }
  DEVICE inline void setSize(int64_t num_rows) {
    num_rows_ = num_rows;
  }

  DEVICE inline bool isNull(int64_t index) const {
    return is_null(ptr_[index]);
  }
  DEVICE inline void setNull(int64_t index) {
    set_null(ptr_[index]);
  }
  DEVICE Column<T>& operator=(const Column<T>& other) {
#ifndef __CUDACC__
    if (size() == other.size()) {
      memcpy(ptr_, &other[0], other.size() * sizeof(T));
    } else {
      throw std::runtime_error("cannot copy assign columns with different sizes");
    }
#else
    if (size() == other.size()) {
      for (unsigned int i = 0; i < size(); i++) {
        ptr_[i] = other[i];
      }
    } else {
      // TODO: set error
    }
#endif
    return *this;
  }

#ifdef HAVE_TOSTRING
  std::string toString() const {
    return ::typeName(this) + "(ptr=" + ::toString(reinterpret_cast<void*>(ptr_)) +
           ", num_rows=" + std::to_string(num_rows_) + ")";
  }
#endif
};

namespace flatbuffer {

// clang-format off
/*
  flatbuffer::Column<RowType, RowStruct> is a base class for various Column types
  that use FlatBuffer storage. It provides an API to access column
  rows:

    bool isNull(int64_t index)      --- check if index-th row is a NULL row
    int64_t size()                  --- return the total number of rows
    RowType getItem(int64_t index)  --- get the index-th row
    RowType operator[](unsigned int index)   --- same as getItem
    RowStruct operator()(unsigned int index) --- getItem result returned as RowStruct
    void setNull(int64_t index)     --- set the index-th row to NULL
    voit setItem(int64_t index, const RowType& item) --- set the index-th row to item

  RowType class is a substruct of NestedArray<ItemType>. If the row
  item is a some sort of a sequence of, say, an array of scalar
  values, geo points, etc., then ItemType itself can be a subclass of
  NestedArray<SubItemType>. The same applies to SubItemType etc.

  RowStruct is a struct capturing the data of a row as a buffer and
  its size.

  NestedArray<ItemType, ...> provides an API for accessing its items:

    size_t size() --- return the number of items in the NestedArray
                      instance
    size_t size(int64_t index)        --- return the number of subitems in an item
    ItemType getItem(int64_t index)   --- return the index-th item
    ItemType operator[](unsigned int index)  --- same as getItem
    bool isNull()                     --- check if NestedArray instance itself is NULL
    NestedArray<ItemType> operator=(NestedArray<ItemType>& other)
         --- copy other items to self

  If ItemType class is not a subclass of NestedArray, then classes
  derived from NestedArray<ItemType> must
  implement the following methods:

    ItemType getItem(const int64_t index);
    ItemType getItem(const int64_t index) const;
    ItemType operator[](unsigned int index);

  Notice that classes derived from NestedArray do not implement
  setItem nor setNull methods. Only flatbuffer::Column<RowType>::setItem
  method can be used to allocate and initialize items. The setItem can
  be called exactly once per index value.

  To change the content of existing NestedArray item, the operator=
  interface can be used. However, then the NestedArray item must have
  exactly the same shape as in the other NestedArray item has.
*/
// clang-format on

template <typename RowType, typename RowStruct>
struct Column {
  int8_t* flatbuffer_;
  int64_t num_rows_;

  Column(int8_t* flatbuffer, int64_t num_rows)
      : flatbuffer_(flatbuffer), num_rows_(num_rows) {}

  // Return true if index-th row is NULL.
  DEVICE inline bool isNull(int64_t index) const {
    FlatBufferManager m{flatbuffer_};
    bool is_null = false;
    auto status = m.isNull(index, is_null);
#ifndef __CUDACC__
    if (status != FlatBufferManager::Status::Success) {
      throw std::runtime_error("isNull failed: " + ::toString(status));
    }
#endif
    return is_null;
  }

  // Return the number of rows.
  DEVICE int64_t size() const {
    return num_rows_;
  }

  // Set the index-th row to NULL. Can be called once per row.
  DEVICE inline void setNull(int64_t index) {
    FlatBufferManager m{flatbuffer_};
    auto status = m.setNull(index);
#ifndef __CUDACC__
    if (status != FlatBufferManager::Status::Success) {
      throw std::runtime_error("setNull failed: " + ::toString(status));
    }
#endif
  }

  // Return row object.
  DEVICE inline RowType getItem(const int64_t index) const {
    RowType row{{flatbuffer_, {index}, 1}};
    return row;
  }

  // Return row object via indexing.
  DEVICE inline RowType operator[](const unsigned int index) const {
    return getItem(static_cast<int64_t>(index));
  }

  // Return row buffer object.
  DEVICE inline RowStruct operator()(const unsigned int index) const {
    return (*this)[index];
  }

  // Copy item into the index-th row.
  DEVICE inline void setItem(int64_t index, const RowType& item) {
    RowType this_item = getItem(index);
    this_item = item;
  }

  // Copy item into the index-th row.
  DEVICE inline void setItem(int64_t index, const RowStruct& item) {
    RowType this_item = getItem(index);
    this_item = item;
  }

  // Return the total number of values that the flatbuffer instance
  // holds.
  inline int64_t getNofValues() const {
    FlatBufferManager m{flatbuffer_};
    return m.getValuesCount();
  }

  DEVICE inline void concatItem(int64_t index, const RowType& item) {
    RowType this_item = getItem(index);
    this_item += item;
  }

  DEVICE inline void concatItem(int64_t index, const RowStruct& item) {
    RowType this_item = getItem(index);
    this_item += item;
  }

  // Return row object with the specified extra number of elements.
  DEVICE RowType getItem(const int64_t index, const int64_t extra_numel) const {
    auto result = (*this)[index];
    if (extra_numel >= 0) {
      result.extend(nullptr, extra_numel, /*assing=*/false);
    }
    return result;
  }

  const SQLTypeInfoLite* getTypeInfo() const {
    FlatBufferManager m{flatbuffer_};
    const auto* ti = reinterpret_cast<const SQLTypeInfoLite*>(m.get_user_data_buffer());
    if (ti == nullptr) {
#ifndef __CUDACC__
      throw std::runtime_error(::typeName(this) +
                               " getTypeInfo failed: no user data buffer");
#endif
    }
    return ti;
  }

#ifdef HAVE_FLATBUFFER_TOSTRING
  std::string toString() const {
    FlatBufferManager m{flatbuffer_};
    return ::typeName(this) + "(" + m.toString() +
           ", num_rows=" + std::to_string(num_rows_) + ")";
  }
#endif
};

template <typename ItemType>
struct NestedArray {
  /*
    flatbuffer_ contains a NestedArray with dimensionality up to NESTED_ARRAY_NDIM.

    index_ contains indices used to index a NestedArray

    n_ defines how many indices in index_ are used in the indexing operation
  */

  int8_t* flatbuffer_;
  int64_t index_[NESTED_ARRAY_NDIM];
  size_t n_{0};

  size_t size() const {
    FlatBufferManager m{flatbuffer_};
    size_t length;
    FlatBufferManager::Status status = FlatBufferManager::Status::NotImplementedError;
    status = m.getLength<NESTED_ARRAY_NDIM>(index_, n_, length);
    if (status != FlatBufferManager::Status::Success) {
#ifndef __CUDACC__
      throw std::runtime_error(::typeName(this) + " size failed: getLength failed with " +
                               ::toString(status));
#endif
      return 0;
    }
    return length;
  }

  size_t size(const int64_t index) const {
    return getItem(index).size();
  }

  void getValuesBuffer(ItemType*& values,
                       int64_t& nof_values,
                       bool& is_null,
                       bool require_1d_item = true) const {
    int8_t* _values;
    size_t value_size;
    getRawBuffer(_values, nof_values, value_size, is_null, require_1d_item);
    values = reinterpret_cast<ItemType*>(_values);
  }

  void getRawBuffer(int8_t*& values,
                    int64_t& nof_values,
                    size_t& value_size,
                    bool& is_null,
                    bool require_1d_item = true) const {
    values = nullptr;
    nof_values = 0;
    is_null = true;
    value_size = 0;
    if constexpr (!IS_VALUE_TYPE(ItemType)) {
#ifndef __CUDACC__
      ItemType item{};
      throw std::runtime_error(::typeName(this) +
                               " getValuesBuffer failed: expected scalar type but got " +
                               ::typeName(&item));
#endif
    } else {
      FlatBufferManager::Status status{};
      FlatBufferManager m{flatbuffer_};
      FlatBufferManager::NestedArrayItem<NESTED_ARRAY_NDIM> item;
      status = m.getItem<NESTED_ARRAY_NDIM>(index_, n_, item);
      if (status != FlatBufferManager::Status::Success) {
#ifndef __CUDACC__
        throw std::runtime_error(::typeName(this) +
                                 " getValuesBuffer failed: getItem returned " +
                                 ::toString(status));
#endif
      }
      if (require_1d_item && item.nof_sizes != 0) {
#ifndef __CUDACC__
        throw std::runtime_error(::typeName(this) +
                                 " &getValuesBuffer failed: expected 1-D item");
#endif
      }
      values = item.values;
      nof_values = item.nof_values;
      is_null = item.is_null;
      value_size = m.getValueSize();
    }
  }

  // Return reference to index-th item that is of scalar type.
  // For the direct assess to the array buffer, one can use `&getValue(0)`
  ItemType& getValue(const int64_t index) const {
    if constexpr (!IS_VALUE_TYPE(ItemType)) {
#ifndef __CUDACC__
      ItemType item{};
      throw std::runtime_error(::typeName(this) +
                               " &getValue failed: expected scalar type but got " +
                               ::typeName(&item));
#endif
    }
    ItemType* values;
    int64_t nof_values;
    bool is_null;
    getValuesBuffer(values, nof_values, is_null);
    if (is_null) {
#ifndef __CUDACC__
      throw std::runtime_error(::typeName(this) +
                               " &getValue failed: expected non-null item ");
#endif
    }
    if (index < 0 || index >= nof_values) {
#ifndef __CUDACC__
      throw std::runtime_error(
          ::typeName(this) + " &getValue failed: index (=" + std::to_string(index) +
          ") is out of range, expected 0 <= index < " + std::to_string(nof_values));
#endif
    }
    return values[index];
  }

  // same as getItem
  const ItemType operator[](const int64_t index) const {
    return getItem(index);
  }

  // Return index-th item,
  const ItemType getItem(const int64_t index) const {
    if constexpr (IS_VALUE_TYPE(ItemType)) {
      return getValue(index);
    } else {
      ItemType result{flatbuffer_, {}, n_ + 1};
      for (size_t i = 0; i < n_; i++) {
        result.index_[i] = index_[i];
      }
      result.index_[n_] = index;
      return result;
    }
  }

  // Check if the parent is NULL
  inline bool isNull() const {
    FlatBufferManager m{flatbuffer_};
    bool is_null = false;
    auto status = m.isNull(index_[0], is_null);
#ifndef __CUDACC__
    if (status != FlatBufferManager::Status::Success) {
      throw std::runtime_error(::typeName(this) + "isNull failed: isNull returned " +
                               ::toString(status));
    }
#endif
    return is_null;
  }

  // Check if the index-th item has NULL value
  inline bool isNull(int64_t index) const {
    FlatBufferManager m{flatbuffer_};
    bool is_null = false;
    int64_t tmp_index[NESTED_ARRAY_NDIM];
    for (size_t i = 0; i < n_; i++) {
      tmp_index[i] = index_[i];
    }
    tmp_index[n_] = index;
    auto status =
        m.isNull<NESTED_ARRAY_NDIM>(tmp_index, static_cast<size_t>(n_ + 1), is_null);
#ifndef __CUDACC__
    if (status != FlatBufferManager::Status::Success) {
      throw std::runtime_error(::typeName(this) +
                               "isNull(index) failed: isNull returned " +
                               ::toString(status));
    }
#endif
    return is_null;
  }

  // Extends or assigns other item to item
  DEVICE void extend(const int8_t* data, const int32_t size, bool assign = false) {
    if (n_ != 1) {
#ifndef __CUDACC__
      throw std::runtime_error(::typeName(this) +
                               " extend failed: expected single index, got " +
                               ::toString(n_));
#endif
    }
    FlatBufferManager::Status status{};
    FlatBufferManager m{flatbuffer_};
    if (assign) {
      status = m.setItem(index_[0], data, size);
      if (status != FlatBufferManager::Status::Success) {
#ifndef __CUDACC__
        throw std::runtime_error(
            ::typeName(this) +
            " extend failed: setItem failed with: " + ::toString(status));
#endif
      }
    } else {
      status = m.concatItem(index_[0], data, size);
      if (status != FlatBufferManager::Status::Success) {
#ifndef __CUDACC__
        throw std::runtime_error(
            ::typeName(this) +
            " extend failed: concatItem failed with: " + ::toString(status));
#endif
      }
    }
  }

  void extend(const NestedArray<ItemType>& other, bool assign = false) {
    // TODO: check flatbuffer_ equality
    if (n_ != 1) {
#ifndef __CUDACC__
      throw std::runtime_error(::typeName(this) +
                               "extend failed: expected single index, got " +
                               ::toString(n_));
#endif
    }
    FlatBufferManager other_m{other.flatbuffer_};
    FlatBufferManager::NestedArrayItem<NESTED_ARRAY_NDIM> item;
    auto status = other_m.getItem<NESTED_ARRAY_NDIM>(other.index_, other.n_, item);
    if (status != FlatBufferManager::Status::Success) {
#ifndef __CUDACC__
      throw std::runtime_error(::typeName(this) + " extend failed: getItem raised " +
                               ::toString(status));
#endif
    } else {
      FlatBufferManager this_m{flatbuffer_};
      if (assign) {
        status = this_m.setItem<NESTED_ARRAY_NDIM>(index_[0], item);
        if (status != FlatBufferManager::Status::Success) {
#ifndef __CUDACC__
          throw std::runtime_error(::typeName(this) + " extend failed: setItem raised " +
                                   ::toString(status));
#endif
        }
      } else {
        status = this_m.concatItem<NESTED_ARRAY_NDIM>(index_[0], item);
        if (status != FlatBufferManager::Status::Success) {
#ifndef __CUDACC__
          throw std::runtime_error(::typeName(this) +
                                   " extend failed: concatItem raised " +
                                   ::toString(status));
#endif
        }
      }
    }
  }

  // copy other into self, can be called exactly once
  NestedArray<ItemType>& operator=(const NestedArray<ItemType>& other) {
    extend(other, /*assign=*/true);
    return *this;
  }

  // extend self with other, can be called multiple times provided
  // that self is the last specified item in its container
  NestedArray<ItemType>& operator+=(const NestedArray<ItemType>& other) {
    extend(other, /*assign=*/false);
    return *this;
  }

  const SQLTypeInfoLite* getTypeInfo() const {
    FlatBufferManager m{flatbuffer_};
    const auto* ti = reinterpret_cast<const SQLTypeInfoLite*>(m.get_user_data_buffer());
    if (ti == nullptr) {
#ifndef __CUDACC__
      throw std::runtime_error(::typeName(this) +
                               " getTypeInfo failed: user data buffer is null");
#endif
    }
    return ti;
  }

#ifdef HAVE_TOSTRING
  std::string toString() const {
    std::string result = ::typeName(this) + "(..., {";
    for (size_t i = 0; i < n_; i++) {
      result += std::to_string(index_[i]) + ", ";
    }
    result += "}, " + std::to_string(n_) + ")";
    return result;
  }
#endif
};

struct TextEncodingNone : NestedArray<char> {
  using NestedArray<char>::operator=;
  using NestedArray<char>::operator+=;

  char& operator[](const int64_t index) { return getValue(index); }

  DEVICE TextEncodingNone& operator=(const ::TextEncodingNone& other) {
    extend(reinterpret_cast<const int8_t*>(other.data()),
           static_cast<int32_t>(other.size()),
           true);
    return *this;
  }

  DEVICE TextEncodingNone& operator+=(const ::TextEncodingNone& other) {
    extend(reinterpret_cast<const int8_t*>(other.data()),
           static_cast<int32_t>(other.size()),
           false);
    return *this;
  }

#ifndef __CUDACC__
  // std::string supported API

  std::string str() const {
    char* values;
    int64_t nof_values;
    bool is_null;
    getValuesBuffer(values, nof_values, is_null);
    if (is_null) {
      return {};
    }
    std::string result;
    result.assign(reinterpret_cast<const char*>(values), static_cast<size_t>(nof_values));
    return result;
  }

  DEVICE TextEncodingNone& operator=(const std::string& s) {
    extend(
        reinterpret_cast<const int8_t*>(s.data()), static_cast<int32_t>(s.size()), true);
    return *this;
  }

  DEVICE TextEncodingNone& operator+=(const std::string& s) {
    extend(
        reinterpret_cast<const int8_t*>(s.data()), static_cast<int32_t>(s.size()), false);
    return *this;
  }

  std::string getString() const {
    return str();
  }
#endif
};

template <typename T>  // T is scalar type
struct Array : public NestedArray<T> {
  using NestedArray<T>::size;
  using NestedArray<T>::getItem;
  using NestedArray<T>::getValue;
  using NestedArray<T>::extend;

  T& operator[](const int64_t index) { return getValue(index); }

  DEVICE Array<T>& operator=(const ::Array<T>& s) {
    extend(
        reinterpret_cast<const int8_t*>(s.data()), static_cast<int32_t>(s.size()), true);
    return *this;
  }

  DEVICE Array<T>& operator+=(const ::Array<T>& s) {
    extend(
        reinterpret_cast<const int8_t*>(s.data()), static_cast<int32_t>(s.size()), false);
    return *this;
  }

  // For BC:
  DEVICE int64_t getSize() const { return size(); }
  DEVICE T operator()(const unsigned int index) const {
    return getItem(static_cast<int64_t>(index));
  }
  DEVICE T operator()(const int64_t index) const { return getItem(index); }
};

};  // namespace flatbuffer

namespace Geo {

struct Point2D {
  double x{std::numeric_limits<double>::quiet_NaN()};
  double y{std::numeric_limits<double>::quiet_NaN()};

#ifdef HAVE_TOSTRING
  std::string toString() const {
    return ::typeName(this) + "(x=" + ::toString(x) + ", y=" + std::to_string(y) + ")";
  }
#endif
};

DEVICE inline int32_t compress_x_coord(const double* x, const int64_t index) {
  return static_cast<int32_t>(Geospatial::compress_longitude_coord_geoint32(x[index]));
}

DEVICE inline int32_t compress_y_coord(const double* x, const int64_t index) {
  return static_cast<int32_t>(Geospatial::compress_latitude_coord_geoint32(x[index + 1]));
}

DEVICE inline double decompress_x_coord(const int8_t* data,
                                        const int64_t index,
                                        const bool is_geoint) {
  if (is_geoint) {
    return Geospatial::decompress_longitude_coord_geoint32(
        reinterpret_cast<const int32_t*>(data)[index]);
  } else {
    return reinterpret_cast<const double*>(data)[index];
  }
}

DEVICE inline double decompress_y_coord(const int8_t* data,
                                        const int64_t index,
                                        const bool is_geoint) {
  if (is_geoint) {
    return Geospatial::decompress_latitude_coord_geoint32(
        reinterpret_cast<const int32_t*>(data)[index + 1]);
  } else {
    return reinterpret_cast<const double*>(data)[index + 1];
  }
}

DEVICE inline double decompress_x_coord(const int8_t* data,
                                        const int64_t index,
                                        const bool is_geoint,
                                        const int32_t input_srid,
                                        const int32_t output_srid) {
  double x = decompress_x_coord(data, index, is_geoint);
  if (input_srid == output_srid || output_srid == 0) {
    return x;
  } else if (input_srid == 4326 && output_srid == 900913) {
    // WGS 84 --> Web Mercator
    x *= 111319.490778;
  } else {
#ifndef __CUDACC__
    throw std::runtime_error("decompress_x_coord: unhandled geo transformation from " +
                             std::to_string(input_srid) + " to " +
                             std::to_string(output_srid) + '.');
#endif
  }
  return x;
}

DEVICE inline double decompress_y_coord(const int8_t* data,
                                        const int64_t index,
                                        const bool is_geoint,
                                        const int32_t input_srid,
                                        const int32_t output_srid) {
  double y = decompress_y_coord(data, index, is_geoint);
  if (input_srid == output_srid || output_srid == 0) {
    return y;
  } else if (input_srid == 4326 && output_srid == 900913) {
    // WGS 84 --> Web Mercator
    y = 6378136.99911 * log(tan(.00872664626 * y + .785398163397));
  } else {
#ifndef __CUDACC__
    throw std::runtime_error("decompress_y_coord: unhandled geo transformation from " +
                             std::to_string(input_srid) + " to " +
                             std::to_string(output_srid) + '.');
#endif
  }
  return y;
}

DEVICE inline Point2D get_point(const int8_t* data,
                                const int64_t index,
                                const int32_t input_srid,
                                const int32_t output_srid,
                                const bool is_geoint) {
  Point2D point{decompress_x_coord(data, index, is_geoint, input_srid, output_srid),
                decompress_y_coord(data, index, is_geoint, input_srid, output_srid)};
  return point;
}

#ifndef __CUDACC__

template <typename CT>
inline void points_to_vector(const int8_t* points_buf,
                             const int64_t nof_points,
                             const bool is_geoint,
                             std::vector<CT>& result) {
  result.reserve(2 * nof_points);
  if (is_geoint) {
    if constexpr (std::is_same<CT, double>::value) {
      for (int64_t i = 0; i < nof_points; i++) {
        result.push_back(decompress_x_coord(points_buf, 2 * i, is_geoint));
        result.push_back(decompress_y_coord(points_buf, 2 * i, is_geoint));
      }
    } else {
      const int32_t* buf = reinterpret_cast<const int32_t*>(points_buf);
      result.assign(buf, buf + 2 * nof_points);
    }
  } else {
    const double* buf = reinterpret_cast<const double*>(points_buf);
    if constexpr (std::is_same<CT, double>::value) {
      result.assign(buf, buf + 2 * nof_points);
    } else {
      for (int64_t i = 0; i < nof_points; i++) {
        result.push_back(compress_x_coord(buf, 2 * i));
        result.push_back(compress_y_coord(buf, 2 * i));
      }
    }
  }
}

inline std::vector<int32_t> compress_coords(const int8_t* data,
                                            const int64_t size,
                                            const bool is_geoint) {
  int64_t nofpoints = size / (is_geoint ? sizeof(int32_t) * 2 : sizeof(double) * 2);
  std::vector<int32_t> result;
  result.reserve(2 * nofpoints);
  if (is_geoint) {
    const int32_t* buf = reinterpret_cast<const int32_t*>(data);
    result.assign(buf, buf + 2 * nofpoints);
  } else {
    const double* buf = reinterpret_cast<const double*>(data);
    for (int64_t i = 0; i < nofpoints; i++) {
      result.push_back(compress_x_coord(buf, 2 * i));
      result.push_back(compress_y_coord(buf, 2 * i));
    }
  }
  return result;
}

inline std::vector<double> decompress_coords(const int8_t* data,
                                             const int64_t size,
                                             const bool is_geoint) {
  int64_t nofpoints = size / (is_geoint ? sizeof(int32_t) * 2 : sizeof(double) * 2);
  std::vector<double> result;
  result.reserve(2 * nofpoints);
  for (int64_t i = 0; i < nofpoints; i++) {
    result.push_back(decompress_x_coord(data, 2 * i, is_geoint));
    result.push_back(decompress_y_coord(data, 2 * i, is_geoint));
  }
  return result;
}

inline std::vector<int32_t> compress_coords(const std::vector<double>& coords) {
  std::vector<int32_t> result;
  const size_t nofpoints = coords.size() / 2;
  result.reserve(coords.size());
  const double* buf = coords.data();
  for (size_t i = 0; i < nofpoints; i++) {
    result.push_back(compress_x_coord(buf, 2 * i));
    result.push_back(compress_y_coord(buf, 2 * i));
  }
  return result;
}

inline std::vector<double> decompress_coords(const std::vector<int32_t>& coords) {
  std::vector<double> result;
  const size_t nofpoints = coords.size() / 2;
  result.reserve(coords.size());
  const int8_t* buf = reinterpret_cast<const int8_t*>(coords.data());
  for (size_t i = 0; i < nofpoints; i++) {
    result.push_back(decompress_x_coord(buf, 2 * i, true));
    result.push_back(decompress_y_coord(buf, 2 * i, true));
  }
  return result;
}

inline std::vector<std::vector<int32_t>> compress_coords(
    const std::vector<std::vector<double>>& coords) {
  std::vector<std::vector<int32_t>> result;
  result.reserve(coords.size());
  for (size_t i = 0; i < coords.size(); i++) {
    result.push_back(compress_coords(coords[i]));
  }
  return result;
}

inline std::vector<std::vector<double>> decompress_coords(
    const std::vector<std::vector<int32_t>>& coords) {
  std::vector<std::vector<double>> result;
  result.reserve(coords.size());
  for (size_t i = 0; i < coords.size(); i++) {
    result.push_back(decompress_coords(coords[i]));
  }
  return result;
}

inline std::vector<std::vector<std::vector<int32_t>>> compress_coords(
    const std::vector<std::vector<std::vector<double>>>& coords) {
  std::vector<std::vector<std::vector<int32_t>>> result;
  result.reserve(coords.size());
  for (size_t i = 0; i < coords.size(); i++) {
    result.push_back(compress_coords(coords[i]));
  }
  return result;
}

inline std::vector<std::vector<std::vector<double>>> decompress_coords(
    const std::vector<std::vector<std::vector<int32_t>>>& coords) {
  std::vector<std::vector<std::vector<double>>> result;
  result.reserve(coords.size());
  for (size_t i = 0; i < coords.size(); i++) {
    result.push_back(decompress_coords(coords[i]));
  }
  return result;
}

#endif

// to be deprecated
inline bool get_is_geoint(const int8_t* flatbuffer) {
  FlatBufferManager m{const_cast<int8_t*>(flatbuffer)};
  switch (m.format()) {
    case GeoPointFormatId:
      return m.getGeoPointMetadata()->is_geoint;
    default:
#ifndef __CUDACC__
      throw std::runtime_error("get_is_geoint: unexpected format " +
                               ::toString(m.format()));
#else
      return false;
#endif
  }
}

// to be deprecated
inline int32_t get_input_srid(const int8_t* flatbuffer) {
  FlatBufferManager m{const_cast<int8_t*>(flatbuffer)};
  switch (m.format()) {
    case GeoPointFormatId:
      return m.getGeoPointMetadata()->input_srid;
    default:
#ifndef __CUDACC__
      throw std::runtime_error("get_input_srid: unexpected format " +
                               ::toString(m.format()));
#else
      return -1;
#endif
  }
}

// to be deprecated
inline int32_t get_output_srid(const int8_t* flatbuffer) {
  FlatBufferManager m{const_cast<int8_t*>(flatbuffer)};
  switch (m.format()) {
    case GeoPointFormatId:
      return m.getGeoPointMetadata()->output_srid;
    default:
#ifndef __CUDACC__
      throw std::runtime_error("get_output_srid: unexpected format " +
                               ::toString(m.format()));
#else
      return -1;
#endif
  }
}

template <typename ItemType>
struct GeoNestedArray : public flatbuffer::NestedArray<ItemType> {
  using flatbuffer::NestedArray<ItemType>::flatbuffer_;
  using flatbuffer::NestedArray<ItemType>::index_;
  using flatbuffer::NestedArray<ItemType>::n_;
  using flatbuffer::NestedArray<ItemType>::size;
  using flatbuffer::NestedArray<ItemType>::getItem;
  using flatbuffer::NestedArray<ItemType>::getTypeInfo;
  using flatbuffer::NestedArray<ItemType>::getRawBuffer;

  DEVICE Point2D getPoint(const int64_t index) const {
    int8_t* values;
    int64_t nof_values;
    bool is_null;
    size_t value_size;
    getRawBuffer(values, nof_values, value_size, is_null);
    if (is_null) {
      return {};
    }
    const auto* ti = getTypeInfo();
    return get_point(
        values, 2 * index, ti->get_input_srid(), ti->get_output_srid(), ti->is_geoint());
  }

#ifndef __CUDACC__

  template <typename CT, typename VT>
  FlatBufferManager::Status toCoordsWorker(std::vector<VT>& result) const {
    if constexpr (std::is_same<CT, VT>::value) {
      int8_t* values;
      int64_t nof_values;
      bool is_null;
      size_t value_size;
      getRawBuffer(values, nof_values, value_size, is_null);
      const auto* ti = getTypeInfo();
      points_to_vector(values, nof_values, ti->is_geoint(), result);
      return FlatBufferManager::Status::Success;
    } else {
      auto sz = size();
      result.reserve(sz);
      for (size_t i = 0; i < sz; i++) {
        auto item = getItem(i);
        VT ritem;
        auto status = item.toCoords(ritem);
        if (status != FlatBufferManager::Status::Success) {
          return status;
        }
        result.push_back(ritem);
      }
      return FlatBufferManager::Status::Success;
    }
  }

  // Return coordinates as a vector of double or int32_t type
  template <typename CT>
  FlatBufferManager::Status toCoords(std::vector<CT>& result) const {
    return toCoordsWorker<CT, CT>(result);
  }

  template <typename CT>
  FlatBufferManager::Status toCoords(std::vector<std::vector<CT>>& result) const {
    return toCoordsWorker<CT, std::vector<CT>>(result);
  }

  template <typename CT>
  FlatBufferManager::Status toCoords(
      std::vector<std::vector<std::vector<CT>>>& result) const {
    return toCoordsWorker<CT, std::vector<std::vector<CT>>>(result);
  }

  template <typename CT, typename VT>
  FlatBufferManager::Status fromCoordsWorker(const VT& coords) {
    FlatBufferManager m{flatbuffer_};
    const auto* ti = getTypeInfo();
    if (ti == nullptr) {
      return FlatBufferManager::UserDataError;
    }
    if (n_ != 1) {
      throw std::runtime_error(
          "NestedArray fromCoords failed: expected single index but got " +
          ::toString(n_));
    }
    if (ti->is_geoint()) {
      if constexpr (std::is_same<CT, double>::value) {
        const auto ccoords = compress_coords(coords);
        return m.setItem(index_[0], ccoords);
      } else {
        return m.setItem(index_[0], coords);
      }
    } else {
      if constexpr (std::is_same<CT, double>::value) {
        return m.setItem(index_[0], coords);
      } else {
        const auto dcoords = decompress_coords(coords);
        return m.setItem(index_[0], dcoords);
      }
    }
  }

  // Create row from a nested vector of coordinates either in double or int32_t type
  template <typename CT>
  FlatBufferManager::Status fromCoords(const std::vector<CT>& coords) {
    return fromCoordsWorker<CT, std::vector<CT>>(coords);
  }

  template <typename CT>
  FlatBufferManager::Status fromCoords(const std::vector<std::vector<CT>>& coords) {
    return fromCoordsWorker<CT, std::vector<std::vector<CT>>>(coords);
  }

  template <typename CT>
  FlatBufferManager::Status fromCoords(
      const std::vector<std::vector<std::vector<CT>>>& coords) {
    return fromCoordsWorker<CT, std::vector<std::vector<std::vector<CT>>>>(coords);
  }

#endif
};

struct LineString : public GeoNestedArray<Point2D> {
  DEVICE inline Point2D operator[](const unsigned int index) const {
    return getPoint(static_cast<int64_t>(index));
  }

#ifndef __CUDACC__

  using GeoNestedArray<Point2D>::toCoords;

  template <typename CT>
  std::vector<CT> toCoords() const {
    std::vector<CT> result;
    auto status = toCoords(result);
    if (status != FlatBufferManager::Status::Success) {
#ifndef __CUDACC__
      throw std::runtime_error(::typeName(this) +
                               " toCoords failed: " + ::toString(status));
#endif
    }
    return result;
  }

  std::vector<double> toCoords() const {
    return toCoords<double>();
  }

#endif
};

struct MultiPoint : public GeoNestedArray<Point2D> {
  DEVICE inline Point2D operator[](const unsigned int index) const {
    return getPoint(static_cast<int64_t>(index));
  }

#ifndef __CUDACC__

  using GeoNestedArray<Point2D>::toCoords;

  template <typename CT>
  std::vector<CT> toCoords() const {
    std::vector<CT> result;
    auto status = toCoords(result);
    if (status != FlatBufferManager::Status::Success) {
#ifndef __CUDACC__
      throw std::runtime_error(::typeName(this) +
                               " toCoords failed: " + ::toString(status));
#endif
    }
    return result;
  }

  std::vector<double> toCoords() const {
    return toCoords<double>();
  }

#endif
};

struct MultiLineString : public GeoNestedArray<LineString> {
#ifndef __CUDACC__

  using GeoNestedArray<LineString>::toCoords;

  template <typename CT>
  std::vector<std::vector<CT>> toCoords() const {
    std::vector<std::vector<CT>> result;
    auto status = toCoords(result);
    if (status != FlatBufferManager::Status::Success) {
      throw std::runtime_error(::typeName(this) +
                               " toCoords failed: " + ::toString(status));
    }
    return result;
  }

  std::vector<std::vector<double>> toCoords() const {
    return toCoords<double>();
  }

#endif
};

struct Polygon : public GeoNestedArray<LineString> {
#ifndef __CUDACC__

  using GeoNestedArray<LineString>::toCoords;

  template <typename CT>
  std::vector<std::vector<CT>> toCoords() const {
    std::vector<std::vector<CT>> result;
    auto status = toCoords(result);
    if (status != FlatBufferManager::Status::Success) {
      throw std::runtime_error(::typeName(this) +
                               " toCoords failed: " + ::toString(status));
    }
    return result;
  }

  std::vector<std::vector<double>> toCoords() const {
    return toCoords<double>();
  }

#endif
};

struct MultiPolygon : public GeoNestedArray<Polygon> {
#ifndef __CUDACC__

  using GeoNestedArray<Polygon>::toCoords;

  template <typename CT>
  std::vector<std::vector<std::vector<CT>>> toCoords() const {
    std::vector<std::vector<std::vector<CT>>> result;
    auto status = toCoords(result);
    if (status != FlatBufferManager::Status::Success) {
      throw std::runtime_error(::typeName(this) +
                               " toCoords failed: " + ::toString(status));
    }
    return result;
  }

  std::vector<std::vector<std::vector<double>>> toCoords() const {
    return toCoords<double>();
  }

#endif
};

}  // namespace Geo

template <>
struct Column<GeoLineString> : public flatbuffer::Column<Geo::LineString, GeoLineString> {
};

template <>
struct Column<GeoMultiPoint> : public flatbuffer::Column<Geo::MultiPoint, GeoMultiPoint> {
};

template <>
struct Column<GeoPolygon> : public flatbuffer::Column<Geo::Polygon, GeoPolygon> {};

template <>
struct Column<GeoMultiLineString>
    : public flatbuffer::Column<Geo::MultiLineString, GeoMultiLineString> {};

template <>
struct Column<GeoMultiPolygon>
    : public flatbuffer::Column<Geo::MultiPolygon, GeoMultiPolygon> {};

template <>
struct Column<GeoPoint> {
  int8_t* flatbuffer_;  // contains a flat buffer of the storage, use FlatBufferManager
                        // to access it.
  int64_t num_rows_;    // total row count, the number of all varlen arrays

  DEVICE Geo::Point2D getItem(const int64_t index, const int32_t output_srid = 0) const {
    /*
      output_srid != 0 enables transformation from input_srid to the
      specified output_srid. If output_srid < 0, the column's
      output_srid will be used, otherwise, the user specified
      output_srid is used.
     */
    FlatBufferManager m{flatbuffer_};
    int8_t* ptr;
    int64_t size;
    bool is_null;
    auto status = m.getItemOld(index, size, ptr, is_null);
    if (status != FlatBufferManager::Status::Success) {
#ifndef __CUDACC__
      throw std::runtime_error("getItem failed: " + ::toString(status));
#endif
    }
    bool is_geoint = Geo::get_is_geoint(flatbuffer_);
    int32_t this_input_srid = Geo::get_input_srid(flatbuffer_);
    int32_t this_output_srid = Geo::get_output_srid(flatbuffer_);
    return Geo::get_point(ptr,
                          0,
                          this_input_srid,
                          (output_srid < 0 ? this_output_srid : output_srid),
                          is_geoint);
  }

  DEVICE inline Geo::Point2D operator[](const unsigned int index) const {
    /* Use getItem(index, output_srid) to enable user-specified
       transformation. */
    return getItem(static_cast<int64_t>(index), /*output_srid=*/0);
  }

  DEVICE int64_t size() const {
    return num_rows_;
  }

  DEVICE inline bool isNull(int64_t index) const {
    FlatBufferManager m{flatbuffer_};
    bool is_null = false;
    auto status = m.isNull(index, is_null);
#ifndef __CUDACC__
    if (status != FlatBufferManager::Status::Success) {
      throw std::runtime_error("isNull failed: " + ::toString(status));
    }
#endif
    return is_null;
  }

  DEVICE inline void setNull(int64_t index) {
    FlatBufferManager m{flatbuffer_};
    auto status = m.setNull(index);
#ifndef __CUDACC__
    if (status != FlatBufferManager::Status::Success) {
      throw std::runtime_error("setNull failed: " + ::toString(status));
    }
#endif
  }

  DEVICE inline void setItem(int64_t index, const Geo::Point2D& other) {
    FlatBufferManager m{flatbuffer_};
    const auto* metadata = m.getGeoPointMetadata();
    int8_t* dest = nullptr;
    int64_t sz = 2 * (metadata->is_geoint ? sizeof(int32_t) : sizeof(double));
    FlatBufferManager::Status status = m.setItemOld(index, nullptr, sz, &dest);
    if (status != FlatBufferManager::Status::Success) {
#ifndef __CUDACC__
      throw std::runtime_error("setItem failed: " + ::toString(status));
#endif
    }
    if (dest == nullptr) {
#ifndef __CUDACC__
      throw std::runtime_error("setItem failed: dest is not set?!");
#endif
    }
    if (metadata->is_geoint) {
      // TODO: check other.x/y ranges
      int32_t* ptr = reinterpret_cast<int32_t*>(dest);
      ptr[0] = Geospatial::compress_longitude_coord_geoint32(other.x);
      ptr[1] = Geospatial::compress_latitude_coord_geoint32(other.y);
    } else {
      double* ptr = reinterpret_cast<double*>(dest);
      ptr[0] = other.x;
      ptr[1] = other.y;
    }
  }

#ifdef HAVE_FLATBUFFER_TOSTRING
  std::string toString() const {
    FlatBufferManager m{flatbuffer_};
    return ::typeName(this) + "(" + m.toString() +
           ", num_rows=" + std::to_string(num_rows_) + ")";
  }
#endif
};

template <>
struct Column<TextEncodingNone>
    : public flatbuffer::Column<flatbuffer::TextEncodingNone, TextEncodingNone> {};

template <typename T>
struct Column<Array<T>> : public flatbuffer::Column<flatbuffer::Array<T>, Array<T>> {
  Column(int8_t* flatbuffer, int64_t num_rows)
      : flatbuffer::Column<flatbuffer::Array<T>, Array<T>>(flatbuffer, num_rows) {}
};

template <>
struct Column<Array<TextEncodingDict>>
    : public flatbuffer::Column<flatbuffer::Array<TextEncodingDict>,
                                Array<TextEncodingDict>> {
  Column(int8_t* flatbuffer, int64_t num_rows)
      : flatbuffer::Column<flatbuffer::Array<TextEncodingDict>, Array<TextEncodingDict>>(
            flatbuffer,
            num_rows) {}

  DEVICE inline int32_t getDictDbId() const {
    const auto* ti = getTypeInfo();
    return (ti ? ti->db_id : 0);
  }

  DEVICE inline int32_t getDictId() const {
    const auto* ti = getTypeInfo();
    return (ti ? ti->dict_id : 0);
  }
};

template <>
struct Column<TextEncodingDict> {
  TextEncodingDict* ptr_;  // row data
  int64_t num_rows_;       // row count
#ifndef __CUDACC__
#ifndef UDF_COMPILED
  StringDictionaryProxy* string_dict_proxy_;
  DEVICE Column(const Column& other)
      : ptr_(other.ptr_)
      , num_rows_(other.num_rows_)
      , string_dict_proxy_(other.string_dict_proxy_) {}
  DEVICE Column(TextEncodingDict* ptr,
                const int64_t num_rows,
                StringDictionaryProxy* string_dict_proxy)
      : ptr_(ptr), num_rows_(num_rows), string_dict_proxy_(string_dict_proxy) {}
  DEVICE Column(std::vector<TextEncodingDict>& input_vec)
      : ptr_(input_vec.data())
      , num_rows_(static_cast<int64_t>(input_vec.size()))
      , string_dict_proxy_(nullptr) {}
#else
  DEVICE Column(TextEncodingDict* ptr, const int64_t num_rows)
      : ptr_(ptr), num_rows_(num_rows) {}
#endif  // #ifndef UDF_COMPILED
#else
  DEVICE Column(TextEncodingDict* ptr, const int64_t num_rows)
      : ptr_(ptr), num_rows_(num_rows) {}
#endif  // #ifndef __CUDACC__

  DEVICE TextEncodingDict& operator[](const unsigned int index) const {
    if (index >= num_rows_) {
#ifndef __CUDACC__
      throw std::runtime_error("column buffer index is out of range");
#else
      static DEVICE TextEncodingDict null_value;
      set_null(null_value.value);
      return null_value;
#endif
    }
    return ptr_[index];
  }
  DEVICE inline TextEncodingDict* getPtr() const {
    return ptr_;
  }
  DEVICE inline int64_t size() const {
    return num_rows_;
  }
  DEVICE inline void setSize(int64_t num_rows) {
    num_rows_ = num_rows;
  }

  DEVICE inline bool isNull(int64_t index) const {
    return is_null(ptr_[index].value);
  }

  DEVICE inline void setNull(int64_t index) {
    set_null(ptr_[index].value);
  }

#ifndef __CUDACC__
#ifndef UDF_COMPILED
  DEVICE inline int32_t getDictDbId() const {
    return string_dict_proxy_->getDictKey().db_id;
  }
  DEVICE inline int32_t getDictId() const {
    return string_dict_proxy_->getDictKey().dict_id;
  }
  DEVICE inline const std::string getString(int64_t index) const {
    return isNull(index) ? "" : string_dict_proxy_->getString(ptr_[index].value);
  }
  DEVICE inline const TextEncodingDict getOrAddTransient(const std::string& str) {
    return string_dict_proxy_->getOrAddTransient(str);
  }
#endif  // #ifndef UDF_COMPILED
#endif  // #ifndef __CUDACC__

  DEVICE Column<TextEncodingDict>& operator=(const Column<TextEncodingDict>& other) {
#ifndef __CUDACC__
    if (size() == other.size()) {
      memcpy(ptr_, other.ptr_, other.size() * sizeof(TextEncodingDict));
    } else {
      throw std::runtime_error("cannot copy assign columns with different sizes");
    }
#else
    if (size() == other.size()) {
      for (unsigned int i = 0; i < size(); i++) {
        ptr_[i] = other[i];
      }
    } else {
      // TODO: set error
    }
#endif
    return *this;
  }

#ifdef HAVE_TOSTRING
  std::string toString() const {
    return ::typeName(this) + "(ptr=" + ::toString(reinterpret_cast<void*>(ptr_)) +
           ", num_rows=" + std::to_string(num_rows_) + ")";
  }
#endif
};

template <>
DEVICE inline bool Column<Timestamp>::isNull(int64_t index) const {
  return is_null(ptr_[index].time);
}

template <>
DEVICE inline void Column<Timestamp>::setNull(int64_t index) {
  set_null(ptr_[index].time);
}

template <>
CONSTEXPR DEVICE inline void set_null<Timestamp>(Timestamp& t) {
  set_null(t.time);
}

/*
  ColumnList is an ordered list of Columns.
*/
template <typename T>
struct ColumnList {
  int8_t** ptrs_;     // ptrs to columns data
  int64_t num_cols_;  // the length of columns list
  int64_t num_rows_;  // the number of rows of columns

  DEVICE ColumnList(int8_t** ptrs, const int64_t num_cols, const int64_t num_rows)
      : ptrs_(ptrs), num_cols_(num_cols), num_rows_(num_rows) {}

  DEVICE int64_t size() const { return num_rows_; }
  DEVICE int64_t numCols() const { return num_cols_; }
  DEVICE Column<T> operator[](const int index) const {
    if (index >= 0 && index < num_cols_)
      return {reinterpret_cast<T*>(ptrs_[index]), num_rows_};
    else
      return {nullptr, -1};
  }

#ifdef HAVE_TOSTRING

  std::string toString() const {
    std::string result = ::typeName(this) + "(ptrs=[";
    for (int64_t index = 0; index < num_cols_; index++) {
      result += ::toString(reinterpret_cast<void*>(ptrs_[index])) +
                (index < num_cols_ - 1 ? ", " : "");
    }
    result += "], num_cols=" + std::to_string(num_cols_) +
              ", num_rows=" + std::to_string(num_rows_) + ")";
    return result;
  }

#endif
};

template <typename T>
struct ColumnList<Array<T>> {
  int8_t** ptrs_;     // ptrs to columns data in FlatBuffer format
  int64_t num_cols_;  // the length of columns list
  int64_t num_rows_;  // the size of columns

  DEVICE int64_t size() const { return num_rows_; }
  DEVICE int64_t numCols() const { return num_cols_; }
  DEVICE Column<Array<T>> operator[](const int index) const {
    int8_t* ptr = ((index >= 0 && index < num_cols_) ? ptrs_[index] : nullptr);
    int64_t num_rows = ((index >= 0 && index < num_cols_) ? num_rows_ : -1);
    Column<Array<T>> result(ptr, num_rows);
    return result;
  }

#ifdef HAVE_TOSTRING

  std::string toString() const {
    std::string result = ::typeName(this) + "(ptrs=[";
    for (int64_t index = 0; index < num_cols_; index++) {
      result += ::toString(reinterpret_cast<void*>(ptrs_[index])) +
                (index < num_cols_ - 1 ? ", " : "");
    }
    result += "], num_cols=" + std::to_string(num_cols_) +
              ", num_rows=" + std::to_string(num_rows_) + ")";
    return result;
  }

#endif
};

template <>
struct ColumnList<TextEncodingDict> {
  int8_t** ptrs_;     // ptrs to columns data
  int64_t num_cols_;  // the length of columns list
  int64_t num_rows_;  // the size of columns
#ifndef __CUDACC__
#ifndef UDF_COMPILED
  StringDictionaryProxy** string_dict_proxies_;  // the size of columns
  DEVICE ColumnList(int8_t** ptrs,
                    const int64_t num_cols,
                    const int64_t num_rows,
                    StringDictionaryProxy** string_dict_proxies)
      : ptrs_(ptrs)
      , num_cols_(num_cols)
      , num_rows_(num_rows)
      , string_dict_proxies_(string_dict_proxies) {}
#else
  DEVICE ColumnList(int8_t** ptrs, const int64_t num_cols, const int64_t num_rows)
      : ptrs_(ptrs), num_cols_(num_cols), num_rows_(num_rows) {}
#endif  // #ifndef UDF_COMPILED
#else
  DEVICE ColumnList(int8_t** ptrs, const int64_t num_cols, const int64_t num_rows)
      : ptrs_(ptrs), num_cols_(num_cols), num_rows_(num_rows) {}
#endif  // #ifndef __CUDACC__

  DEVICE int64_t size() const {
    return num_rows_;
  }
  DEVICE int64_t numCols() const {
    return num_cols_;
  }
  DEVICE Column<TextEncodingDict> operator[](const int index) const {
    if (index >= 0 && index < num_cols_) {
      Column<TextEncodingDict> result(reinterpret_cast<TextEncodingDict*>(ptrs_[index]),
                                      num_rows_
#ifndef __CUDACC__
#ifndef UDF_COMPILED
                                      ,
                                      string_dict_proxies_[index]
#endif  // #ifndef UDF_COMPILED
#endif  // #ifndef __CUDACC__
      );
      return result;
    } else {
      Column<TextEncodingDict> result(nullptr,
                                      -1
#ifndef __CUDACC__
#ifndef UDF_COMPILED
                                      ,
                                      nullptr
#endif  // #ifndef UDF_COMPILED
#endif  // #ifndef__CUDACC__
      );
      return result;
    }
  }

#ifdef HAVE_TOSTRING

  std::string toString() const {
    std::string result = ::typeName(this) + "(ptrs=[";
    for (int64_t index = 0; index < num_cols_; index++) {
      result += ::toString(reinterpret_cast<void*>(ptrs_[index])) +
                (index < num_cols_ - 1 ? ", " : "");
    }
    result += "], num_cols=" + std::to_string(num_cols_) +
              ", num_rows=" + std::to_string(num_rows_) + ")";
    return result;
  }

#endif
};

#endif  // #ifndef UDF_COMPILED
