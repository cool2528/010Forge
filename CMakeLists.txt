cmake_minimum_required(VERSION 3.21)
project(010Forge VERSION 4.1.0 LANGUAGES CXX RC)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "010Forge requires a Windows x64 toolchain.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# Multi-config generators retain Debug/Release subdirectories.
function(forge_target target)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/W4>)
    endif()
endfunction()
option(FORGE_BUILD_STANDALONE_ZHCN "Build the optional non-proxy zhcn.dll" OFF)
if(FORGE_BUILD_STANDALONE_ZHCN)
    add_subdirectory(zhcn)
endif()
add_subdirectory(hijack)

configure_file(version.h.in "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)
# The custom-command file dependency is intentional: a changed DLL, icon or
# WAV must regenerate the RC even with Visual Studio's resource compiler.
set(FORGE_GENERATED_RC "${CMAKE_BINARY_DIR}/generated/$<CONFIG>/resource.rc")
add_custom_command(
    OUTPUT "${FORGE_GENERATED_RC}"
    COMMAND "${CMAKE_COMMAND}"
        "-DOUTPUT=${FORGE_GENERATED_RC}"
        "-DPROXY=$<TARGET_FILE:quazip_hijack>"
        "-DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/resource.rc"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateResource.cmake"
    DEPENDS quazip_hijack resource.rc resource.h cmake/GenerateResource.cmake
        resources/app.ico resources/bgm.wav resources/app.manifest
    COMMENT "Embedding the current quazip proxy, icon and BGM"
    VERBATIM
)
add_library(forge_deployment STATIC deployment.cpp deployment.h)
forge_target(forge_deployment)
target_include_directories(forge_deployment PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(forge_deployment PUBLIC bcrypt)
add_executable(010Forge WIN32 main.cpp license.cpp license.h lic_table.h ui_text.h
    tracker_theme.h resource.h "${FORGE_GENERATED_RC}")
forge_target(010Forge)
add_dependencies(010Forge quazip_hijack)
target_include_directories(010Forge PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(010Forge PRIVATE forge_deployment user32 gdi32 comctl32 advapi32 shell32 winmm uxtheme dwmapi)
if(MSVC)
    target_link_options(010Forge PRIVATE /MANIFEST:NO)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(forge_deployment_tests tests/test_deployment.cpp)
    forge_target(forge_deployment_tests)
    target_link_libraries(forge_deployment_tests PRIVATE forge_deployment)
    add_test(NAME deployment_transaction COMMAND forge_deployment_tests)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-tmp")
    set_tests_properties(deployment_transaction PROPERTIES ENVIRONMENT
        "TEMP=${CMAKE_BINARY_DIR}/test-tmp;TMP=${CMAKE_BINARY_DIR}/test-tmp")
    add_executable(forge_resource_tests tests/test_resources.cpp license.cpp)
    forge_target(forge_resource_tests)
    target_link_libraries(forge_resource_tests PRIVATE forge_deployment)
    add_dependencies(forge_resource_tests 010Forge)
    add_test(NAME embedded_resources COMMAND forge_resource_tests "$<TARGET_FILE:010Forge>" "$<TARGET_FILE:quazip_hijack>")
    add_executable(forge_ui_tests tests/test_ui.cpp)
    forge_target(forge_ui_tests)
    target_include_directories(forge_ui_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(forge_ui_tests PRIVATE user32)
    add_test(NAME ui_language_mapping COMMAND forge_ui_tests)
endif()
option(FORGE_BUILD_UI_CAPTURE "Build non-interactive desktop documentation capture utility" OFF)
if(FORGE_BUILD_UI_CAPTURE)
    add_executable(forge_capture_ui tools/capture_ui.cpp)
    forge_target(forge_capture_ui)
    target_link_libraries(forge_capture_ui PRIVATE user32 gdi32)
endif()
install(TARGETS 010Forge RUNTIME DESTINATION .)
install(FILES README.md README.en.md CHANGELOG.md DESTINATION .)
install(DIRECTORY images DESTINATION .)
