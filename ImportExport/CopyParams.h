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

/*
 * @file CopyParams.h
 * @brief CopyParams struct
 *
 */

#pragma once

#include <optional>
#include <string>

#include "ImportExport/SourceType.h"
#include "Shared/S3Config.h"
#include "Shared/sqltypes.h"

namespace import_export {

// not too big (need much memory) but not too small (many thread forks)
constexpr static size_t kImportFileBufferSize = (1 << 23);

// import buffers may grow to this size if necessary
constexpr static size_t max_import_buffer_resize_byte_size = 1024 * 1024 * 1024;

enum class ImportHeaderRow { kAutoDetect, kNoHeader, kHasHeader };
enum class RasterPointType { kNone, kAuto, kSmallInt, kInt, kFloat, kDouble, kPoint };
enum class RasterPointTransform { kNone, kAuto, kFile, kWorld };

inline std::string to_string(const RasterPointTransform& pt) {
  switch (pt) {
    case RasterPointTransform::kNone:
      return "NONE";
    case RasterPointTransform::kAuto:
      return "AUTO";
    case RasterPointTransform::kFile:
      return "FILE";
    case RasterPointTransform::kWorld:
      return "WORLD";
    default:
      UNREACHABLE();
  }
  return "";
}

struct CopyParams {
  char delimiter;
  std::string null_str;
  ImportHeaderRow has_header;
  bool quoted;  // does the input have any quoted fields, default to false
  char quote;
  char escape;
  char line_delim;
  char array_delim;
  char array_begin;
  char array_end;
  int threads;
  size_t
      max_reject;  // maximum number of records that can be rejected before copy is failed
  import_export::SourceType source_type;
  bool plain_text = false;
  bool trim_spaces;
  shared::S3Config s3_config;
  // kafka related params
  size_t retry_count;
  size_t retry_wait;
  size_t batch_size;
  size_t buffer_size;
  size_t max_import_batch_row_count;
  // geospatial params
  bool lonlat;
  EncodingType geo_coords_encoding;
  int32_t geo_coords_comp_param;
  SQLTypes geo_coords_type;
  int32_t geo_coords_srid;
  bool sanitize_column_names;
  std::string geo_layer_name;
  bool geo_explode_collections;
  bool geo_validate_geometry;
  int32_t source_srid;
  std::optional<std::string> regex_path_filter;
  std::optional<std::string> file_sort_order_by;
  std::optional<std::string> file_sort_regex;
  RasterPointType raster_point_type;
  std::string raster_import_bands;
  int32_t raster_scanlines_per_thread;
  RasterPointTransform raster_point_transform;
  std::string raster_import_dimensions;
  std::optional<int> raster_width{};
  std::optional<int> raster_height{};
  std::string add_metadata_columns;
  bool raster_drop_if_all_null;
  // odbc parameters
  std::string sql_select;
  std::string sql_order_by;
  // odbc user mapping parameters
  std::string username;
  std::string password;
  std::string credential_string;
  // odbc server parameters
  std::string dsn;
  std::string connection_string;
  // regex parameters
  std::string line_start_regex;
  std::string line_regex;
  // geo/raster bounding box filter
  std::string bounding_box_clip;

  CopyParams()
      : delimiter(',')
      , null_str("\\N")
      , has_header(ImportHeaderRow::kAutoDetect)
      , quoted(true)
      , quote('"')
      , escape('"')
      , line_delim('\n')
      , array_delim(',')
      , array_begin('{')
      , array_end('}')
      , threads(0)
      , max_reject(100000)
      , source_type(import_export::SourceType::kDelimitedFile)
      , trim_spaces(true)
      , retry_count(100)
      , retry_wait(5)
      , batch_size(1000)
      , buffer_size(kImportFileBufferSize)
      , max_import_batch_row_count(0)
      , lonlat(true)
      , geo_coords_encoding(kENCODING_GEOINT)
      , geo_coords_comp_param(32)
      , geo_coords_type(kGEOMETRY)
      , geo_coords_srid(4326)
      , sanitize_column_names(true)
      , geo_explode_collections(false)
      , geo_validate_geometry{false}
      , source_srid(0)
      , raster_point_type(RasterPointType::kAuto)
      , raster_scanlines_per_thread(32)
      , raster_point_transform(RasterPointTransform::kAuto)
      , raster_drop_if_all_null{false} {}

  CopyParams(char d, const std::string& n, char l, size_t b, size_t retries, size_t wait)
      : delimiter(d)
      , null_str(n)
      , has_header(ImportHeaderRow::kAutoDetect)
      , quoted(true)
      , quote('"')
      , escape('"')
      , line_delim(l)
      , array_delim(',')
      , array_begin('{')
      , array_end('}')
      , threads(0)
      , max_reject(100000)
      , source_type(import_export::SourceType::kDelimitedFile)
      , trim_spaces(true)
      , retry_count(retries)
      , retry_wait(wait)
      , batch_size(b)
      , buffer_size(kImportFileBufferSize)
      , max_import_batch_row_count(0)
      , lonlat(true)
      , geo_coords_encoding(kENCODING_GEOINT)
      , geo_coords_comp_param(32)
      , geo_coords_type(kGEOMETRY)
      , geo_coords_srid(4326)
      , sanitize_column_names(true)
      , geo_explode_collections(false)
      , geo_validate_geometry{false}
      , source_srid(0)
      , raster_point_type(RasterPointType::kAuto)
      , raster_scanlines_per_thread(32)
      , raster_point_transform(RasterPointTransform::kAuto)
      , raster_drop_if_all_null{false} {}
};

}  // namespace import_export
