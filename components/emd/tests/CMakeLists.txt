# tests/CMakeLists.txt — emd unit test build definitions.
#
# Each test is a standalone executable linked against emd_core.
# cmocka: prefer system installation; fall back to vendored stub.

cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# cmocka library target
# ---------------------------------------------------------------------------
find_package(cmocka QUIET)

if(cmocka_FOUND)
    set(CMOCKA_LIBS cmocka)
    set(CMOCKA_INCLUDE_DIRS "")
else()
    # Use vendored header-only stub
    message(STATUS "cmocka not found — using vendored stub")
    add_library(cmocka_stub INTERFACE)
    target_include_directories(cmocka_stub INTERFACE
        ${CMAKE_SOURCE_DIR}/third_party/cmocka/include
    )
    set(CMOCKA_LIBS cmocka_stub)
    set(CMOCKA_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/cmocka/include)
endif()

# ---------------------------------------------------------------------------
# Helper macro: add a unit test executable
# ---------------------------------------------------------------------------
macro(emd_add_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE emd_core ${CMOCKA_LIBS})
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/tomlc99
        ${CMAKE_SOURCE_DIR}/third_party/mqtt-c/include
        ${CMOCKA_INCLUDE_DIRS}
    )
    # Tests contain vendored includes; relax some pedantic warnings
    target_compile_options(${name} PRIVATE
        -Wno-unused-parameter
        -Wno-missing-field-initializers
    )
    add_test(NAME ${name} COMMAND ${name})
endmacro()

# ---------------------------------------------------------------------------
# Test executables
# ---------------------------------------------------------------------------

# H.264 bitstream parser
emd_add_test(test_h264_parse)

# H.264 RTP depacketizer
emd_add_test(test_h264_depay)

# H.265 bitstream parser
emd_add_test(test_h265_parse)

# H.265 RTP depacketizer
emd_add_test(test_h265_depay)

# Motion inspector — basic motion detection
emd_add_test(test_inspector_motion)

# Motion inspector — gradual scene change
emd_add_test(test_inspector_gradual)

# Ring buffer (SPSC NAL store)
emd_add_test(test_ringbuf)
target_link_libraries(test_ringbuf PRIVATE pthread)

# MPMC event bus
emd_add_test(test_event_bus)
target_link_libraries(test_event_bus PRIVATE pthread)

# fMP4 / MPEG-TS muxer output
emd_add_test(test_recorder_mp4)

# MQTT wire protocol + topic/payload helpers
emd_add_test(test_mqtt)

# TOML parser + config loader
emd_add_test(test_config)
