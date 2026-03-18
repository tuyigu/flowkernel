cmake_minimum_required(VERSION 3.10)
project(robot_dataflow_core VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 确保链接 pthread 以支持无锁队列和多线程
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Find Dependencies
find_package(zenohc REQUIRED)
find_program(FLATC_EXECUTABLE flatc REQUIRED)

# Find liburing
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

# Include Directories
include_directories(include)
include_directories(${CMAKE_CURRENT_BINARY_DIR}) # For generated headers

# Generate FlatBuffers Headers
set(FBS_SRC fbs/robot_state.fbs)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/robot_state_generated.h
    COMMAND ${FLATC_EXECUTABLE} --cpp -o ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/${FBS_SRC}
    DEPENDS ${FBS_SRC}
    COMMENT "Generating FlatBuffers headers for ${FBS_SRC}"
)

# Executables
add_executable(dataflow_kernel 
    src/main.cpp 
    src/reactor.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/robot_state_generated.h
)
target_include_directories(dataflow_kernel PRIVATE ${LIBURING_INCLUDE_DIRS})
target_link_libraries(dataflow_kernel zenohc::lib ${LIBURING_LIBRARIES})

# Install
install(TARGETS dataflow_kernel DESTINATION bin)
