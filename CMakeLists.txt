cmake_minimum_required(VERSION 3.16)

project(tlvdemux VERSION 0.1.7 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(BUILD_SHARED_LIBS "Build libtlvdemux as a shared library" ON)
option(TLVDEMUX_BUILD_TOOLS "Build tlvdemux command-line tools" ON)
option(TLVDEMUX_BUILD_WASM_BINDINGS "Build the Emscripten JavaScript/WASM wrapper" ${EMSCRIPTEN})

if(NOT EMSCRIPTEN)
    find_package(ZLIB REQUIRED)
endif()

add_library(tlvdemux
    src/application_resources.cpp
    src/compressed_ip_parser.cpp
    src/demuxer.cpp
    src/duration_probe.cpp
    src/mmtp_media_parser.cpp
    src/mmtp_parser.cpp
    src/mmtp_signalling_parser.cpp
    src/mmtp_signalling_tables.cpp
    src/mse/hevc_parser.cpp
    src/mse/latm_parser.cpp
    src/mse/mp4_builder.cpp
    wasm/mse_remuxer.cpp
    src/playback.cpp
    src/recording.cpp
    src/tlv_parser.cpp
)
add_library(tlvdemux::tlvdemux ALIAS tlvdemux)

set_target_properties(tlvdemux PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME tlvdemux
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_compile_features(tlvdemux PUBLIC cxx_std_20)
if(EMSCRIPTEN)
    target_compile_options(tlvdemux PRIVATE "-sUSE_ZLIB=1")
    target_link_options(tlvdemux PUBLIC "-sUSE_ZLIB=1")
else()
    target_link_libraries(tlvdemux PUBLIC ZLIB::ZLIB)
endif()
target_include_directories(tlvdemux
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(tlvdemux PRIVATE /W4 /permissive-)
else()
    target_compile_options(tlvdemux PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

install(TARGETS tlvdemux
    EXPORT tlvdemuxTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/tlvdemux)
install(EXPORT tlvdemuxTargets
    FILE tlvdemuxTargets.cmake
    NAMESPACE tlvdemux::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tlvdemux
)
configure_package_config_file(
    cmake/tlvdemuxConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/tlvdemuxConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tlvdemux
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/tlvdemuxConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/tlvdemuxConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/tlvdemuxConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tlvdemux
)

if(TLVDEMUX_BUILD_TOOLS)
    add_executable(tlvdemux-inspect tools/tlvdemux-inspect.cpp)
    target_link_libraries(tlvdemux-inspect PRIVATE tlvdemux::tlvdemux)
    add_executable(tlvdemux-cocktail tools/tlvdemux-cocktail.cpp)
    target_link_libraries(tlvdemux-cocktail PRIVATE tlvdemux::tlvdemux)
    add_executable(tlvdemux-extract tools/tlvdemux-extract.cpp)
    target_link_libraries(tlvdemux-extract PRIVATE tlvdemux::tlvdemux)
    add_executable(tlvdemux-probe tools/tlvdemux-probe.cpp)
    target_link_libraries(tlvdemux-probe PRIVATE tlvdemux::tlvdemux)
    if(APPLE)
        add_executable(tlvdemux-videotoolbox-probe tools/tlvdemux-videotoolbox-probe.cpp)
        target_link_libraries(tlvdemux-videotoolbox-probe PRIVATE
            tlvdemux::tlvdemux
            "-framework CoreFoundation"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework VideoToolbox"
        )
        target_compile_options(tlvdemux-videotoolbox-probe PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    if(MSVC)
        target_compile_options(tlvdemux-inspect PRIVATE /W4 /permissive-)
        target_compile_options(tlvdemux-cocktail PRIVATE /W4 /permissive-)
        target_compile_options(tlvdemux-extract PRIVATE /W4 /permissive-)
        target_compile_options(tlvdemux-probe PRIVATE /W4 /permissive-)
    else()
        target_compile_options(tlvdemux-inspect PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(tlvdemux-cocktail PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(tlvdemux-extract PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(tlvdemux-probe PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    install(TARGETS tlvdemux-inspect tlvdemux-extract tlvdemux-probe
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(APPLE)
        install(TARGETS tlvdemux-videotoolbox-probe
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()

if(TLVDEMUX_BUILD_WASM_BINDINGS)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "TLVDEMUX_BUILD_WASM_BINDINGS requires the Emscripten toolchain")
    endif()

    add_executable(tlvdemux-wasm wasm/bindings.cpp)
    target_link_libraries(tlvdemux-wasm PRIVATE tlvdemux::tlvdemux)
    target_link_options(tlvdemux-wasm PRIVATE
        --bind
        "-sALLOW_MEMORY_GROWTH=1"
        "-sGROWABLE_ARRAYBUFFERS=0"
        "-sENVIRONMENT=web,worker,node"
        "-sEXPORTED_FUNCTIONS=['_malloc','_free']"
        "-sEXPORTED_RUNTIME_METHODS=['HEAPU8']"
        "-sEXPORT_NAME=createTlvDemuxModule"
        "-sFILESYSTEM=0"
        "-sMODULARIZE=1"
        "-sNO_EXIT_RUNTIME=1"
        "-sSINGLE_FILE=1"
        "-sWASM_BIGINT=1"
    )
    set_target_properties(tlvdemux-wasm PROPERTIES
        OUTPUT_NAME tlvdemux
        SUFFIX ".js"
    )

    install(PROGRAMS
        ${CMAKE_CURRENT_BINARY_DIR}/tlvdemux.js
        DESTINATION ${CMAKE_INSTALL_DATADIR}/tlvdemux/wasm
    )
endif()

if(BUILD_TESTING)
    add_executable(tlvdemux-application-resources-tests tests/application_resources_tests.cpp)
    target_link_libraries(tlvdemux-application-resources-tests PRIVATE tlvdemux::tlvdemux)
    if(MSVC)
        target_compile_options(tlvdemux-application-resources-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(tlvdemux-application-resources-tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME tlvdemux-application-resources-tests
             COMMAND tlvdemux-application-resources-tests)

    add_executable(tlvdemux-tests tests/demuxer_tests.cpp)
    target_link_libraries(tlvdemux-tests PRIVATE tlvdemux::tlvdemux)
    if(MSVC)
        target_compile_options(tlvdemux-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(tlvdemux-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME tlvdemux-tests COMMAND tlvdemux-tests)

    add_executable(tlvdemux-recording-tests tests/recording_tests.cpp)
    target_link_libraries(tlvdemux-recording-tests PRIVATE tlvdemux::tlvdemux)
    if(MSVC)
        target_compile_options(tlvdemux-recording-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(tlvdemux-recording-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME tlvdemux-recording-tests COMMAND tlvdemux-recording-tests)

    add_executable(tlvdemux-mse-remuxer-tests tests/mse_remuxer_tests.cpp)
    target_link_libraries(tlvdemux-mse-remuxer-tests PRIVATE tlvdemux::tlvdemux)
    if(MSVC)
        target_compile_options(tlvdemux-mse-remuxer-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(tlvdemux-mse-remuxer-tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME tlvdemux-mse-remuxer-tests COMMAND tlvdemux-mse-remuxer-tests)

    if(TLVDEMUX_BUILD_WASM_BINDINGS)
        find_program(NODE_EXECUTABLE node)
        if(NODE_EXECUTABLE)
            add_test(
                NAME tlvdemux-wasm-smoke
                COMMAND ${NODE_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_smoke.mjs
                        $<TARGET_FILE:tlvdemux-wasm>
            )
        endif()
    endif()
endif()
