add_definitions(-DTHREADED)

# 搜集所有子目录下的源文件
file(GLOB COMMON_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/common/*.cc)
file(GLOB MONITOR_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/monitor/*.cc)
file(GLOB PROTOCOL_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/protocol/*.cc)
file(GLOB REGISTRY_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/registry/*.cc)
file(GLOB RPC_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/rpc/*.cc)

# 将所有源文件合并到 SRC_FILES
set(SRC_FILES
    ${COMMON_SRCS}
    ${MONITOR_SRCS}
    ${PROTOCOL_SRCS}
    ${REGISTRY_SRCS}
    ${RPC_SRCS}
)

# 获取 protobuf 的生成文件（可选，如果已经包含在上面的 glob 中）
file(GLOB PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/protocol/*.pb.cc)

# 创建静态库
add_library(krpc_core STATIC ${SRC_FILES})

# 链接 protobuf 库
target_link_libraries(krpc_core PUBLIC protobuf)

# 设置头文件的路径
target_include_directories(krpc_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/krpc/common
    ${CMAKE_CURRENT_SOURCE_DIR}/include/krpc/monitor
    ${CMAKE_CURRENT_SOURCE_DIR}/include/krpc/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/include/krpc/registry
    ${CMAKE_CURRENT_SOURCE_DIR}/include/krpc/rpc
)

# 添加编译选项
target_compile_options(krpc_core PRIVATE -std=c++17 -Wall)
