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

#include "DataMgr/Chunk/Chunk.h"
#include "Geospatial/Compression.h"
#include "Geospatial/Types.h"

#include "DataMgr/ArrayNoneEncoder.h"
#include "DataMgr/FixedLengthArrayNoneEncoder.h"
#include "DataMgr/ForeignStorage/ForeignStorageException.h"
#include "DataMgr/StringNoneEncoder.h"
#include "ImportExport/Importer.h"

namespace foreign_storage {

template <typename T>
inline ArrayDatum encode_as_array_datum(const std::vector<T>& data) {
  const size_t num_bytes = data.size() * sizeof(T);
  std::shared_ptr<int8_t> buffer(new int8_t[num_bytes], std::default_delete<int8_t[]>());
  memcpy(buffer.get(), data.data(), num_bytes);
  return ArrayDatum(num_bytes, buffer, false);
}

class GeospatialEncoder {
 public:
  virtual ~GeospatialEncoder() = default;

  GeospatialEncoder(const bool geo_validate_geometry)
      : geo_validate_geometry_{geo_validate_geometry} {}

  GeospatialEncoder(std::list<Chunk_NS::Chunk>& chunks, const bool geo_validate_geometry)
      : geo_column_descriptor_(chunks.begin()->getColumnDesc())
      , base_column_encoder_(nullptr)
      , coords_column_encoder_(nullptr)
      , bounds_column_encoder_(nullptr)
      , ring_or_line_sizes_column_encoder_(nullptr)
      , poly_rings_column_encoder_(nullptr)
      , base_column_metadata_(nullptr)
      , coords_column_metadata_(nullptr)
      , bounds_column_metadata_(nullptr)
      , ring_or_line_sizes_column_metadata_(nullptr)
      , poly_rings_column_metadata_(nullptr)
      , geo_validate_geometry_{geo_validate_geometry} {
    CHECK(geo_column_descriptor_->columnType.is_geometry());
    validateChunksSizing(chunks);
    const auto geo_column_type = geo_column_descriptor_->columnType.get_type();

    // initialize coords column
    coords_column_descriptor_ = getColumnDescriptor(chunks, geo_column_type, COORDS);

    // initialize bounds column
    if (hasBoundsColumn()) {
      bounds_column_descriptor_ = getColumnDescriptor(chunks, geo_column_type, BOUNDS);
    }

    // initialize ring sizes column & render group column
    if (hasRingOrLineSizesColumn()) {
      ring_or_line_sizes_column_descriptor_ =
          getColumnDescriptor(chunks, geo_column_type, RING_OR_LINE_SIZES);
    }

    // initialize poly rings column
    if (hasPolyRingsColumn()) {
      poly_rings_column_descriptor_ =
          getColumnDescriptor(chunks, geo_column_type, POLY_RINGS);
    }
  }

  GeospatialEncoder(std::list<Chunk_NS::Chunk>& chunks,
                    std::list<std::unique_ptr<ChunkMetadata>>& chunk_metadata,
                    const bool geo_validate_geometry)
      : geo_column_descriptor_(chunks.begin()->getColumnDesc())
      , base_column_encoder_(nullptr)
      , coords_column_encoder_(nullptr)
      , bounds_column_encoder_(nullptr)
      , ring_or_line_sizes_column_encoder_(nullptr)
      , poly_rings_column_encoder_(nullptr)
      , base_column_metadata_(nullptr)
      , coords_column_metadata_(nullptr)
      , bounds_column_metadata_(nullptr)
      , ring_or_line_sizes_column_metadata_(nullptr)
      , poly_rings_column_metadata_(nullptr)
      , geo_validate_geometry_{geo_validate_geometry} {
    CHECK(geo_column_descriptor_->columnType.is_geometry());

    validateChunksSizing(chunks);
    validateMetadataSizing(chunk_metadata);

    const auto geo_column_type = geo_column_descriptor_->columnType.get_type();

    // initialize base column encoder
    auto base_chunk = chunks.begin();
    base_chunk->initEncoder();
    base_column_encoder_ =
        dynamic_cast<StringNoneEncoder*>(base_chunk->getBuffer()->getEncoder());
    base_column_metadata_ = chunk_metadata.begin()->get();
    CHECK(base_column_encoder_);

    // initialize coords column
    std::tie(coords_column_encoder_, coords_column_metadata_, coords_column_descriptor_) =
        initEncoderAndGetEncoderAndMetadata(
            chunks, chunk_metadata, geo_column_type, COORDS);

    // initialize bounds column
    if (hasBoundsColumn()) {
      std::tie(
          bounds_column_encoder_, bounds_column_metadata_, bounds_column_descriptor_) =
          initEncoderAndGetEncoderAndMetadata(
              chunks, chunk_metadata, geo_column_type, BOUNDS);
    }

    // initialize ring sizes column & render group column
    if (hasRingOrLineSizesColumn()) {
      std::tie(ring_or_line_sizes_column_encoder_,
               ring_or_line_sizes_column_metadata_,
               ring_or_line_sizes_column_descriptor_) =
          initEncoderAndGetEncoderAndMetadata(
              chunks, chunk_metadata, geo_column_type, RING_OR_LINE_SIZES);
    }

    // initialize poly rings column
    if (hasPolyRingsColumn()) {
      std::tie(poly_rings_column_encoder_,
               poly_rings_column_metadata_,
               poly_rings_column_descriptor_) =
          initEncoderAndGetEncoderAndMetadata(
              chunks, chunk_metadata, geo_column_type, POLY_RINGS);
    }
  }

 protected:
  void appendBaseDataAndUpdateMetadata(const int64_t row_count) {
    base_values_.resize(row_count);
    *base_column_metadata_ =
        *base_column_encoder_->appendData(&base_values_, 0, row_count);
  }

  void validateChunksSizing(const std::list<Chunk_NS::Chunk>& chunks) const {
    size_t expected_size = geo_column_descriptor_->columnType.get_physical_cols() + 1;
    CHECK_EQ(chunks.size(), expected_size);
  }

  void validateMetadataSizing(
      const std::list<std::unique_ptr<ChunkMetadata>>& chunk_metadata) const {
    size_t expected_size = geo_column_descriptor_->columnType.get_physical_cols() + 1;
    CHECK_EQ(chunk_metadata.size(), expected_size);
  }

  void appendArrayDatumsToBufferAndUpdateMetadata() {
    appendToArrayEncoderAndUpdateMetadata(
        coords_datum_buffer_, coords_column_encoder_, coords_column_metadata_);
    appendToArrayEncoderAndUpdateMetadata(
        bounds_datum_buffer_, bounds_column_encoder_, bounds_column_metadata_);
    appendToArrayEncoderAndUpdateMetadata(ring_or_line_sizes_datum_buffer_,
                                          ring_or_line_sizes_column_encoder_,
                                          ring_or_line_sizes_column_metadata_);
    appendToArrayEncoderAndUpdateMetadata(poly_rings_datum_buffer_,
                                          poly_rings_column_encoder_,
                                          poly_rings_column_metadata_);
  }

  void appendToArrayEncoderAndUpdateMetadata(
      const std::vector<ArrayDatum>& datum_parse_buffer,
      Encoder* encoder,
      ChunkMetadata* chunk_metadata) const {
    if (!encoder) {
      CHECK(!chunk_metadata);
      return;
    }
    if (auto fixed_len_array_encoder =
            dynamic_cast<FixedLengthArrayNoneEncoder*>(encoder)) {
      auto new_chunk_metadata = fixed_len_array_encoder->appendData(
          &datum_parse_buffer, 0, datum_parse_buffer.size());
      *chunk_metadata = *new_chunk_metadata;
    } else if (auto array_encoder = dynamic_cast<ArrayNoneEncoder*>(encoder)) {
      auto new_chunk_metadata = array_encoder->appendData(
          &datum_parse_buffer, 0, datum_parse_buffer.size(), false);
      *chunk_metadata = *new_chunk_metadata;
    } else {
      UNREACHABLE();
    }
  }

  void processGeoElement(std::string_view geo_string_view) {
    SQLTypeInfo import_ti{geo_column_descriptor_->columnType};
    if (!Geospatial::GeoTypesFactory::getGeoColumns(std::string(geo_string_view),
                                                    import_ti,
                                                    coords_parse_buffer_,
                                                    bounds_parse_buffer_,
                                                    ring_or_line_sizes_parse_buffer_,
                                                    poly_rings_parse_buffer_,
                                                    geo_validate_geometry_)) {
      throwMalformedGeoElement(geo_column_descriptor_->columnName);
    }

    // validate types

    if (!geo_promoted_type_match(import_ti.get_type(),
                                 geo_column_descriptor_->columnType.get_type())) {
      throwMismatchedGeoElement(geo_column_descriptor_->columnName);
    }

    // append coords
    std::vector<uint8_t> compressed_coords = Geospatial::compress_coords(
        coords_parse_buffer_, geo_column_descriptor_->columnType);
    coords_datum_buffer_.emplace_back(encode_as_array_datum(compressed_coords));

    // append bounds
    if (hasBoundsColumn()) {
      bounds_datum_buffer_.emplace_back(encode_as_array_datum(bounds_parse_buffer_));
    }

    // append ring sizes
    if (hasRingOrLineSizesColumn()) {
      ring_or_line_sizes_datum_buffer_.emplace_back(
          encode_as_array_datum(ring_or_line_sizes_parse_buffer_));
    }

    // append poly rings
    if (hasPolyRingsColumn()) {
      poly_rings_datum_buffer_.emplace_back(
          encode_as_array_datum(poly_rings_parse_buffer_));
    }
  }

  void processNullGeoElement() {
    SQLTypeInfo import_ti{geo_column_descriptor_->columnType};
    Geospatial::GeoTypesFactory::getNullGeoColumns(import_ti,
                                                   coords_parse_buffer_,
                                                   bounds_parse_buffer_,
                                                   ring_or_line_sizes_parse_buffer_,
                                                   poly_rings_parse_buffer_);
    // POINT columns are represented using fixed length arrays and need
    // special treatment of nulls
    if (geo_column_descriptor_->columnType.get_type() == kPOINT) {
      std::vector<uint8_t> compressed_coords = Geospatial::compress_coords(
          coords_parse_buffer_, geo_column_descriptor_->columnType);
      coords_datum_buffer_.emplace_back(encode_as_array_datum(compressed_coords));
    } else {
      coords_datum_buffer_.emplace_back(import_export::ImporterUtils::composeNullArray(
          coords_column_descriptor_->columnType));
    }
    if (hasBoundsColumn()) {
      bounds_datum_buffer_.emplace_back(import_export::ImporterUtils::composeNullArray(
          bounds_column_descriptor_->columnType));
    }
    if (hasRingOrLineSizesColumn()) {
      ring_or_line_sizes_datum_buffer_.emplace_back(
          import_export::ImporterUtils::composeNullArray(
              ring_or_line_sizes_column_descriptor_->columnType));
    }
    if (hasPolyRingsColumn()) {
      poly_rings_datum_buffer_.emplace_back(
          import_export::ImporterUtils::composeNullArray(
              poly_rings_column_descriptor_->columnType));
    }
  }

  void clearParseBuffers() {
    coords_parse_buffer_.clear();
    bounds_parse_buffer_.clear();
    ring_or_line_sizes_parse_buffer_.clear();
    poly_rings_parse_buffer_.clear();
  }

  void clearDatumBuffers() {
    coords_datum_buffer_.clear();
    bounds_datum_buffer_.clear();
    ring_or_line_sizes_datum_buffer_.clear();
    poly_rings_datum_buffer_.clear();
  }

  enum GeoColumnType { COORDS, BOUNDS, RING_OR_LINE_SIZES, POLY_RINGS };

  template <typename T>
  typename std::list<T>::iterator getIteratorForGeoColumnType(
      std::list<T>& list,
      const SQLTypes column_type,
      const GeoColumnType geo_column) {
    auto list_iter = list.begin();
    list_iter++;  // skip base column
    switch (column_type) {
      case kPOINT: {
        if (geo_column == COORDS) {
          return list_iter;
        }
        UNREACHABLE();
      }
      case kMULTIPOINT:
      case kLINESTRING: {
        if (geo_column == COORDS) {
          return list_iter;
        }
        list_iter++;
        if (geo_column == BOUNDS) {
          return list_iter;
        }
        UNREACHABLE();
      }
      case kMULTILINESTRING: {
        if (geo_column == COORDS) {
          return list_iter;
        }
        list_iter++;
        if (geo_column == RING_OR_LINE_SIZES) {
          return list_iter;
        }
        list_iter++;
        if (geo_column == BOUNDS) {
          return list_iter;
        }
        UNREACHABLE();
      }
      case kPOLYGON: {
        if (geo_column == COORDS) {
          return list_iter;
        }
        list_iter++;
        if (geo_column == RING_OR_LINE_SIZES) {
          return list_iter;
        }
        list_iter++;
        if (geo_column == BOUNDS) {
          return list_iter;
        }
        UNREACHABLE();
      }
      case kMULTIPOLYGON: {
        if (geo_column == COORDS) {
          return list_iter;
        }
        list_iter++;
        if (geo_column == RING_OR_LINE_SIZES) {
          return list_iter;
        }
        list_iter++;
        if (geo_column == POLY_RINGS) {
          return list_iter;
        }
        list_iter++;
        if (geo_column == BOUNDS) {
          return list_iter;
        }
        UNREACHABLE();
      }
      default:
        UNREACHABLE();
    }
    return {};
  }

  std::tuple<Encoder*, ChunkMetadata*, const ColumnDescriptor*>
  initEncoderAndGetEncoderAndMetadata(
      std::list<Chunk_NS::Chunk>& chunks,
      std::list<std::unique_ptr<ChunkMetadata>>& chunk_metadata,
      const SQLTypes sql_type,
      GeoColumnType geo_column_type) {
    auto chunk = getIteratorForGeoColumnType(chunks, sql_type, geo_column_type);
    chunk->initEncoder();
    auto encoder = chunk->getBuffer()->getEncoder();
    auto metadata =
        getIteratorForGeoColumnType(chunk_metadata, sql_type, geo_column_type)->get();
    auto column_descriptor = chunk->getColumnDesc();
    return {encoder, metadata, column_descriptor};
  }

  const ColumnDescriptor* getColumnDescriptor(std::list<Chunk_NS::Chunk>& chunks,
                                              const SQLTypes sql_type,
                                              GeoColumnType geo_column_type) {
    auto chunk = getIteratorForGeoColumnType(chunks, sql_type, geo_column_type);
    auto column_descriptor = chunk->getColumnDesc();
    return column_descriptor;
  }

  static void throwMalformedGeoElement(const std::string& omnisci_column_name) {
    std::string error_message = "Failed to extract valid geometry in HeavyDB column '" +
                                omnisci_column_name + "'.";
    throw foreign_storage::ForeignStorageException(error_message);
  }

  static void throwMismatchedGeoElement(const std::string& omnisci_column_name) {
    throw foreign_storage::ForeignStorageException(
        "Imported geometry"
        " doesn't match the geospatial type of HeavyDB column '" +
        omnisci_column_name + "'.");
  }

  bool hasBoundsColumn() const {
    const auto geo_column_type = geo_column_descriptor_->columnType.get_type();
    return geo_column_type == kMULTIPOINT || geo_column_type == kLINESTRING ||
           geo_column_type == kMULTILINESTRING || geo_column_type == kPOLYGON ||
           geo_column_type == kMULTIPOLYGON;
  }

  bool hasRingOrLineSizesColumn() const {
    const auto geo_column_type = geo_column_descriptor_->columnType.get_type();
    return geo_column_type == kPOLYGON || geo_column_type == kMULTIPOLYGON ||
           geo_column_type == kMULTILINESTRING;
  }

  bool hasPolyRingsColumn() const {
    const auto geo_column_type = geo_column_descriptor_->columnType.get_type();
    return geo_column_type == kMULTIPOLYGON;
  }

  const ColumnDescriptor* geo_column_descriptor_;

  StringNoneEncoder* base_column_encoder_;
  Encoder* coords_column_encoder_;
  Encoder* bounds_column_encoder_;
  Encoder* ring_or_line_sizes_column_encoder_;
  Encoder* poly_rings_column_encoder_;

  ChunkMetadata* base_column_metadata_;
  ChunkMetadata* coords_column_metadata_;
  ChunkMetadata* bounds_column_metadata_;
  ChunkMetadata* ring_or_line_sizes_column_metadata_;
  ChunkMetadata* poly_rings_column_metadata_;

  const ColumnDescriptor* coords_column_descriptor_;
  const ColumnDescriptor* bounds_column_descriptor_;
  const ColumnDescriptor* ring_or_line_sizes_column_descriptor_;
  const ColumnDescriptor* poly_rings_column_descriptor_;

  std::vector<std::string> base_values_;

  // Used repeatedly in parsing geo types, declared as members to prevent
  // deallocation/reallocation costs
  std::vector<double> coords_parse_buffer_;
  std::vector<double> bounds_parse_buffer_;
  std::vector<int> ring_or_line_sizes_parse_buffer_;
  std::vector<int> poly_rings_parse_buffer_;

  // Used to buffer array appends in memory for a batch
  std::vector<ArrayDatum> coords_datum_buffer_;
  std::vector<ArrayDatum> bounds_datum_buffer_;
  std::vector<ArrayDatum> ring_or_line_sizes_datum_buffer_;
  std::vector<ArrayDatum> poly_rings_datum_buffer_;

  // CopyParams
  bool geo_validate_geometry_;
};

}  // namespace foreign_storage
