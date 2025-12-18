cmake_minimum_required(VERSION 3.10)
project(bring_up)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(cv_bridge REQUIRED)

find_package(avs REQUIRED)
find_package(pacc REQUIRED)

add_executable(ingest
  src/ingest_main.cpp
)

ament_target_dependencies(ingest
  rclcpp
  sensor_msgs
  cv_bridge
  avs
  pacc
)

target_link_libraries(ingest yaml-cpp)


# add_executable(retrieve
#   src/retrieve_main.cpp
# )
# target_link_libraries(retrieve
#   PRIVATE
#     avs
#     pacc
# )
# ament_target_dependencies(retrieve
#   rclcpp
#   sensor_msgs
#   cv_bridge
#   avs
#   pacc
# )

install(TARGETS
  ingest
  # retrieve
  DESTINATION lib/${PROJECT_NAME}
)

# install(DIRECTORY launch/
#   DESTINATION share/${PROJECT_NAME}
# )

# install(DIRECTORY config/
#   DESTINATION share/${PROJECT_NAME}
# )

ament_package()
