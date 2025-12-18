cmake_minimum_required(VERSION 3.10)
project(pacc)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(pacc SHARED
  src/encrypto.cpp
)

target_include_directories(pacc
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(pacc
  PUBLIC
    OpenSSL::Crypto
)

ament_export_include_directories(include)
ament_export_libraries(pacc)
ament_export_dependencies(OpenSSL)

install(TARGETS pacc
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(DIRECTORY include/
  DESTINATION include
)

ament_package()

