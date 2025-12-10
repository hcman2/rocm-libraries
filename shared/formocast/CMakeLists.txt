# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier:  MIT

cmake_minimum_required(VERSION 3.24.4)

project(Formocast VERSION 1.0.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FORMOCAST_STANDALONE ON)
else()
    set(FORMOCAST_STANDALONE OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Set cpack license file here to avoid rocm-cmake warning
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.md")

include(dependencies)

rocm_setup_version(VERSION ${PROJECT_VERSION})

option(FORMOCAST_BUILD_SHARED_LIBS "Build shared libraries." ${FORMOCAST_STANDALONE})
option(FORMOCAST_ENABLE_PYTHON "Enable Python bindings." OFF)
option(FORMOCAST_BUILD_TESTING "Enable Python binding tests." OFF)
option(FORMOCAST_ENABLE_INSTALL "Configure formocast installation" ON)

find_package(hip REQUIRED)

if(FORMOCAST_BUILD_SHARED_LIBS OR (BUILD_SHARED_LIBS AND FORMOCAST_STANDALONE))
    add_library(formocast SHARED) 
else()
    add_library(formocast STATIC)
endif()

add_library(roc::formocast ALIAS formocast)

set_target_properties(formocast PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
target_compile_features(formocast PUBLIC cxx_std_17)

add_library(formocast-headers INTERFACE)
add_library(roc::formocast-headers ALIAS formocast-headers)

target_include_directories(formocast-headers
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../origami/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_sources(formocast-headers
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/formocast.hpp>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/formocast_predict.hpp>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/formocast_utils.hpp>
)

target_link_libraries(formocast PUBLIC roc::formocast-headers)

target_sources(formocast
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formocast.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formocast_predict.cpp"
)

target_link_libraries(formocast PUBLIC hip::host)


rocm_install(TARGETS formocast formocast-headers)

rocm_export_targets(
    TARGETS roc::formocast roc::formocast-headers
    DEPENDS PACKAGE hip
    NAMESPACE roc::
)

if(FORMOCAST_BUILD_TESTING OR BUILD_TESTING)
    rocm_install(TARGETS formocast-tests
        COMPONENT tests
    )
    
    rocm_install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/tests/formocast_gtest.yaml"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT tests
    )
endif()

rocm_install(
    DIRECTORY include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT devel
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/formocast-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/formocast-config.cmake"
    @ONLY
)

rocm_install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/formocast-config.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/formocast"
    COMPONENT devel
)

set(BUILD_SHARED_LIBS ${FORMOCAST_BUILD_SHARED_LIBS})
set(FORMOCAST_CONFIG_DIR "\${CPACK_PACKAGING_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}" CACHE PATH "Path placed into ldconfig file")

rocm_create_package(
    NAME formocast
    DESCRIPTION "FORMOCAST C++ library for analytical GEMM computations"
    MAINTAINER "FORMOCAST Maintainer <menghung@amd.com>"
    LDCONFIG
    LDCONFIG_DIR ${FORMOCAST_CONFIG_DIR}
)
