/*
 * Copyright 2023 HEAVY.AI, Inc.
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

#include <map>
#include <optional>
#include <vector>

#include <rapidjson/document.h>

#include "Logger/Logger.h"
#include "Shared/sqltypes.h"

// helper functions for serializing/deserializing objects to rapidjson value
namespace json_utils {
std::string get_type_as_string(const rapidjson::Value& object);

// Forward declare for vector/map functions
template <class T>
void add_value_to_object(rapidjson::Value& object,
                         const T& value,
                         const std::string& name,
                         rapidjson::Document::AllocatorType& allocator);
template <class T>
void get_value_from_object(const rapidjson::Value& object,
                           T& value,
                           const std::string& name);

std::optional<std::string> get_optional_string_value_from_object(
    const rapidjson::Value& object,
    const std::string& name);

// Basic types (more can be added as required) will be defined in source file
// int
void set_value(rapidjson::Value& json_val,
               const size_t& value,
               rapidjson::Document::AllocatorType& allocator);
void get_value(const rapidjson::Value& json_val, size_t& value);
// unsigned long int / size_t
void set_value(rapidjson::Value& json_val,
               const int& value,
               rapidjson::Document::AllocatorType& allocator);
void get_value(const rapidjson::Value& json_val, int& value);
// string
void set_value(rapidjson::Value& json_val,
               const std::string& value,
               rapidjson::Document::AllocatorType& allocator);
void get_value(const rapidjson::Value& json_val, std::string& value);

// int64
void set_value(rapidjson::Value& json_val,
               const int64_t& value,
               rapidjson::Document::AllocatorType& allocator);

void get_value(const rapidjson::Value& json_val, int64_t& value);

// bool
void set_value(rapidjson::Value& json_val,
               const bool& value,
               rapidjson::Document::AllocatorType& allocator);

void get_value(const rapidjson::Value& json_val, bool& value);

// SQLTypes
void set_value(rapidjson::Value& json_val,
               const SQLTypes& value,
               rapidjson::Document::AllocatorType& allocator);

void get_value(const rapidjson::Value& json_val, SQLTypes& value);

// EncodingType
void set_value(rapidjson::Value& json_val,
               const EncodingType& column_desc,
               rapidjson::Document::AllocatorType& allocator);

void get_value(const rapidjson::Value& json_val, EncodingType& column_desc);

// StringDictKey
void set_value(rapidjson::Value& json_val,
               const shared::StringDictKey& dict_key,
               rapidjson::Document::AllocatorType& allocator);

void get_value(const rapidjson::Value& json_val, shared::StringDictKey& dict_key);

// SQLTypeInfo
void set_value(rapidjson::Value& json_val,
               const SQLTypeInfo& type_info,
               rapidjson::Document::AllocatorType& allocator);

template <typename T, typename MemberFunc>
void get_value_from_object(const rapidjson::Value& json_val,
                           MemberFunc mem_fn,
                           SQLTypeInfo& type_info,
                           const std::string& key) {
  T value;
  auto setter = std::bind(mem_fn, &type_info, std::placeholders::_1);
  get_value_from_object(json_val, value, key);
  setter(value);
}

void get_value(const rapidjson::Value& json_val, SQLTypeInfo& type_info);

// general case (uses SFINAE to determine use, activated for objects with
// `serializeToJson` and `deserializeFromJson`)
template <class T>
void set_value(rapidjson::Value& json_val,
               const T& value,
               rapidjson::Document::AllocatorType& allocator) {
  value.serializeToJson(json_val, allocator);
}

template <class T>
void get_value(const rapidjson::Value& json_val, T& value) {
  value.deserializeFromJson(json_val);
}

// std::vector
template <class T>
void set_value(rapidjson::Value& json_val,
               const std::vector<T>& vector_value,
               rapidjson::Document::AllocatorType& allocator) {
  json_val.SetArray();
  for (const auto& value : vector_value) {
    rapidjson::Value json_obj;
    set_value(json_obj, value, allocator);
    json_val.PushBack(json_obj, allocator);
  }
}

template <class T>
void get_value(const rapidjson::Value& json_val, std::vector<T>& vector_value) {
  CHECK(json_val.IsArray());
  CHECK_EQ(vector_value.size(), size_t(0));
  for (const auto& json_obj : json_val.GetArray()) {
    T val;
    get_value(json_obj, val);
    vector_value.push_back(val);
  }
}

// std::list
template <class T>
void set_value(rapidjson::Value& json_val,
               const std::list<T>& list_value,
               rapidjson::Document::AllocatorType& allocator) {
  json_val.SetArray();
  for (const auto& value : list_value) {
    rapidjson::Value json_obj;
    set_value(json_obj, value, allocator);
    json_val.PushBack(json_obj, allocator);
  }
}

template <class T>
void get_value(const rapidjson::Value& json_val, std::list<T>& list_value) {
  CHECK(json_val.IsArray());
  CHECK_EQ(list_value.size(), size_t(0));
  for (const auto& json_obj : json_val.GetArray()) {
    T val;
    get_value(json_obj, val);
    list_value.push_back(val);
  }
}

// std::vector<std::pair>
template <class T, class V>
void set_value(rapidjson::Value& json_val,
               const std::vector<std::pair<T, V>>& vector_value,
               rapidjson::Document::AllocatorType& allocator) {
  json_val.SetArray();
  for (const auto& pair : vector_value) {
    rapidjson::Value pair_obj;
    pair_obj.SetObject();
    add_value_to_object(pair_obj, pair.first, "key", allocator);
    add_value_to_object(pair_obj, pair.second, "value", allocator);
    json_val.PushBack(pair_obj, allocator);
  }
}

template <class T, class V>
void get_value(const rapidjson::Value& json_val,
               std::vector<std::pair<T, V>>& vector_value) {
  CHECK(json_val.IsArray());
  CHECK_EQ(vector_value.size(), size_t(0));
  for (const auto& json_obj : json_val.GetArray()) {
    CHECK(json_obj.IsObject());
    T key;
    V value;
    get_value_from_object(json_obj, key, "key");
    get_value_from_object(json_obj, value, "value");
    vector_value.emplace_back(std::make_pair(key, value));
  }
}

// std::list<std::pair>
template <class T, class V>
void set_value(rapidjson::Value& json_val,
               const std::list<std::pair<T, V>>& list_value,
               rapidjson::Document::AllocatorType& allocator) {
  json_val.SetArray();
  for (const auto& pair : list_value) {
    rapidjson::Value pair_obj;
    pair_obj.SetObject();
    add_value_to_object(pair_obj, pair.first, "key", allocator);
    add_value_to_object(pair_obj, pair.second, "value", allocator);
    json_val.PushBack(pair_obj, allocator);
  }
}

template <class T, class V>
void get_value(const rapidjson::Value& json_val, std::list<std::pair<T, V>>& list_value) {
  CHECK(json_val.IsArray());
  CHECK_EQ(list_value.size(), size_t(0));
  for (const auto& json_obj : json_val.GetArray()) {
    CHECK(json_obj.IsObject());
    T key;
    V value;
    get_value_from_object(json_obj, key, "key");
    get_value_from_object(json_obj, value, "value");
    list_value.emplace_back(std::make_pair(key, value));
  }
}

// std::map
template <class T, class V>
void set_value(rapidjson::Value& json_val,
               const std::map<T, V>& map_value,
               rapidjson::Document::AllocatorType& allocator) {
  json_val.SetArray();
  for (const auto& pair : map_value) {
    rapidjson::Value pair_obj;
    pair_obj.SetObject();
    add_value_to_object(pair_obj, pair.first, "key", allocator);
    add_value_to_object(pair_obj, pair.second, "value", allocator);
    json_val.PushBack(pair_obj, allocator);
  }
}

template <class T, class V>
void get_value(const rapidjson::Value& json_val, std::map<T, V>& map_value) {
  CHECK(json_val.IsArray());
  CHECK_EQ(map_value.size(), size_t(0));
  for (const auto& json_obj : json_val.GetArray()) {
    CHECK(json_obj.IsObject());
    T key;
    V value;
    get_value_from_object(json_obj, key, "key");
    get_value_from_object(json_obj, value, "value");
    map_value[key] = value;
  }
}

// Serialize value into json object with valid set_value functions
template <class T>
void add_value_to_object(rapidjson::Value& object,
                         const T& value,
                         const std::string& name,
                         rapidjson::Document::AllocatorType& allocator) {
  CHECK(object.IsObject());
  CHECK(!object.HasMember(name)) << "Found unexpected member: " << name;
  rapidjson::Value json_val;
  set_value(json_val, value, allocator);
  rapidjson::Value json_name;
  json_name.SetString(name, allocator);
  object.AddMember(json_name, json_val, allocator);
}

// Deserialize value from json object with valid get_value/set_value functions
template <class T>
void get_value_from_object(const rapidjson::Value& object,
                           T& value,
                           const std::string& name) {
  CHECK(object.IsObject());
  CHECK(object.HasMember(name)) << "Could not find member: " << name;
  get_value(object[name], value);
}

// Read JSON content from the given file path
rapidjson::Document read_from_file(const std::string& file_path);

// Read JSON content from the input string
rapidjson::Document read_from_string(const std::string& input);

// Write JSON content (encapsulated by the given Document object) to the given file path
void write_to_file(const rapidjson::Document& document, const std::string& file_path);

std::string write_to_string(const rapidjson::Document& document);

class json_schema_error : public std::runtime_error {
 public:
  json_schema_error(std::string msg) : std::runtime_error("json parsing error: " + msg) {}
};

const rapidjson::Value& get_member(const rapidjson::Value& value, const std::string& key);

int32_t get_int(const rapidjson::Value& value);

int32_t get_int_member(const rapidjson::Value& value, const std::string& key);

std::vector<int32_t> get_int_vector(const rapidjson::Value& value);

}  // namespace json_utils
