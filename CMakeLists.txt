cmake_minimum_required(VERSION 3.20)
project(pdal VERSION 0.1.0 LANGUAGES CXX)

option(PDAL_WITH_AVS "Build the AVS hot-tier storage adapter" ON)
set(PDAL_AVS_SOURCE_ROOT "/home/avs/AVS-PI/src/avs" CACHE PATH
    "Path to the AVS ROS package source tree")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost 1.81 REQUIRED COMPONENTS json system)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(yaml-cpp REQUIRED)

add_library(pdal_core
  src/model/types.cpp
  src/model/error.cpp
  src/model/json.cpp
  src/catalog/resource_catalog.cpp
  src/policy/policy_engine.cpp
  src/planner/query_planner.cpp
  src/storage/storage_backend.cpp
  src/representation/representation_provider.cpp
  src/adapter/request_translator.cpp
  src/audit/audit.cpp
  src/pipeline.cpp
)
target_include_directories(pdal_core PUBLIC include)
target_link_libraries(pdal_core
  PUBLIC Boost::json yaml-cpp OpenSSL::Crypto Threads::Threads
)
target_compile_options(pdal_core PRIVATE -Wall -Wextra -Wpedantic)

if(PDAL_WITH_AVS)
  if(NOT EXISTS "${PDAL_AVS_SOURCE_ROOT}/include/avs/retrieve_api.h" OR
     NOT EXISTS "${PDAL_AVS_SOURCE_ROOT}/src/retrieve/retrieve_api.cpp")
    message(FATAL_ERROR
      "PDAL_WITH_AVS=ON but RetrieveAPI was not found under PDAL_AVS_SOURCE_ROOT=${PDAL_AVS_SOURCE_ROOT}")
  endif()
  add_library(pdal_avs_backend
    src/storage/avs_storage_backend.cpp
    "${PDAL_AVS_SOURCE_ROOT}/src/retrieve/retrieve_api.cpp"
  )
  target_include_directories(pdal_avs_backend
    PUBLIC include
    PRIVATE "${PDAL_AVS_SOURCE_ROOT}/include"
  )
  target_link_libraries(pdal_avs_backend PUBLIC pdal_core SQLite::SQLite3)
  target_compile_options(pdal_avs_backend PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(pdal_http src/transport/http_server.cpp)
target_include_directories(pdal_http PUBLIC include)
target_link_libraries(pdal_http PUBLIC pdal_core Boost::system Threads::Threads)
target_compile_options(pdal_http PRIVATE -Wall -Wextra -Wpedantic)

add_executable(pdal src/main.cpp)
target_link_libraries(pdal PRIVATE pdal_core pdal_http)
if(PDAL_WITH_AVS)
  target_link_libraries(pdal PRIVATE pdal_avs_backend)
  target_compile_definitions(pdal PRIVATE PDAL_WITH_AVS=1)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(pdal_tests tests/pdal_tests.cpp)
  target_link_libraries(pdal_tests PRIVATE pdal_core)
  target_compile_options(pdal_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME pdal_tests COMMAND pdal_tests)
  set_tests_properties(pdal_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

install(TARGETS pdal RUNTIME DESTINATION bin)
install(DIRECTORY config docs web DESTINATION share/pdal)
install(DIRECTORY include/pdal DESTINATION include)
