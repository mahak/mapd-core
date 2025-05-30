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
 * @file    InValuesBitmap.h
 * @brief
 *
 */

#ifndef QUERYENGINE_INVALUESBITMAP_H
#define QUERYENGINE_INVALUESBITMAP_H

#include "../DataMgr/DataMgr.h"
#include "ThriftHandler/CommandLineOptions.h"

#include <llvm/IR/Value.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

class Executor;

class FailedToCreateBitmap : public std::runtime_error {
 public:
  FailedToCreateBitmap() : std::runtime_error("FailedToCreateBitmap") {}
};

class InValuesBitmap {
 public:
  InValuesBitmap(const std::vector<int64_t>& values,
                 const int64_t null_val,
                 const Data_Namespace::MemoryLevel memory_level,
                 Executor* executor,
                 CompilationOptions const& co);
  ~InValuesBitmap();

  llvm::Value* codegen(llvm::Value* needle, Executor* executor) const;

  bool isEmpty() const;

  bool hasNull() const;

  struct BitIsSetParams {
    llvm::Value* bitmap_ptr_lv;
    llvm::Value* min_val_lv;
    llvm::Value* max_val_lv;
    llvm::Value* null_val_lv;
  };

  BitIsSetParams prepareBitIsSetParams(
      Executor* executor,
      std::unordered_map<int, std::shared_ptr<const Analyzer::Constant>> const&
          constant_owned) const;

 private:
  std::unordered_map<int, int8_t*> bitsets_per_devices_;
  bool rhs_has_null_;
  int64_t min_val_;
  int64_t max_val_;
  const int64_t null_val_;
  const Data_Namespace::MemoryLevel memory_level_;
  CompilationOptions co_;
};

#endif  // QUERYENGINE_INVALUESBITMAP_H
