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

/**
 * @file    QueryPhysicalInputsCollector.h
 * @brief   Find out all the physical inputs (columns) a query is using.
 *
 */

#ifndef QUERYENGINE_QUERYPHYSICALINPUTSCOLLECTOR_H
#define QUERYENGINE_QUERYPHYSICALINPUTSCOLLECTOR_H

#include "Shared/DbObjectKeys.h"

#include <ostream>
#include <unordered_set>

class RelAlgNode;

struct PhysicalInput {
  int32_t col_id;
  int32_t table_id;
  int32_t db_id;

  size_t hash() const;

  bool operator==(const PhysicalInput& that) const;
};

std::ostream& operator<<(std::ostream&, PhysicalInput const&);

namespace std {

template <>
struct hash<PhysicalInput> {
  size_t operator()(const PhysicalInput& phys_input) const { return phys_input.hash(); }
};

}  // namespace std

std::unordered_set<PhysicalInput> get_physical_inputs(const RelAlgNode*);
std::unordered_set<shared::TableKey> get_physical_table_inputs(const RelAlgNode*);

#endif  // QUERYENGINE_QUERYPHYSICALINPUTSCOLLECTOR_H
