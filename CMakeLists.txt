# 设置最低版本和项目名称
cmake_minimum_required(VERSION 3.5)
project(xrpc)

# 设置全局的 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 查找 protobuf 包
find_package(Protobuf REQUIRED)
find_package(absl CONFIG QUIET)
include_directories(${Protobuf_INCLUDE_DIRS})
find_library(ZOOKEEPER_MT_LIBRARY NAMES zookeeper_mt)

# 设置全局链接库
set(CLIENT_LIBS
    protobuf
    pthread
    zookeeper_mt
)

set(SERVER_LIBS
    protobuf
    pthread
    zookeeper_mt
    muduo_net
    muduo_base
    glog
)

# 添加子目录
add_subdirectory(src)
add_subdirectory(example)

include(CTest)
if(BUILD_TESTING)
  add_executable(async_multiplex_test
      tools/async_multiplex_test.cc
      example/user.pb.cc)
  target_include_directories(async_multiplex_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(async_multiplex_test PRIVATE
      xrpc_core
      protobuf::libprotobuf
      pthread
      ${ZOOKEEPER_MT_LIBRARY})
  add_test(NAME async_multiplex_test COMMAND async_multiplex_test)

  add_executable(async_qps_benchmark
      tools/async_qps_benchmark.cc
      example/user.pb.cc)
  target_include_directories(async_qps_benchmark PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(async_qps_benchmark PRIVATE
      xrpc_core
      protobuf::libprotobuf
      pthread
      ${ZOOKEEPER_MT_LIBRARY})
endif()
