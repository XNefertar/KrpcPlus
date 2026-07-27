#获取客户端的源文件
set(Client_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/client.cc)
set(BenchClient_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/bench_client.cc)

#获取protobuf生成的.cc
file(GLOB PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../*.pb.cc)

#创建客户端可执行文件 (ZK 模式)
add_executable(client ${Client_SRCS} ${PROTO_SRCS})
target_link_libraries(client xrpc_core ${CLIENT_LIBS})
target_compile_options(client PRIVATE -std=c++17 -Wall)
set_target_properties(client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)

#创建直连压测客户端 (无 ZK 依赖)
add_executable(bench_client ${BenchClient_SRCS} ${PROTO_SRCS})
target_link_libraries(bench_client xrpc_core ${CLIENT_LIBS})
target_compile_options(bench_client PRIVATE -std=c++17 -Wall)
set_target_properties(bench_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
