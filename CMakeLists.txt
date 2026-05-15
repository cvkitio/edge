cmake_minimum_required(VERSION 3.22)

project(emd
    VERSION 1.0.0
    DESCRIPTION "Edge Motion Detector"
    LANGUAGES C
)

# ---------------------------------------------------------------------------
# C standard
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Build types
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Compiler warnings / hardening
# ---------------------------------------------------------------------------
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wconversion -Wshadow -Wformat=2
    -Werror
)

# Hardening flags (security)
if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
    add_compile_options(
        -D_FORTIFY_SOURCE=2
        -fstack-protector-strong
    )
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
        add_compile_options(-fstack-clash-protection -fcf-protection=full)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        add_compile_options(-mbranch-protection=standard)
    endif()
endif()

# Debug: no optimisation, full debug info
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g3)
endif()

# LTO for Release
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
    if(_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# ---------------------------------------------------------------------------
# Version info (exposed to code via compile definitions)
# ---------------------------------------------------------------------------
set(EMD_VERSION_STRING "${PROJECT_VERSION}")
# Allow override from environment (CI sets this from git describe)
if(DEFINED ENV{EMD_GIT_COMMIT})
    set(EMD_GIT_COMMIT "$ENV{EMD_GIT_COMMIT}")
else()
    set(EMD_GIT_COMMIT "unknown")
endif()

add_compile_definitions(
    EMD_VERSION="${EMD_VERSION_STRING}"
    EMD_GIT_COMMIT="${EMD_GIT_COMMIT}"
)

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/mqtt-c/include
    ${CMAKE_SOURCE_DIR}/third_party/tomlc99
)

# ---------------------------------------------------------------------------
# Third-party: tomlc99
# ---------------------------------------------------------------------------
add_library(tomlc99 STATIC
    third_party/tomlc99/toml.c
)
target_include_directories(tomlc99 PUBLIC third_party/tomlc99)
# Vendored code: relax warnings
target_compile_options(tomlc99 PRIVATE -w)

# ---------------------------------------------------------------------------
# Third-party: mqtt-c (vendored stub)
# ---------------------------------------------------------------------------
add_library(mqtt_c STATIC
    third_party/mqtt-c/mqtt.c
    third_party/mqtt-c/mqtt_pal.c
)
target_include_directories(mqtt_c PUBLIC third_party/mqtt-c/include)
target_compile_options(mqtt_c PRIVATE -w)

# ---------------------------------------------------------------------------
# Core library (all emd modules except main)
# ---------------------------------------------------------------------------
set(EMD_CORE_SOURCES
    src/emd_config.c
    src/emd_log.c
    src/emd_metrics.c
    src/emd_net.c
    src/emd_rtsp.c
    src/emd_rtp.c
    src/emd_h264_depay.c
    src/emd_h265_depay.c
    src/emd_h264_parse.c
    src/emd_h265_parse.c
    src/emd_inspector.c
    src/emd_ringbuf.c
    src/emd_event.c
    src/emd_mux_mpegts.c
    src/emd_mux_fmp4.c
    src/emd_recorder.c
    src/emd_mqtt.c
    src/emd_supervisor.c
)

add_library(emd_core STATIC ${EMD_CORE_SOURCES})
target_link_libraries(emd_core PUBLIC tomlc99 mqtt_c m pthread)
target_include_directories(emd_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/mqtt-c/include
    ${CMAKE_SOURCE_DIR}/third_party/tomlc99
)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(emd src/main.c)
target_link_libraries(emd PRIVATE emd_core)

# Static PIE
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(emd PRIVATE -static-pie)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS emd DESTINATION bin)
install(FILES
    ${CMAKE_SOURCE_DIR}/include/emd/config.h
    ${CMAKE_SOURCE_DIR}/include/emd/log.h
    ${CMAKE_SOURCE_DIR}/include/emd/metrics.h
    ${CMAKE_SOURCE_DIR}/include/emd/net.h
    ${CMAKE_SOURCE_DIR}/include/emd/rtsp.h
    ${CMAKE_SOURCE_DIR}/include/emd/rtp.h
    ${CMAKE_SOURCE_DIR}/include/emd/h264_depay.h
    ${CMAKE_SOURCE_DIR}/include/emd/h265_depay.h
    ${CMAKE_SOURCE_DIR}/include/emd/h264_parse.h
    ${CMAKE_SOURCE_DIR}/include/emd/h265_parse.h
    ${CMAKE_SOURCE_DIR}/include/emd/inspector.h
    ${CMAKE_SOURCE_DIR}/include/emd/ringbuf.h
    ${CMAKE_SOURCE_DIR}/include/emd/event.h
    ${CMAKE_SOURCE_DIR}/include/emd/recorder.h
    ${CMAKE_SOURCE_DIR}/include/emd/mqtt.h
    ${CMAKE_SOURCE_DIR}/include/emd/supervisor.h
    DESTINATION include/emd
)
