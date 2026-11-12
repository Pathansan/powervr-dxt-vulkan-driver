cmake_minimum_required(VERSION 3.22)
project(powervr_dxt_vulkan_driver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(vulkan_powervr_compat SHARED
    src/log.cpp
    src/icd_main.cpp
)

target_include_directories(vulkan_powervr_compat PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(vulkan_powervr_compat PRIVATE
    -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden
)

target_link_libraries(vulkan_powervr_compat PRIVATE
    log
    dl
)

set_target_properties(vulkan_powervr_compat PROPERTIES
    OUTPUT_NAME "vulkan.powervr_compat"
    LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/icd.map"
)
