add_definitions(-DTHREADED)

# 搜集所有子目录下的源文件
file(GLOB CODEC_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/codec/*.cc)
file(GLOB COMMON_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/common/*.cc)
file(GLOB MONITOR_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/monitor/*.cc)
file(GLOB PROTOCOL_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/protocol/*.cc)
file(GLOB REGISTRY_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/registry/*.cc)

# RPC 源文件拆分为客户端和服务端
file(GLOB RPC_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/rpc/*.cc)
# 将 server.cc 从客户端核心库中排除（server.cc 依赖 muduo）
list(REMOVE_ITEM RPC_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/rpc/server.cc)
set(SERVER_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/rpc/server.cc)

# 将所有客户端源文件合并到 SRC_FILES
set(SRC_FILES
    ${CODEC_SRCS}
    ${COMMON_SRCS}
    ${MONITOR_SRCS}
    ${PROTOCOL_SRCS}
    ${REGISTRY_SRCS}
    ${RPC_SRCS}
)

# 获取 protobuf 的生成文件
file(GLOB PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/protocol/*.pb.cc)

# 创建客户端核心静态库 (不依赖 muduo)
add_library(xrpc_core STATIC ${SRC_FILES})
target_link_libraries(xrpc_core PUBLIC protobuf)
target_include_directories(xrpc_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/xrpc/codec
    ${CMAKE_CURRENT_SOURCE_DIR}/include/xrpc/common
    ${CMAKE_CURRENT_SOURCE_DIR}/include/xrpc/monitor
    ${CMAKE_CURRENT_SOURCE_DIR}/include/xrpc/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/include/xrpc/registry
    ${CMAKE_CURRENT_SOURCE_DIR}/include/xrpc/rpc
)
target_compile_options(xrpc_core PRIVATE -std=c++17 -Wall)

# 创建服务端核心静态库 (依赖 muduo)
add_library(xrpc_server_core STATIC ${SERVER_SRCS})
target_link_libraries(xrpc_server_core PUBLIC xrpc_core muduo_net muduo_base)
target_include_directories(xrpc_server_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/xrpc/rpc
)
target_compile_options(xrpc_server_core PRIVATE -std=c++17 -Wall)
