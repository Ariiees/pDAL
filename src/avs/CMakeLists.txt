cmake_minimum_required(VERSION 3.10)
project(avs)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(cv_bridge REQUIRED)
find_package(OpenCV REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(SQLITE3 REQUIRED sqlite3)

add_library(avs SHARED
  src/logger/append_logger.cpp
  src/logger/trip_manager.cpp
  src/logger/topic_map.cpp
  src/image/img_compress.cpp
  src/image/img_dedup.cpp
)

target_include_directories(avs
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${SQLITE3_INCLUDE_DIRS}
)

target_link_libraries(avs
  PUBLIC
    ${OpenCV_LIBS}
    yaml-cpp
    ${SQLITE3_LIBRARIES}
)

ament_target_dependencies(avs
  PUBLIC
    rclcpp
    sensor_msgs
    cv_bridge
    OpenCV
    yaml-cpp
)

ament_export_include_directories(include)
ament_export_libraries(avs)
ament_export_dependencies(rclcpp sensor_msgs cv_bridge OpenCV yaml-cpp)

install(TARGETS avs
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(DIRECTORY include/
  DESTINATION include
)

ament_package()

