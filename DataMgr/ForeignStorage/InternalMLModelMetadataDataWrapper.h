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

#include <map>
#include <vector>

#include "Catalog/ForeignTable.h"
#include "DataMgr/Chunk/Chunk.h"
#include "ForeignDataWrapper.h"
#include "InternalSystemDataWrapper.h"

#include "QueryEngine/TableFunctions/SystemFunctions/os/ML/MLModelMetadata.h"
namespace foreign_storage {

class InternalMLModelMetadataDataWrapper : public InternalSystemDataWrapper {
 public:
  InternalMLModelMetadataDataWrapper();

  InternalMLModelMetadataDataWrapper(const int db_id, const ForeignTable* foreign_table);

 private:
  void initializeObjectsForTable(const std::string& table_name) override;

  void populateChunkBuffersForTable(
      const std::string& table_name,
      std::map<std::string, import_export::UnmanagedTypedImportBuffer*>& import_buffers)
      override;

  std::vector<MLModelMetadata> ml_models_metadata_;
};
}  // namespace foreign_storage
