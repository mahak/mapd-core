if(NOT MSVC)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-result")
endif()

set(datamgr_source_files
    AbstractBuffer.cpp
    Allocators/CpuMgrArenaAllocator.cpp
    Allocators/CudaAllocator.cpp
    Allocators/ThrustAllocator.cpp
    Chunk/Chunk.cpp
    DataMgr.cpp
    Encoder.cpp
    StringNoneEncoder.cpp
    FileMgr/CachingFileMgr.cpp
    FileMgr/GlobalFileMgr.cpp
    FileMgr/CachingGlobalFileMgr.cpp
    FileMgr/FileMgr.cpp
    FileMgr/FileBuffer.cpp
    FileMgr/FileInfo.cpp
    ForeignStorage/AbstractTextFileDataWrapper.cpp
    ForeignStorage/ArrowForeignStorage.cpp
    ForeignStorage/CacheEvictionAlgorithms/LRUEvictionAlgorithm.cpp
    ForeignStorage/CsvDataWrapper.cpp
    ForeignStorage/CachingForeignStorageMgr.cpp
    ForeignStorage/DummyForeignStorage.cpp
    ForeignStorage/ForeignStorageInterface.cpp
    ForeignStorage/ForeignStorageBuffer.cpp
    ForeignStorage/PassThroughBuffer.cpp
    ForeignStorage/ForeignStorageMgr.cpp
    ForeignStorage/ForeignStorageCache.cpp
    ForeignStorage/ForeignTableSchema.cpp
    ForeignStorage/FsiChunkUtils.cpp
    ForeignStorage/FileReader.cpp
    ForeignStorage/FileRegions.cpp
    ForeignStorage/TextFileBufferParser.cpp
    ForeignStorage/CsvFileBufferParser.cpp
    ForeignStorage/DataPreview.cpp
    BufferMgr/GpuCudaBufferMgr/GpuCudaBufferMgr.cpp
    BufferMgr/GpuCudaBufferMgr/GpuCudaBuffer.cpp
    BufferMgr/CpuBufferMgr/CpuBufferMgr.cpp
    BufferMgr/CpuBufferMgr/CpuBuffer.cpp
    BufferMgr/BufferMgr.cpp
    BufferMgr/Buffer.cpp
    PersistentStorageMgr/PersistentStorageMgr.cpp
    ForeignStorage/ForeignTableRefresh.cpp
    ForeignStorage/AbstractFileStorageDataWrapper.cpp
    ForeignStorage/ForeignDataWrapperFactory.cpp
    ForeignStorage/InternalCatalogDataWrapper.cpp
    ForeignStorage/InternalExecutorStatsDataWrapper.cpp
    ForeignStorage/InternalMLModelMetadataDataWrapper.cpp
    ForeignStorage/InternalMemoryStatsDataWrapper.cpp
    ForeignStorage/InternalStorageStatsDataWrapper.cpp
    ForeignStorage/InternalSystemDataWrapper.cpp
    ForeignStorage/RegexParserDataWrapper.cpp
    ForeignStorage/RegexFileBufferParser.cpp
    ForeignStorage/LogFileBufferParser.cpp
    ForeignStorage/InternalLogsDataWrapper.cpp
)

if(NOT ENABLE_SYSTEM_TFS)
  # ForeignStorage/InternalMLModelMetadataDataWrapper.cpp uses class MLModelMetadata
  # but is only otherwise compiled in QueryEngine/CMakeLists.txt when ENABLE_SYSTEM_TFS.
  list(APPEND datamgr_source_files
       ${CMAKE_SOURCE_DIR}/QueryEngine/TableFunctions/SystemFunctions/os/ML/MLModelMetadata.cpp)
endif()

if(ENABLE_AWS_S3)
  list(APPEND datamgr_source_files ForeignStorage/S3FilePathUtil.cpp)
endif()

if("${MAPD_EDITION_LOWER}" STREQUAL "ee")
  if(ENABLE_AWS_S3)
    list(APPEND datamgr_source_files ForeignStorage/ee/FileReaderS3.cpp)
    list(APPEND datamgr_source_files ForeignStorage/ee/S3Utils.cpp)
    list(APPEND datamgr_source_files ForeignStorage/ee/S3SelectClient.cpp)
    list(APPEND datamgr_source_files ForeignStorage/ee/S3SelectDataWrapper.cpp)
  endif()
  list(APPEND datamgr_source_files ForeignStorage/ee/RasterDataWrapper.cpp)
endif()

if(ENABLE_IMPORT_PARQUET)
  list(APPEND datamgr_source_files ForeignStorage/ParquetDataWrapper.cpp)
  list(APPEND datamgr_source_files ForeignStorage/ParquetShared.cpp)
  list(APPEND datamgr_source_files ForeignStorage/LazyParquetChunkLoader.cpp)
  list(APPEND datamgr_source_files ForeignStorage/TypedParquetStorageBuffer.cpp)
  list(APPEND datamgr_source_files ForeignStorage/TypedParquetDetectBuffer.cpp)
  list(APPEND datamgr_source_files ForeignStorage/ParquetImporter.cpp)
endif()

# On Linux, Arrow internally links symbols for its dependency libraries (such as the C++ AWS SDK)
# when it is built as a shared library. In this case, set a flag that would be used to determine
# how to handle AWS SDK initialization and shutdown.
function(set_arrow_has_private_aws_sdk)
  if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    foreach(LIB_PATH ${Arrow_LIBRARIES})
      get_filename_component(LIB_NAME ${LIB_PATH} NAME)
      if (${LIB_NAME} STREQUAL "${CMAKE_SHARED_LIBRARY_PREFIX}arrow${CMAKE_SHARED_LIBRARY_SUFFIX}")
        add_definitions("-DARROW_HAS_PRIVATE_AWS_SDK")
      endif()
    endforeach()
  endif()
endfunction()

if(ENABLE_AWS_S3)
  list(APPEND datamgr_source_files HeavyDbAwsSdk.cpp)
  set_arrow_has_private_aws_sdk()
endif()

add_library(DataMgr ${datamgr_source_files})
add_dependencies(DataMgr Calcite)

target_link_libraries(DataMgr CudaMgr Shared ${Boost_THREAD_LIBRARY} ${TBB_LIBS} ${CMAKE_DL_LIBS})
# Generator expressions such as "$<$<BOOL:${ENABLE_FOLLY}>:${XXX_LIBRARIES}>"
# cannot be used in target_link_libraries. On Windows, ${XXX_LIBRARIES} includes the substring
# ";optimized;" resulting in an attempt to link to optimized.lib which does not exist.
# See https://cmake.org/cmake/help/latest/command/target_link_libraries.html for details,
# specifically the section containing "These keywords are interpreted immediately by this command
# and therefore have no special meaning when produced by a generator expression."
if(ENABLE_FSI_ODBC)
  target_link_libraries(DataMgr ${iODBC_LIBRARIES})
endif()

option(ENABLE_CRASH_CORRUPTION_TEST "Enable crash using SIGUSR2 during page deletion to faster and affirmative test/repro db corruption" OFF)
if(ENABLE_CRASH_CORRUPTION_TEST)
  add_definitions("-DENABLE_CRASH_CORRUPTION_TEST")
endif()
