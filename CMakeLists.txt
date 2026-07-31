cmake_minimum_required(VERSION 3.21)
project(MHW16x10Fix VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "MHW16x10Fix targets 64-bit Windows/Proton.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Configure a 64-bit toolchain.")
endif()

add_library(MHW16x10Fix SHARED
    src/config.cpp
    src/dinput8_proxy.cpp
    src/dllmain.cpp
    src/fix.cpp
    src/log.cpp
    src/native_aspect_fix.cpp
)

target_compile_features(MHW16x10Fix PRIVATE cxx_std_20)
target_compile_definitions(MHW16x10Fix PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_compile_options(MHW16x10Fix PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc>
)
target_link_libraries(MHW16x10Fix PRIVATE d3d11 d3d12 dxgi)
set_target_properties(MHW16x10Fix PROPERTIES
    OUTPUT_NAME dinput8
    PREFIX ""
)

add_custom_command(TARGET MHW16x10Fix POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/mhw_16x10.ini"
        "$<TARGET_FILE_DIR:MHW16x10Fix>/mhw_16x10.ini"
)
