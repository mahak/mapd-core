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

#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifdef HAVE_CUDA
#include <cuda.h>
#else
#include "Shared/nocuda.h"
#endif  // HAVE_CUDA

#include "CudaMgr/CudaShared.h"
#include "CudaMgr/DeviceMemoryAllocationMap.h"
#ifdef HAVE_CUDA
#include "CudaMgr/JumpBufferTransferMgr.h"
#endif
#include "Logger/Logger.h"
#include "Shared/DeviceGroup.h"

namespace CudaMgr_Namespace {

enum class NvidiaDeviceArch {
  Pascal,    // compute major = 6
  Volta,     // compute major = 7, compute minor = 0
  Turing,    // compute major = 7, compute minor = 5
  Ampere,    // compute major = 8, compute minor = 0
  Ada,       // compute major = 8, compute minor = 9
  Hopper,    // compute major = 9
  Blackwell  // compute major = 10 or 12
};

std::ostream& operator<<(std::ostream& os, const NvidiaDeviceArch device_arch);

struct DeviceProperties {
  CUdevice device;
  heavyai::UUID uuid;
  int computeMajor;
  int computeMinor;
  size_t globalMem;
  int constantMem;
  int sharedMemPerBlockOptIn;
  int sharedMemPerBlock;
  int sharedMemPerMP;
  int numMPs;
  int warpSize;
  int maxThreadsPerBlock;
  int maxRegistersPerBlock;
  int maxRegistersPerMP;
  int pciBusId;
  int pciDeviceId;
  int memoryClockKhz;
  int memoryBusWidth;  // in bits
  float memoryBandwidthGBs;
  int clockKhz;
  int numCore;
  size_t allocationGranularity;
};

class CudaMgr {
 public:
  CudaMgr(const int num_gpus, const int start_gpu = 0);
  virtual ~CudaMgr();

  void synchronizeDevices() const;
  int getDeviceCount() const { return device_count_; }
  int getStartGpu() const { return start_gpu_; }
  const heavyai::DeviceGroup& getDeviceGroup() const { return device_group_; }
  size_t computePaddedBufferSize(size_t buf_size, size_t granularity) const;
  size_t getGranularity(const int device_num) const;

  void copyHostToDevice(int8_t* device_ptr,
                        const int8_t* host_ptr,
                        const size_t num_bytes,
                        const int device_num,
                        std::optional<std::string_view> tag,
                        CUstream cuda_stream = 0);
  void copyDeviceToHost(int8_t* host_ptr,
                        const int8_t* device_ptr,
                        const size_t num_bytes,
                        std::optional<std::string_view> tag,
                        CUstream cuda_stream = 0);
  void copyDeviceToDevice(int8_t* dest_ptr,
                          int8_t* src_ptr,
                          const size_t num_bytes,
                          const int dest_device_num,
                          const int src_device_num,
                          std::optional<std::string_view> tag,
                          CUstream cuda_stream = 0);

  virtual int8_t* allocateDeviceMem(const size_t num_bytes,
                                    const int device_num,
                                    const bool is_slab = false);
  void freeDeviceMem(int8_t* device_ptr);
  void zeroDeviceMem(int8_t* device_ptr,
                     const size_t num_bytes,
                     const int device_num,
                     CUstream cuda_stream = 0);
  void setDeviceMem(int8_t* device_ptr,
                    const unsigned char uc,
                    const size_t num_bytes,
                    const int device_num,
                    CUstream cuda_stream = 0);

  size_t getMinSharedMemoryPerBlockForAllDevices() const {
    return min_shared_memory_per_block_for_all_devices;
  }

  size_t getMinNumMPsForAllDevices() const { return min_num_mps_for_all_devices; }

  const std::vector<DeviceProperties>& getAllDeviceProperties() const {
    return device_properties_;
  }
  const DeviceProperties* getDeviceProperties(const size_t device_num) const {
    // device_num is the device number relative to start_gpu_ (real_device_num -
    // start_gpu_)
    CHECK(device_properties_initialized_);
    if (device_num < device_properties_.size()) {
      return &device_properties_[device_num];
    }
    throw std::runtime_error("Specified device number " + std::to_string(device_num) +
                             " is out of range of number of devices (" +
                             std::to_string(device_properties_.size()) + ")");
  }
  inline bool isArchPascal() const {
    CHECK(device_properties_initialized_);
    return (getDeviceCount() > 0 && device_properties_[0].computeMajor == 6);
  }
  bool isArchVoltaOrGreaterForAll() const;

  static std::string deviceArchToSM(const NvidiaDeviceArch arch) {
    // Must match ${CUDA_COMPILATION_ARCH} CMAKE flag
    switch (arch) {
      case NvidiaDeviceArch::Pascal:
        return "sm_60";
      case NvidiaDeviceArch::Volta:
        return "sm_70";
      case NvidiaDeviceArch::Turing:
        return "sm_75";
      case NvidiaDeviceArch::Ampere:
        return "sm_80";
      // For Ada, Hopper, and Blackwell architectures, use the latest compute capability
      // that is supported by the current LLVM version (LLVM 14). Update returned value
      // when LLVM is updated.
      case NvidiaDeviceArch::Ada:
      case NvidiaDeviceArch::Hopper:
      case NvidiaDeviceArch::Blackwell:
        return "sm_86";
    }
    UNREACHABLE();
    return "";
  }

  NvidiaDeviceArch getDeviceArch() const {
    CHECK_GT(device_properties_.size(), 0u)
        << "Failed to fetch CUDA device properties. Server cannot start.";
    auto const compute_major = device_properties_.front().computeMajor;
    auto const compute_minor = device_properties_.front().computeMinor;
    if (compute_major == 6) {
      return NvidiaDeviceArch::Pascal;
    } else if (compute_major == 7) {
      if (compute_minor < 5) {
        return NvidiaDeviceArch::Volta;
      } else {
        return NvidiaDeviceArch::Turing;
      }
    } else if (compute_major == 8) {
      if (compute_minor < 9) {
        return NvidiaDeviceArch::Ampere;
      } else {
        return NvidiaDeviceArch::Ada;
      }
    } else if (compute_major == 9) {
      return NvidiaDeviceArch::Hopper;
    } else if (compute_major == 10 || compute_major == 12) {
      return NvidiaDeviceArch::Blackwell;
    } else if (compute_major > 12) {
      LOG(WARNING) << "Unrecognized CUDA device (compute version " << compute_major << "."
                   << compute_minor << "), treating as Blackwell";
      return NvidiaDeviceArch::Blackwell;
    }
    LOG(FATAL) << "Unsupported CUDA device (compute version " << compute_major << "."
               << compute_minor << ")";
    return NvidiaDeviceArch::Pascal;
  }

  void setContext(const int device_num) const;
  int getContext() const;

#ifdef HAVE_CUDA

  void logDeviceProperties() const;

  void finishDevicePropertiesInitialization() {
    device_properties_initialized_ = true;
  }

  const std::vector<CUcontext>& getDeviceContexts() const {
    return device_contexts_;
  }
  const int getGpuDriverVersion() const {
    return gpu_driver_version_;
  }

  void loadGpuModuleData(CUmodule* module,
                         const void* image,
                         unsigned int num_options,
                         CUjit_option* options,
                         void** option_values,
                         const int device_id) const;
  void unloadGpuModuleData(CUmodule* module, const int device_id) const;

  struct CudaMemoryUsage {
    size_t free;   // available GPU RAM memory on active card in bytes
    size_t total;  // total GPU RAM memory on active card in bytes
  };

  std::vector<CudaMgr::CudaMemoryUsage> getCudaMemoryUsage();

  std::string getCudaMemoryUsageInString();

  DeviceMemoryAllocationMap& getDeviceMemoryAllocationMap();
  int exportHandle(const uint64_t handle) const;
  void enableMemoryActivityLog();
  bool logMemoryActivity() const;

  int getDeviceNumFromDevicePtr(CUdeviceptr cu_device_ptr,
                                const size_t allocated_mem_bytes);
#endif

 private:
#ifdef HAVE_CUDA
  void fillDeviceProperties();
  void initDeviceGroup();
  void createDeviceContexts();
  size_t computeMinSharedMemoryPerBlockForAllDevices() const;
  size_t computeMinNumMPsForAllDevices() const;
  void checkError(CUresult cu_result) const;

  int gpu_driver_version_;
#endif

  int device_count_;
  int start_gpu_;
  size_t min_shared_memory_per_block_for_all_devices;
  size_t min_num_mps_for_all_devices;
  std::vector<DeviceProperties> device_properties_;
  heavyai::DeviceGroup device_group_;
  std::vector<CUcontext> device_contexts_;
  mutable std::mutex device_mutex_;
  bool device_properties_initialized_{false};

#ifdef HAVE_CUDA
  bool log_memory_activity_;
  DeviceMemoryAllocationMapUqPtr device_memory_allocation_map_;
  std::unique_ptr<JumpBufferTransferMgr> jump_buffer_transfer_mgr_;
#endif
};

}  // Namespace CudaMgr_Namespace

extern std::string get_cuda_home(void);
extern std::string get_cuda_libdevice_dir(void);
