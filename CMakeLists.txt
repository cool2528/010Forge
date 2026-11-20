cmake_minimum_required(VERSION 3.20)
# 010Forge - 010 Editor 一键补丁工具（激活 + 汉化）
project(010Forge LANGUAGES CXX RC)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# 汉化 DLL 子项目：构建产物输出到 resources/，作为本 exe 的嵌入资源
add_subdirectory(zhcn)

add_executable(010Forge WIN32
    main.cpp
    license.cpp
    license.h
    lic_table.h
    resource.rc
    resource.h
)
# 保证先构建 zhcn.dll，再编译资源脚本（RC 需要读取 resources/zhcn.dll）
add_dependencies(010Forge zhcn)

target_include_directories(010Forge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(010Forge PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(010Forge PRIVATE /utf-8 /W3 /O2)
    target_compile_definitions(010Forge PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
    target_compile_options(010Forge PRIVATE -Wall -O2)
endif()

target_link_libraries(010Forge PRIVATE
    user32 gdi32 comctl32 advapi32 shell32 winmm
)

set_target_properties(010Forge PROPERTIES OUTPUT_NAME "010Forge")
