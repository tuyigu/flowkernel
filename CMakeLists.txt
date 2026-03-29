cmake_minimum_required(VERSION 3.14)
project(robot_dataflow_core VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 确保链接 pthread 以支持无锁队列和多线程
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Find Dependencies
find_package(zenohc REQUIRED)
find_program(FLATC_EXECUTABLE flatc REQUIRED)

# Find liburing and spdlog
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)
pkg_check_modules(SPDLOG   REQUIRED spdlog)

# ── FetchContent for ftxui and yaml-cpp ─────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v5.0.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE
)

set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(ftxui yaml-cpp googletest)

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
    src/config.cpp
    src/tui/tui_app.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/robot_state_generated.h
)
target_include_directories(dataflow_kernel PRIVATE ${LIBURING_INCLUDE_DIRS} ${SPDLOG_INCLUDE_DIRS})
target_link_libraries(dataflow_kernel 
    zenohc::lib 
    ${LIBURING_LIBRARIES} 
    ${SPDLOG_LIBRARIES}
    ftxui::screen 
    ftxui::dom 
    ftxui::component
    yaml-cpp::yaml-cpp
)
target_compile_options(dataflow_kernel PRIVATE -Wno-interference-size ${SPDLOG_CFLAGS_OTHER})

# Install
install(TARGETS dataflow_kernel DESTINATION bin)

# ── 基准测试（无需 Zenoh，仅依赖 spdlog + pthread）──────────────────────────
add_executable(bench test/bench.cpp)
target_include_directories(bench PRIVATE include ${SPDLOG_INCLUDE_DIRS})
target_link_libraries(bench Threads::Threads ${SPDLOG_LIBRARIES})
target_compile_options(bench PRIVATE
    -O2 -march=native -Wno-interference-size ${SPDLOG_CFLAGS_OTHER})

# ── 单元测试 ──────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_mpsc_queue test/test_mpsc_queue.cpp)
target_include_directories(test_mpsc_queue PRIVATE include)
target_link_libraries(test_mpsc_queue GTest::gtest_main Threads::Threads)
target_compile_options(test_mpsc_queue PRIVATE -Wno-interference-size)
add_test(NAME MPSCQueueTest COMMAND test_mpsc_queue)

add_executable(test_latest_cache test/test_latest_cache.cpp)
target_include_directories(test_latest_cache PRIVATE include)
target_link_libraries(test_latest_cache GTest::gtest_main Threads::Threads)
target_compile_options(test_latest_cache PRIVATE -Wno-interference-size)
add_test(NAME LatestCacheTest COMMAND test_latest_cache)

add_executable(test_config test/test_config.cpp src/config.cpp)
target_include_directories(test_config PRIVATE include ${SPDLOG_INCLUDE_DIRS})
target_link_libraries(test_config GTest::gtest_main yaml-cpp::yaml-cpp ${SPDLOG_LIBRARIES})
target_compile_options(test_config PRIVATE -Wno-interference-size ${SPDLOG_CFLAGS_OTHER})
add_test(NAME ConfigTest COMMAND test_config)
