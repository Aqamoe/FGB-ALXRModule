cmake_minimum_required(VERSION 3.24)
project(fgb_alxr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(fgb_dependency)

# SDK 先进来：Kit 在独立构建时才会自己拉 SDK。
fgb_fetch_from_lock(facegaze_sdk "${CMAKE_CURRENT_SOURCE_DIR}/sdk.lock.json")

# 本地开发可以用 -DFGB_KIT_DIR=<Kit 源码目录> 绕过归档下载；CI 走 kit.lock.json。
set(FGB_KIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(FGB_KIT_DIR)
    add_subdirectory("${FGB_KIT_DIR}" "${CMAKE_BINARY_DIR}/fgb-kit")
else()
    fgb_fetch_from_lock(fgb_kit "${CMAKE_CURRENT_SOURCE_DIR}/kit.lock.json")
endif()

list(APPEND CMAKE_MODULE_PATH "${FGB_KIT_CMAKE_DIR}")
include(FgbModule)

# enable_testing 必须在顶层工程调用，ctest 才能在 build 目录里发现用例。
enable_testing()

# ---- 产物一：Remote（TCP 客户端连串流端 IP:49192）----
# 只有这个产物传 SMOKE_SOURCE：Kit 的 smoke-test 自定义目标名是固定的，
# 两个产物都传会在第二次 fgb_add_module 时撞名。
fgb_add_module(
    NAME alxr-remote
    MAIN src/main_remote.cpp
    SOURCES src/alxr_packet.cpp src/alxr_mapper.cpp src/alxr_transport.cpp
    TEST_SOURCES
        tests/test_packet.cpp
        tests/test_mapper.cpp
        tests/test_reconnect.cpp
    SMOKE_SOURCE tests/abi_smoke.cpp
)

# ---- 产物二：Local（运行时加载 alxr_engine.dll 的 headless OpenXR 会话）----
# ABI 冒烟走普通测试 tests/test_abi_local.cpp（动态加载本仓库构建出的
# alxr-local.dll，验证导出与缺依赖时的优雅失败），原因见 README。
fgb_add_module(
    NAME alxr-local
    MAIN src/main_local.cpp
    SOURCES src/alxr_packet.cpp src/alxr_mapper.cpp src/alxr_engine.cpp
    TEST_SOURCES
        tests/test_engine_stub.cpp
        tests/test_abi_local.cpp
)
