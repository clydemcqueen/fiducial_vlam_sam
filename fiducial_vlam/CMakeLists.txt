cmake_minimum_required(VERSION 3.5)
project(fiducial_vlam)

#=============
# Setup
#=============

# Default to C99
if (NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 99)
endif ()

# Default to C++14
if (NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 14)
endif ()

# Emulate colcon
if ($ENV{CLION_IDE})
  message("Running inside CLion")
  set(fiducial_vlam_msgs_DIR "${PROJECT_SOURCE_DIR}/../../../install/fiducial_vlam_msgs/share/fiducial_vlam_msgs/cmake")
  set(ros2_shared_DIR "${PROJECT_SOURCE_DIR}/../../../install/ros2_shared/share/ros2_shared/cmake")
  set(sim_fiducial_DIR "${PROJECT_SOURCE_DIR}/../../../install/sim_fiducial/share/sim_fiducial/cmake")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DRUN_INSIDE_CLION")
endif ()

# Try for OpenCV 4.X, but settle for whatever is installed
find_package(OpenCV 4 QUIET)
if (NOT OpenCV_FOUND)
  find_package(OpenCV REQUIRED)
endif ()
message(STATUS "Found OpenCV version ${OpenCV_VERSION}")

# Find packages
find_package(ament_cmake REQUIRED)
find_package(cv_bridge REQUIRED)
find_package(fiducial_vlam_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(GTSAM REQUIRED)
find_package(rclcpp REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(ros2_shared REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(sim_fiducial REQUIRED)
find_package(std_msgs REQUIRED)
find_package(tf2_msgs REQUIRED)
find_package(visualization_msgs REQUIRED)
find_package(yaml_cpp_vendor REQUIRED)

# Local includes
include_directories(
  include
  ${cv_bridge_INCLUDE_DIRS}
  ${fiducial_vlam_msgs_INCLUDE_DIRS}
  ${geometry_msgs_INCLUDE_DIRS}
  ${GTSAM_INCLUDE_DIR}
  ${GTSAM_INCLUDE_DIR}/gtsam/3rdparty/Eigen
  ${nav_msgs_INCLUDE_DIRS}
  ${ros2_shared_INCLUDE_DIRS}
  ${sensor_msgs_INCLUDE_DIRS}
  ${std_msgs_INCLUDE_DIRS}
  ${tf2_msgs_INCLUDE_DIRS}
  ${visualization_msgs_INCLUDE_DIRS}
  ${yaml_cpp_vendor_INCLUDE_DIRS}
)


#=============
# vloc node
#=============

add_executable(vloc_node
  src/vloc_node.cpp
  src/map.cpp
  src/convert_util.cpp
  src/transform_with_covariance.cpp
  src/fiducial_math.cpp
  src/vloc_context.cpp
  )

ament_target_dependencies(vloc_node
  cv_bridge
  fiducial_vlam_msgs
  OpenCV
  rclcpp
  nav_msgs
  ros2_shared
  sensor_msgs
  std_msgs
  tf2_msgs
  )

# ?? Why can't I put this in ament_target_dependencies
target_link_libraries(vloc_node
  gtsam
  )

# Debugging: set _dump_all_variables to true
set(_dump_all_variables false)
if (_dump_all_variables)
  get_cmake_property(_variable_names VARIABLES)
  list(SORT _variable_names)
  foreach (_variable_name ${_variable_names})
    message(STATUS "${_variable_name}=${${_variable_name}}")
  endforeach ()
endif ()

#=============
# vmap node
#=============

add_executable(vmap_node
  src/vmap_node.cpp
  src/map.cpp
  src/convert_util.cpp
  src/transform_with_covariance.cpp
  src/fiducial_math.cpp
  src/vmap_context.cpp
  )

ament_target_dependencies(vmap_node
  fiducial_vlam_msgs
  geometry_msgs
  OpenCV
  rclcpp
  ros2_shared
  std_msgs
  tf2_msgs
  visualization_msgs
  yaml_cpp_vendor
  )

# ?? Why can't I put this in ament_target_dependencies
target_link_libraries(vmap_node
  gtsam
  )

#=============
# Install
#=============

# Install targets
install(TARGETS
  vloc_node
  vmap_node
  DESTINATION lib/fiducial_vlam
  )

# Install various directories
install(DIRECTORY
  cfg
  launch
  DESTINATION share/fiducial_vlam
  )

#=============
# Run ament macros
#=============

ament_package()
