# Optional fallback target; not embedded or needed by the GUI's proxy mode.
add_library(zhcn SHARED zhcn.cpp mem_patch.h dict_gen.inc)
forge_target(zhcn)
target_include_directories(zhcn PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(zhcn PROPERTIES OUTPUT_NAME zhcn PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/resources"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/resources")
target_link_libraries(zhcn PRIVATE user32)
