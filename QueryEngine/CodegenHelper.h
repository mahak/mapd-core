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

#include "CodeGenerator.h"
#include "CompilationOptions.h"

#include <optional>
#include <string_view>

#include <llvm/IR/IRBuilder.h>

namespace CodegenUtil {

// todo (yoonmin) : locate more utility functions used during codegen here
llvm::Function* findCalledFunction(llvm::CallInst& call_inst);
std::optional<std::string_view> getCalledFunctionName(llvm::CallInst& call_inst);
std::unordered_map<int, llvm::Value*> createPtrWithHoistedMemoryAddr(
    CgenState* cgen_state,
    CodeGenerator* code_generator,
    CompilationOptions const& co,
    llvm::ConstantInt* ptr,
    llvm::Type* type,
    std::set<int> const& target_device_ids);
std::unordered_map<int, llvm::Value*> hoistLiteral(
    CodeGenerator* code_generator,
    CompilationOptions const& co,
    Datum d,
    SQLTypeInfo type,
    std::set<int> const& target_device_ids);

}  // namespace CodegenUtil
