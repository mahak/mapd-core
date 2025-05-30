set(THRIFT_HANDLER_SOURCES DBHandler.cpp RequestInfo.cpp TokenCompletionHints.cpp CommandLineOptions.cpp SystemValidator.cpp ForeignTableRefreshScheduler.cpp)
set(THRIFT_HANDLER_LIBS mapd_thrift Shared ${CMAKE_DL_LIBS})

if(ENABLE_RUNTIME_LIBS)
  list(APPEND THRIFT_HANDLER_LIBS RuntimeLibManager)
endif(ENABLE_RUNTIME_LIBS)

if("${MAPD_EDITION_LOWER}" STREQUAL "ee")
  list(APPEND THRIFT_HANDLER_LIBS ${RdKafka_LIBRARIES} StringDictionary)
  include_directories(${CMAKE_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/ee")
  list(APPEND THRIFT_HANDLER_SOURCES ee/RenderHandler.cpp ee/DistributedHandler.cpp ee/HAHandler.cpp ee/KafkaMgr.cpp ee/DistributedValidate.cpp)
elseif("${MAPD_EDITION_LOWER}" STREQUAL "ce")
  include_directories(${CMAKE_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/ee")
  list(APPEND THRIFT_HANDLER_SOURCES ee/RenderHandler.cpp ee/ProjHitTestColSqlBuilder.cpp ee/DistributedHandler.cpp)
else()
  include_directories(${CMAKE_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/os")
  list(APPEND THRIFT_HANDLER_SOURCES os/RenderHandler.cpp)
endif()

add_library(QueryState QueryState.cpp)
add_dependencies(QueryState Calcite)

add_library(QueryParsing QueryParsing.cpp QueryAuth.cpp)
add_dependencies(QueryParsing Calcite)

add_library(token_completion_hints TokenCompletionHints.cpp)
target_link_libraries(token_completion_hints mapd_thrift)

add_library(thrift_handler ${THRIFT_HANDLER_SOURCES})
add_dependencies(thrift_handler Parser Calcite)
target_link_libraries(thrift_handler token_completion_hints QueryState QueryParsing UdfCompiler MigrationMgr ${THRIFT_HANDLER_LIBS})
