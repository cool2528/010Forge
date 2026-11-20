cmake_minimum_required(VERSION 3.20)
# zhcn - 010 Editor 汉化 hook DLL（010Forge 的子项目）
# 产物直接输出到上级 resources/，供 010Forge 作为嵌入资源
project(zhcn LANGUAGES CXX)

add_library(zhcn SHARED zhcn.cpp mem_patch.h dict_gen.inc)
target_include_directories(zhcn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(zhcn PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(zhcn PRIVATE /utf-8 /O2 /W3)
    target_compile_definitions(zhcn PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()

# 关键：显式指定输出目录到上级 resources/（DLL 在 Windows 上算 RUNTIME 产物）
set(OUT "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
get_filename_component(OUT "${OUT}" ABSOLUTE)
file(MAKE_DIRECTORY "${OUT}")
set_target_properties(zhcn PROPERTIES
    OUTPUT_NAME "zhcn"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${OUT}"
    LIBRARY_OUTPUT_DIRECTORY "${OUT}"
)
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(zhcn PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${OUT}"
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${OUT}"
    )
endforeach()

target_link_libraries(zhcn PRIVATE user32)
