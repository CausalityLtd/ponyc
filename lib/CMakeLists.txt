cmake_minimum_required(VERSION 3.21 FATAL_ERROR)

file(STRINGS "../VERSION" PONYC_PROJECT_VERSION)
project(ponyclibs VERSION ${PONYC_PROJECT_VERSION} LANGUAGES C CXX)

include(ExternalProject)

if(NOT DEFINED PONYC_LIBS_BUILD_TYPE)
    set(PONYC_LIBS_BUILD_TYPE Release)
endif()

# System information
message("-- CMAKE_SYSTEM_INFO_FILE: ${CMAKE_SYSTEM_INFO_FILE}")
message("-- CMAKE_SYSTEM_NAME:      ${CMAKE_SYSTEM_NAME}")
message("-- CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}")
message("-- CMAKE_SYSTEM:           ${CMAKE_SYSTEM}")

set(_compiler_arch ${CMAKE_C_COMPILER_ARCHITECTURE_ID})
if("${_compiler_arch}" STREQUAL "")
    set(_compiler_arch ${CMAKE_SYSTEM_PROCESSOR})
endif()

message("Compiler architecture is ${_compiler_arch}")

if(NOT MSVC)
    if((NOT DEFINED PONY_PIC_FLAG) OR (PONY_PIC_FLAG STREQUAL ""))
        set(PONY_PIC_FLAG "-fpic")
    endif()
endif()

# Libraries

set(PONYC_GBENCHMARK_URL https://github.com/google/benchmark/archive/v1.9.1.tar.gz)
ExternalProject_Add(gbenchmark
    URL ${PONYC_GBENCHMARK_URL}
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=${PONYC_LIBS_BUILD_TYPE} -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX} -DBENCHMARK_ENABLE_GTEST_TESTS=OFF -DBENCHMARK_ENABLE_WERROR=OFF -DCMAKE_CXX_FLAGS=${PONY_PIC_FLAG} --no-warn-unused-cli
)

set(PONYC_GOOGLETEST_URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)

ExternalProject_Add(googletest
    URL ${PONYC_GOOGLETEST_URL}
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=${PONYC_LIBS_BUILD_TYPE} -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX} -DCMAKE_CXX_FLAGS=${PONY_PIC_FLAG} -Dgtest_force_shared_crt=ON --no-warn-unused-cli
)

add_library(blake2 STATIC blake2/blake2b-ref.c)
set_property(TARGET blake2 PROPERTY POSITION_INDEPENDENT_CODE ON)

install(TARGETS blake2
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    COMPONENT library
)

find_package(Git)

set(LLVM_DESIRED_HASH "7cbf1a2591520c2491aa35339f227775f4d3adf6")
set(PATCHES_DESIRED_HASH "99d46d25e96861de9847c055aeda1fef13fa0219524b2b7074c9acedb5f7a1c4")

if(GIT_FOUND)
    if(EXISTS "${PROJECT_SOURCE_DIR}/../.git")
        # Update submodules as needed
        option(GIT_SUBMODULE "Check submodules during build" ON)
        if(GIT_SUBMODULE)
            message(STATUS "Updating submodules...")
            execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive --depth 1
                            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                            RESULT_VARIABLE git_submod_result)
            #message("git_submod_result ${git_submod_result}")
            if(NOT git_submod_result EQUAL "0")
                message(FATAL_ERROR "git submodule update --init --recursive --depth 1 failed with ${git_submod_result}, please checkout submodules")
            endif()

            # we check to make sure the submodule hash matches
            # the reason the submodule hash is in this file is to be able to use this file as a key for caching the libs in CI
            execute_process(COMMAND ${GIT_EXECUTABLE} submodule status
                            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                            OUTPUT_VARIABLE git_submod_output)
            #message("git_submod_output ${git_submod_output}")
            string(FIND "${git_submod_output}" "${LLVM_DESIRED_HASH}" LLVM_SUBMOD_POS)
            if(LLVM_SUBMOD_POS EQUAL "-1")
                message(FATAL_ERROR "Expecting the lib/llvm/src submodule to be at hash '${LLVM_DESIRED_HASH}'; found '${git_submod_output}'; update the LLVM_DESIRED_HASH variable in lib/CMakeLists.txt if you've updated the submodule.")
            endif()
        endif()
    endif()

    # Apply patches
    message("Applying patches...")
    file(GLOB PONY_LLVM_PATCHES "${PROJECT_SOURCE_DIR}/llvm/patches/*.diff")

    # check to see if the patch hashes match
    message("Checking patches ${PONY_LLVM_PATCHES}")
    set(PATCHES_ACTUAL_HASH "needed_if_no_patches")
    foreach (PATCH ${PONY_LLVM_PATCHES})
        file(STRINGS ${PATCH} patch_file NEWLINE_CONSUME)
        string(REPLACE "\n" " " patch_file "${patch_file}")
        string(SHA256 patch_file_hash "${patch_file}")
        string(CONCAT PATCHES_ACTUAL_HASH ${PATCHES_ACTUAL_HASH} ${patch_file_hash})
        # message("concat is '${PATCHES_ACTUAL_HASH}'")
    endforeach()
    string(SHA256 PATCHES_ACTUAL_HASH ${PATCHES_ACTUAL_HASH})
    # message("Desired hash ${PATCHES_DESIRED_HASH}")
    # message("Actual hash  ${PATCHES_ACTUAL_HASH}")
    if(NOT PATCHES_ACTUAL_HASH MATCHES "${PATCHES_DESIRED_HASH}")
        message(FATAL_ERROR "Patch hash actual '${PATCHES_ACTUAL_HASH}' does not match desired '${PATCHES_DESIRED_HASH}'")
    endif()

    foreach (PATCH ${PONY_LLVM_PATCHES})
        message("  Checking ${PATCH}")
        execute_process(COMMAND ${GIT_EXECUTABLE} apply --check -p 1 --ignore-whitespace --whitespace=nowarn ${PATCH}
                        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/llvm/src"
                        ERROR_VARIABLE _err_out
                        RESULT_VARIABLE git_apply_check_result)
        if(git_apply_check_result EQUAL "0")
            message("    Applying ${PATCH}")
            execute_process(COMMAND ${GIT_EXECUTABLE} apply -p 1 --ignore-whitespace --whitespace=nowarn ${PATCH}
                            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/llvm/src"
                            RESULT_VARIABLE git_apply_result)
            if(NOT git_apply_result EQUAL "0")
                message(FATAL_ERROR "Unable to apply ${PATCH}")
            endif()
        else()
            message("    Already applied ${PATCH}")
        endif()
    endforeach()
else()
    message(FATAL_ERROR "Git not found!")
endif()

message("Building targets: ${LLVM_TARGETS_TO_BUILD}")

set(LLVM_ENABLE_BINDINGS OFF CACHE BOOL "ponyc specific override of LLVM cache entry")
set(LLVM_ENABLE_OCAMLDOC OFF CACHE BOOL "ponyc specific override of LLVM cache entry")
set(LLVM_ENABLE_PIC OFF CACHE BOOL "ponyc specific override of LLVM cache entry")
set(LLVM_ENABLE_TERMINFO OFF CACHE BOOL "ponyc specific override of LLVM cache entry")
set(LLVM_ENABLE_WARNINGS OFF CACHE BOOL "ponyc specific override of LLVM cache entry")
set(LLVM_ENABLE_Z3_SOLVER OFF CACHE BOOL "ponyc specific override of LLVM cache entry")
set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "ponyc specific override of LLVM cache entry")
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "ponyc specific override of LLVM cache entry")
set(LLVM_TOOL_REMARKS_SHLIB_BUILD OFF CACHE BOOL "ponyc specific override of LLVM cache entry")
set(LLVM_ENABLE_ZLIB OFF CACHE BOOL "ponyc specific override of LLVM cache entry")
set(LLVM_ENABLE_ZSTD OFF CACHE BOOL "ponyc specific override of LLVM cache entry")
set(LLVM_ENABLE_LIBXML2 OFF CACHE BOOL "ponyc specific override of LLVM cache entry")

if(${CMAKE_HOST_SYSTEM_NAME} MATCHES "Darwin")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mmacosx-version-min=10.14 -DUSE_SCHEDULER_SCALING_PTHREADS")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -mmacosx-version-min=10.14")
endif()

add_subdirectory(llvm/src/llvm)
