cmake_minimum_required(VERSION 3.20)
project(pdal VERSION 0.1.0 LANGUAGES C CXX)

option(PDAL_WITH_AVS "Build the unified AVS historical storage adapter" ON)
option(PDAL_WITH_ROS_LIVE "Build the ROS 2 live-data source when dependencies are available" ON)
option(PDAL_WITH_PRIVACY "Build the camera privacy (human blur) stage (needs OpenCV)" ON)
option(PDAL_BUILD_BENCHMARKS "Build pDAL performance benchmarks" ON)
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
  src/model/data.cpp
  src/catalog/resource_catalog.cpp
  src/policy/policy_engine.cpp
  src/planner/query_planner.cpp
  src/storage/storage_backend.cpp
  src/live/live_data_source.cpp
  src/representation/representation_provider.cpp
  src/adapter/request_translator.cpp
  src/audit/audit.cpp
  src/security/authenticator.cpp
  src/security/engine_policy_hook.cpp
  src/privacy/fail_closed_stage.cpp
  src/pipeline.cpp
  src/query_engine.cpp
  src/sdk/client.cpp
)
target_include_directories(pdal_core PUBLIC include)
target_link_libraries(pdal_core
  PUBLIC Boost::json yaml-cpp OpenSSL::Crypto Threads::Threads
)
target_compile_options(pdal_core PRIVATE -Wall -Wextra -Wpedantic)

set(PDAL_PRIVACY_AVAILABLE OFF)
set(PDAL_ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime"
    CACHE PATH "ONNX Runtime prebuilt (headers + lib) for the privacy stage")
if(PDAL_WITH_PRIVACY)
  find_package(OpenCV QUIET COMPONENTS core imgproc imgcodecs)
  find_library(ORT_LIB onnxruntime PATHS "${PDAL_ONNXRUNTIME_ROOT}/lib" NO_DEFAULT_PATH)
  find_path(ORT_INCLUDE onnxruntime_cxx_api.h
            PATHS "${PDAL_ONNXRUNTIME_ROOT}/include" NO_DEFAULT_PATH)
  if(OpenCV_FOUND AND ORT_LIB AND ORT_INCLUDE)
    add_library(pdal_privacy src/privacy/human_blur.cpp)
    target_include_directories(pdal_privacy PUBLIC include
      PRIVATE ${OpenCV_INCLUDE_DIRS} "${ORT_INCLUDE}")
    target_link_libraries(pdal_privacy PUBLIC pdal_core PRIVATE ${OpenCV_LIBS} "${ORT_LIB}")
    target_compile_definitions(pdal_privacy PUBLIC PDAL_WITH_PRIVACY=1)
    target_compile_options(pdal_privacy PRIVATE -Wall -Wextra -Wpedantic)
    get_filename_component(_ort_libdir "${ORT_LIB}" DIRECTORY)
    set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH};${_ort_libdir}")
    set(PDAL_PRIVACY_AVAILABLE ON)
  else()
    message(WARNING "PDAL_WITH_PRIVACY=ON but OpenCV (core/imgproc/imgcodecs) or "
                    "ONNX Runtime under PDAL_ONNXRUNTIME_ROOT=${PDAL_ONNXRUNTIME_ROOT} "
                    "was not found. Run scripts/fetch_privacy_deps.sh, or set "
                    "PDAL_WITH_PRIVACY=OFF. Camera requests will fail closed.")
  endif()
endif()

if(PDAL_WITH_AVS)
  if(NOT EXISTS "${PDAL_AVS_SOURCE_ROOT}/include/avs/retrieve_api.h" OR
     NOT EXISTS "${PDAL_AVS_SOURCE_ROOT}/src/retrieve/retrieve_api.cpp")
    message(FATAL_ERROR
      "PDAL_WITH_AVS=ON but RetrieveAPI was not found under PDAL_AVS_SOURCE_ROOT=${PDAL_AVS_SOURCE_ROOT}")
  endif()
  add_library(pdal_avs_backend
    src/storage/avs_storage_backend.cpp
    "${PDAL_AVS_SOURCE_ROOT}/src/retrieve/retrieve_api.cpp"
    "${PDAL_AVS_SOURCE_ROOT}/src/retrieve/historical_retrieve_api.cpp"
  )
  target_include_directories(pdal_avs_backend
    PUBLIC include
    PRIVATE "${PDAL_AVS_SOURCE_ROOT}/include"
  )
  target_link_libraries(pdal_avs_backend PUBLIC pdal_core SQLite::SQLite3)
  target_compile_options(pdal_avs_backend PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PDAL_BUILD_BENCHMARKS AND PDAL_WITH_AVS)
  add_executable(pdal_avs_benchmark benchmarks/pdal_avs_benchmark.cpp)
  target_include_directories(pdal_avs_benchmark PRIVATE
    "${PDAL_AVS_SOURCE_ROOT}/include")
  target_link_libraries(pdal_avs_benchmark PRIVATE pdal_avs_backend)
  target_compile_options(pdal_avs_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(PDAL_ROS_LIVE_AVAILABLE OFF)
if(PDAL_WITH_ROS_LIVE)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  find_package(ament_cmake QUIET)
  find_package(rclcpp QUIET)
  find_package(sensor_msgs QUIET)
  find_package(gps_msgs QUIET)
  find_package(cv_bridge QUIET)
  find_package(pcl_conversions QUIET)
  find_package(OpenCV QUIET COMPONENTS core imgcodecs)
  find_package(PCL QUIET COMPONENTS common)
  find_path(LASZIP_INCLUDE_DIR NAMES laszip_api.h PATH_SUFFIXES laszip)
  find_library(LASZIP_LIBRARY NAMES laszip)
  if(ament_cmake_FOUND AND rclcpp_FOUND AND sensor_msgs_FOUND AND
     gps_msgs_FOUND AND cv_bridge_FOUND AND pcl_conversions_FOUND AND
     OpenCV_FOUND AND PCL_FOUND AND LASZIP_INCLUDE_DIR AND LASZIP_LIBRARY)
    add_library(pdal_ros_live
      src/live/ros_live_data_source.cpp
      "${PDAL_AVS_SOURCE_ROOT}/src/process/lidar_compress.cpp"
    )
    target_include_directories(pdal_ros_live
      PUBLIC include
      PRIVATE
        "${PDAL_AVS_SOURCE_ROOT}/include"
        ${OpenCV_INCLUDE_DIRS}
        ${PCL_INCLUDE_DIRS}
        ${LASZIP_INCLUDE_DIR}
    )
    ament_target_dependencies(pdal_ros_live
      rclcpp sensor_msgs gps_msgs cv_bridge pcl_conversions)
    target_link_libraries(pdal_ros_live
      pdal_core ${OpenCV_LIBS} ${PCL_LIBRARIES} ${LASZIP_LIBRARY} yaml-cpp)
    target_compile_options(pdal_ros_live PRIVATE -Wall -Wextra -Wpedantic)
    if(PDAL_BUILD_BENCHMARKS)
      add_executable(pdal_ros_live_benchmark
        benchmarks/pdal_ros_live_benchmark.cpp)
      target_link_libraries(pdal_ros_live_benchmark
        pdal_ros_live pdal_core)
      target_compile_options(pdal_ros_live_benchmark PRIVATE
        -Wall -Wextra -Wpedantic)
    endif()
    set(PDAL_ROS_LIVE_AVAILABLE ON)
  else()
    message(WARNING "ROS live dependencies were not found; building pDAL without RosLiveDataSource")
  endif()
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
if(PDAL_ROS_LIVE_AVAILABLE)
  target_link_libraries(pdal PRIVATE pdal_ros_live)
  target_compile_definitions(pdal PRIVATE PDAL_WITH_ROS_LIVE=1)
endif()
if(PDAL_PRIVACY_AVAILABLE)
  target_link_libraries(pdal PRIVATE pdal_privacy)
  target_compile_definitions(pdal PRIVATE PDAL_WITH_PRIVACY=1)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(pdal_tests tests/pdal_tests.cpp)
  target_link_libraries(pdal_tests PRIVATE pdal_core)
  target_compile_options(pdal_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME pdal_tests COMMAND pdal_tests)
  set_tests_properties(pdal_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

  add_executable(pdal_security_tests tests/security_tests.cpp)
  target_link_libraries(pdal_security_tests PRIVATE pdal_core OpenSSL::Crypto)
  target_compile_options(pdal_security_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME pdal_security_tests COMMAND pdal_security_tests)
  set_tests_properties(pdal_security_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

  if(PDAL_PRIVACY_AVAILABLE)
    add_executable(pdal_privacy_tests tests/privacy_tests.cpp)
    target_link_libraries(pdal_privacy_tests PRIVATE pdal_privacy)
    target_compile_options(pdal_privacy_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME pdal_privacy_tests COMMAND pdal_privacy_tests)
    set_tests_properties(pdal_privacy_tests PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()

  if(PDAL_WITH_AVS)
    add_test(NAME pdal_http_auth_smoke
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/http_auth_smoke.sh"
              "$<TARGET_FILE:pdal>" "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  if(PDAL_WITH_AVS)
    add_executable(pdal_avs_backend_tests tests/avs_backend_tests.cpp)
    target_include_directories(pdal_avs_backend_tests PRIVATE
      "${PDAL_AVS_SOURCE_ROOT}/include")
    target_link_libraries(pdal_avs_backend_tests PRIVATE pdal_avs_backend)
    target_compile_options(pdal_avs_backend_tests PRIVATE
      -Wall -Wextra -Wpedantic)
    add_test(NAME pdal_avs_backend_tests COMMAND pdal_avs_backend_tests)
  endif()
endif()

install(TARGETS pdal RUNTIME DESTINATION bin)
install(DIRECTORY config docs web DESTINATION share/pdal)
install(DIRECTORY include/pdal DESTINATION include)
