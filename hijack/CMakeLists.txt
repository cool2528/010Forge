# A proxy and a standalone DLL share exactly the same localization code.
add_library(quazip_hijack SHARED ../zhcn/zhcn.cpp ../zhcn/mem_patch.h
    ../zhcn/dict_gen.inc quazip.def)
forge_target(quazip_hijack)
target_include_directories(quazip_hijack PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../zhcn")
set_target_properties(quazip_hijack PROPERTIES OUTPUT_NAME quazip PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/resources"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/resources"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
target_link_libraries(quazip_hijack PRIVATE user32)
