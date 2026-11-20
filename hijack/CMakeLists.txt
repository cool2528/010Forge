cmake_minimum_required(VERSION 3.20)
# quazip DLL 代理劫持：伪装成 quazip.dll，转发全部导出到 quazip_orig.dll，
# 同时在 DllMain 里执行汉化 hook（复用 zhcn 的逻辑）。
project(quazip_hijack LANGUAGES CXX)

add_library(quazip_hijack SHARED
    hijack.cpp
    mem_patch.h
    dict_gen.inc
    quazip.def
)
target_include_directories(quazip_hijack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(quazip_hijack PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(quazip_hijack PRIVATE /utf-8 /O2 /W3)
    target_compile_definitions(quazip_hijack PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()

# 输出名就是 quazip.dll（劫持目标），直接输出到本目录供部署脚本使用
set(OUT "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(quazip_hijack PROPERTIES
    OUTPUT_NAME "quazip"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${OUT}"
    LIBRARY_OUTPUT_DIRECTORY "${OUT}"
)
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(quazip_hijack PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${OUT}"
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${OUT}"
    )
endforeach()

target_link_libraries(quazip_hijack PRIVATE user32)
