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

#include "DataMgr/Allocators/CudaAllocator.h"

#include <CudaMgr/CudaMgr.h>
#include <DataMgr/DataMgr.h>
#include <Logger/Logger.h>
#include <Shared/types.h>

CudaAllocator::CudaAllocator(Data_Namespace::DataMgr* data_mgr,
                             const int device_id,
                             CUstream cuda_stream)
    : data_mgr_(data_mgr), device_id_(device_id), cuda_stream_(cuda_stream) {
  CHECK(data_mgr_);
#ifdef HAVE_CUDA
  const auto cuda_mgr = data_mgr_->getCudaMgr();
  CHECK(cuda_mgr);
  cuda_mgr->setContext(device_id);
#endif  // HAVE_CUDA
}

CudaAllocator::~CudaAllocator() {
  CHECK(data_mgr_);
  for (auto& buffer_ptr : owned_buffers_) {
    data_mgr_->free(buffer_ptr);
  }
}

Data_Namespace::AbstractBuffer* CudaAllocator::allocGpuAbstractBuffer(
    Data_Namespace::DataMgr* data_mgr,
    const size_t num_bytes,
    const int device_id) {
  CHECK(data_mgr);
  auto ab = data_mgr->alloc(Data_Namespace::GPU_LEVEL, device_id, num_bytes);
  CHECK_EQ(ab->getPinCount(), 1);
#ifdef HAVE_CUDA
  auto cuda_mgr = data_mgr->getCudaMgr();
  CHECK(cuda_mgr);
  if (cuda_mgr->logMemoryActivity()) {
    VLOG(1) << "DataMgr allocating GPU memory: " << num_bytes
            << " bytes (address: " << static_cast<void*>(ab->getMemoryPtr())
            << ", device-" << device_id << ")";
  }
#endif
  return ab;
}

void CudaAllocator::freeGpuAbstractBuffer(Data_Namespace::DataMgr* data_mgr,
                                          Data_Namespace::AbstractBuffer* ab) {
  CHECK(data_mgr);
  data_mgr->free(ab);
#ifdef HAVE_CUDA
  auto cuda_mgr = data_mgr->getCudaMgr();
  CHECK(cuda_mgr);
  if (cuda_mgr->logMemoryActivity()) {
    VLOG(1) << "DataMgr freeing GPU memory: " << ab->size()
            << " bytes (address: " << static_cast<void*>(ab->getMemoryPtr())
            << ", device-" << ab->getDeviceId() << ")";
  }
#endif
}

int8_t* CudaAllocator::alloc(const size_t num_bytes) {
  CHECK(data_mgr_);
  owned_buffers_.emplace_back(
      CudaAllocator::allocGpuAbstractBuffer(data_mgr_, num_bytes, device_id_));
  return owned_buffers_.back()->getMemoryPtr();
}

void CudaAllocator::free(Data_Namespace::AbstractBuffer* ab) const {
  data_mgr_->free(ab);
#ifdef HAVE_CUDA
  auto cuda_mgr = data_mgr_->getCudaMgr();
  CHECK(cuda_mgr);
  if (ab->getType() == MemoryLevel::GPU_LEVEL && cuda_mgr->logMemoryActivity()) {
    VLOG(1) << "DataMgr freeing GPU memory: " << ab->size()
            << " bytes (address: " << static_cast<void*>(ab->getMemoryPtr())
            << ", device-" << ab->getDeviceId() << ")";
  }
#endif
}

void CudaAllocator::copyToDevice(void* device_dst,
                                 const void* host_src,
                                 const size_t num_bytes,
                                 std::optional<std::string_view> tag) const {
  const auto cuda_mgr = data_mgr_->getCudaMgr();
  CHECK(cuda_mgr);
  cuda_mgr->copyHostToDevice(
      (int8_t*)device_dst, (int8_t*)host_src, num_bytes, device_id_, tag, cuda_stream_);
}

void CudaAllocator::copyFromDevice(void* host_dst,
                                   const void* device_src,
                                   const size_t num_bytes,
                                   std::optional<std::string_view> tag) const {
  const auto cuda_mgr = data_mgr_->getCudaMgr();
  CHECK(cuda_mgr);
  cuda_mgr->copyDeviceToHost(
      (int8_t*)host_dst, (int8_t*)device_src, num_bytes, tag, cuda_stream_);
}

void CudaAllocator::zeroDeviceMem(int8_t* device_ptr, const size_t num_bytes) const {
  const auto cuda_mgr = data_mgr_->getCudaMgr();
  CHECK(cuda_mgr);
  cuda_mgr->zeroDeviceMem(device_ptr, num_bytes, device_id_, cuda_stream_);
}

void CudaAllocator::setDeviceMem(int8_t* device_ptr,
                                 unsigned char uc,
                                 const size_t num_bytes) const {
  const auto cuda_mgr = data_mgr_->getCudaMgr();
  CHECK(cuda_mgr);
  cuda_mgr->setDeviceMem(device_ptr, uc, num_bytes, device_id_, cuda_stream_);
}
