cmake_minimum_required(VERSION 3.10)
project(webrtc-aec-component)

if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()

# Find required packages
find_package(YARP REQUIRED COMPONENTS os sig dev)
find_package(PkgConfig REQUIRED)

set(WEBRTC_AUDIO_PROCESSING_ROOT "" CACHE PATH "Path to the WebRTC audio-processing install prefix")

set(WEBRTC_AUDIO_PROCESSING_ROOT_RESOLVED "${WEBRTC_AUDIO_PROCESSING_ROOT}")
if(DEFINED ENV{WEBRTC_AUDIO_PROCESSING_ROOT} AND NOT "$ENV{WEBRTC_AUDIO_PROCESSING_ROOT}" STREQUAL "")
    set(WEBRTC_AUDIO_PROCESSING_ROOT_RESOLVED "$ENV{WEBRTC_AUDIO_PROCESSING_ROOT}")
elseif(NOT WEBRTC_AUDIO_PROCESSING_ROOT_RESOLVED)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../webrtc-audio-processing/install")
        set(WEBRTC_AUDIO_PROCESSING_ROOT_RESOLVED "${CMAKE_CURRENT_LIST_DIR}/../webrtc-audio-processing/install")
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../install")
        set(WEBRTC_AUDIO_PROCESSING_ROOT_RESOLVED "${CMAKE_CURRENT_LIST_DIR}/../install")
    endif()
endif()

set(WEBRTC_AUDIO_PROCESSING_ROOT "${WEBRTC_AUDIO_PROCESSING_ROOT_RESOLVED}")

set(WAP_LOCAL_PKGCONFIG_DIRS)
foreach(WAP_LOCAL_PKGCONFIG_DIR
        "${WEBRTC_AUDIO_PROCESSING_ROOT}/lib/x86_64-linux-gnu/pkgconfig"
        "${WEBRTC_AUDIO_PROCESSING_ROOT}/lib/pkgconfig"
        "${WEBRTC_AUDIO_PROCESSING_ROOT}/share/pkgconfig")
    if(EXISTS "${WAP_LOCAL_PKGCONFIG_DIR}/webrtc-audio-processing-2.pc")
        list(APPEND WAP_LOCAL_PKGCONFIG_DIRS "${WAP_LOCAL_PKGCONFIG_DIR}")
    endif()
endforeach()

if(WAP_LOCAL_PKGCONFIG_DIRS)
    string(JOIN ":" WAP_LOCAL_PKGCONFIG_PATH "${WAP_LOCAL_PKGCONFIG_DIRS}")
    if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
        set(ENV{PKG_CONFIG_PATH}
            "${WAP_LOCAL_PKGCONFIG_PATH}:$ENV{PKG_CONFIG_PATH}")
    else()
        set(ENV{PKG_CONFIG_PATH} "${WAP_LOCAL_PKGCONFIG_PATH}")
    endif()
endif()

pkg_check_modules(WAP QUIET IMPORTED_TARGET webrtc-audio-processing-2)
if(NOT WAP_FOUND)
    pkg_check_modules(WAP QUIET IMPORTED_TARGET webrtc-audio-processing)
endif()
if(NOT WAP_FOUND)
    pkg_check_modules(WAP QUIET IMPORTED_TARGET webrtc_audio_processing)
endif()

if(NOT WAP_FOUND)
    message(FATAL_ERROR
        "Could not find WebRTC Audio Processing via pkg-config. Set WEBRTC_AUDIO_PROCESSING_ROOT to the install prefix "
        "or add the directory containing webrtc-audio-processing-2.pc to PKG_CONFIG_PATH.")
endif()

# Add the executable
add_executable(aecComponent
    AECComponent.hpp
    AECComponent.cpp
    aec_main.cpp
)

# Link libraries
target_link_libraries(aecComponent
    ${YARP_LIBRARIES}
    PkgConfig::WAP
)

# Include directories
target_include_directories(aecComponent PRIVATE
    ${YARP_INCLUDE_DIRS}
)

set(WAP_LOCAL_INCLUDE_DIR "${WEBRTC_AUDIO_PROCESSING_ROOT}/include")
if(EXISTS "${WAP_LOCAL_INCLUDE_DIR}/absl/base/config.h")
    target_include_directories(aecComponent PRIVATE "${WAP_LOCAL_INCLUDE_DIR}")
endif()

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aecComponent PRIVATE -Wall -Wextra -std=c++17)
endif()

# Installation
install(TARGETS aecComponent DESTINATION bin)
install(FILES aec_config.ini DESTINATION share/aec)
