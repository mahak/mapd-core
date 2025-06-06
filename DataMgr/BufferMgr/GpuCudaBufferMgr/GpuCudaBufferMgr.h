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

#include "DataMgr/BufferMgr/BufferMgr.h"

namespace CudaMgr_Namespace {
class CudaMgr;
}

namespace Buffer_Namespace {

class GpuCudaBufferMgr : public BufferMgr {
 public:
  GpuCudaBufferMgr(const int device_id,
                   const size_t max_buffer_pool_size,
                   CudaMgr_Namespace::CudaMgr* cuda_mgr,
                   const size_t min_slab_size,
                   const size_t max_slab_size,
                   const size_t default_slab_size,
                   const size_t page_size,
                   AbstractBufferMgr* parent_mgr = 0);
  inline MgrType getMgrType() override { return GPU_MGR; }
  inline std::string getStringMgrType() override { return ToString(GPU_MGR); }
  ~GpuCudaBufferMgr() override;

 private:
  void addSlab(const size_t slab_size) override;
  void freeAllMem() override;
  Buffer* createBuffer(BufferList::iterator seg_it, size_t page_size) override;
  CudaMgr_Namespace::CudaMgr* cuda_mgr_;
};

}  // namespace Buffer_Namespace
