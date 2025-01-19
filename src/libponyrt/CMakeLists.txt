cmake_minimum_required(VERSION 3.21 FATAL_ERROR)

project(libponyrt VERSION ${PONYC_PROJECT_VERSION} LANGUAGES C CXX)

set(_c_src
    actor/actor.c
    actor/messageq.c
    asio/asio.c
    asio/emscripten.c
    asio/epoll.c
    asio/event.c
    asio/iocp.c
    asio/kqueue.c
    ds/fun.c
    ds/hash.c
    ds/list.c
    ds/stack.c
    gc/actormap.c
    gc/cycle.c
    gc/delta.c
    gc/gc.c
    gc/objectmap.c
    gc/serialise.c
    gc/trace.c
    lang/directory.c
    lang/errno.c
    lang/io.c
    lang/lsda.c
    lang/paths.c
    lang/posix_except.c
    lang/process.c
    lang/socket.c
    lang/ssl.c
    lang/stat.c
    lang/stdfd.c
    lang/time.c
    lang/win_except.c
    mem/alloc.c
    mem/heap.c
    mem/pagemap.c
    mem/pool.c
    mem/pool_memalign.c
    mem/pool_message_passing.c
    options/options.c
    platform/ponyassert.c
    platform/threads.c
    sched/cpu.c
    sched/mpmcq.c
    sched/mutemap.c
    sched/scheduler.c
    sched/start.c
    sched/systematic_testing.c
)

set(_ll_except_src "${CMAKE_CURRENT_SOURCE_DIR}/lang/except_try_catch.ll")
set(_ll_except_obj "${CMAKE_BINARY_DIR}/except_try_catch.o")

find_file(_llc_command
    NAMES llc.exe llc
    HINTS "${CMAKE_BINARY_DIR}/../../libs/bin" "${CMAKE_BINARY_DIR}/../libs/bin"
)

if(${CMAKE_HOST_SYSTEM_NAME} MATCHES "Darwin")
    set(CMAKE_OSX_DEPLOYMENT_TARGET ${PONY_OSX_PLATFORM})
endif()

add_library(libponyrt STATIC
    ${_c_src}
    ${_ll_except_obj}
)

if(PONY_USE_DTRACE)
    add_custom_command(OUTPUT dtrace_probes.o
        COMMAND dtrace -h -s "${CMAKE_CURRENT_SOURCE_DIR}/../common/dtrace_probes.d" -o "${CMAKE_CURRENT_SOURCE_DIR}/../common/dtrace_probes.h"
        COMMAND dtrace -G -s "${CMAKE_CURRENT_SOURCE_DIR}/../common/dtrace_probes.d" -o dtrace_probes.o
    )
    target_sources(libponyrt PRIVATE dtrace_probes.o)
endif()

if (NOT MSVC)
    if(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86-64")
        target_compile_options(libponyrt PUBLIC -mcx16)
    elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
        target_compile_options(libponyrt PUBLIC -mcx16)
    elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "amd64")
        target_compile_options(libponyrt PUBLIC -mcx16)
    elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "AMD64")
        target_compile_options(libponyrt PUBLIC -mcx16)
    elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86")
        target_compile_options(libponyrt PUBLIC -mcx16)
    endif()
endif()

set_source_files_properties(${ll_obj} PROPERTIES EXTERNAL_OBJECT True)

add_custom_command(
    COMMAND "${_llc_command}" ${LL_FLAGS} ${_ll_obj_flags} -relocation-model=pic -filetype=obj -o ${_ll_except_obj} ${_ll_except_src}
    WORKING_DIRECTORY ${libponyrt_BINARY_DIR}
    DEPENDS ${_ll_except_src}
    OUTPUT ${_ll_except_obj}
)

target_include_directories(libponyrt
    PUBLIC .
    PUBLIC ../common
)

if (MSVC)
    file(GLOB_RECURSE CFILES "${PROJECT_SOURCE_DIR}/*.c")
    set_source_files_properties(${CFILES} PROPERTIES LANGUAGE CXX)

    # copy libponyrt to the ponyc directory for use when linking pony programs
    add_custom_command(TARGET libponyrt POST_BUILD
        COMMAND $<$<CONFIG:Debug>:${CMAKE_COMMAND}> ARGS -E copy ${libponyrt_BINARY_DIR}/debug/libponyrt.lib ${CMAKE_BINARY_DIR}/../debug${PONY_OUTPUT_SUFFIX}/libponyrt.lib
        COMMAND $<$<CONFIG:Release>:${CMAKE_COMMAND}> ARGS -E copy ${libponyrt_BINARY_DIR}/release/libponyrt.lib ${CMAKE_BINARY_DIR}/../release${PONY_OUTPUT_SUFFIX}/libponyrt.lib
        COMMAND $<$<CONFIG:RelWithDebInfo>:${CMAKE_COMMAND}> ARGS -E copy ${libponyrt_BINARY_DIR}/relwithdebinfo/libponyrt.lib ${CMAKE_BINARY_DIR}/../relwithdebinfo${PONY_OUTPUT_SUFFIX}/libponyrt.lib
        COMMAND $<$<CONFIG:MinSizeRel>:${CMAKE_COMMAND}> ARGS -E copy ${libponyrt_BINARY_DIR}/minsizerel/libponyrt.lib ${CMAKE_BINARY_DIR}/../minsizerel${PONY_OUTPUT_SUFFIX}/libponyrt.lib
    )
else()
    target_include_directories(libponyrt
        PRIVATE /usr/local/include
    )

    if(${CMAKE_HOST_SYSTEM_NAME} MATCHES "Darwin")
        set(LIBPONYRT_ARCHIVE libponyrt.a)
    elseif(${CMAKE_HOST_SYSTEM_NAME} MATCHES "BSD|DragonFly")
        set(LIBPONYRT_ARCHIVE libponyrt.a)
    else()
        set(LIBPONYRT_ARCHIVE libponyrt-pic.a)
    endif()

    # copy libponyrt to the ponyc directory for use when linking pony programs
    add_custom_command(TARGET libponyrt POST_BUILD
        COMMAND $<$<CONFIG:Debug>:${CMAKE_COMMAND}> ARGS -E copy ${libponyrt_BINARY_DIR}/libponyrt.a ${CMAKE_BINARY_DIR}/../debug${PONY_OUTPUT_SUFFIX}/${LIBPONYRT_ARCHIVE}
        COMMAND $<$<CONFIG:Release>:${CMAKE_COMMAND}> ARGS -E copy ${libponyrt_BINARY_DIR}/libponyrt.a ${CMAKE_BINARY_DIR}/../release${PONY_OUTPUT_SUFFIX}/${LIBPONYRT_ARCHIVE}
        COMMAND $<$<CONFIG:RelWithDebInfo>:${CMAKE_COMMAND}> ARGS -E copy ${libponyrt_BINARY_DIR}/libponyrt.a ${CMAKE_BINARY_DIR}/../relwithdebinfo${PONY_OUTPUT_SUFFIX}/${LIBPONYRT_ARCHIVE}
        COMMAND $<$<CONFIG:MinSizeRel>:${CMAKE_COMMAND}> ARGS -E copy ${libponyrt_BINARY_DIR}/libponyrt.a ${CMAKE_BINARY_DIR}/../minsizerel${PONY_OUTPUT_SUFFIX}/${LIBPONYRT_ARCHIVE}
    )
endif (MSVC)

if(PONY_RUNTIME_BITCODE)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "You can only use runtime-bitcode with Clang.")
    endif()

    set(_c_flags ${CMAKE_C_FLAGS})
    separate_arguments(_c_flags)

    set(_ll_src_all ${_c_src})
    foreach(_src_file ${_ll_src_all})
        #message("${libponyrt_SOURCE_DIR}/${_src_file} -> ${libponyrt_BINARY_DIR}/${_src_file}.bc")
        get_filename_component(_src_dir ${_src_file} DIRECTORY)
        add_custom_command(
            COMMAND mkdir -p "${libponyrt_BINARY_DIR}/${_src_dir}" && ${CMAKE_C_COMPILER} $<IF:$<BOOL:${CMAKE_C_COMPILER_TARGET}>,--target=${CMAKE_C_COMPILER_TARGET},> -DBUILD_COMPILER="${CMAKE_C_COMPILER_VERSION}" -D_FILE_OFFSET_BITS=64 -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -DLLVM_BUILD_MODE=${PONY_LLVM_BUILD_MODE} -DLLVM_VERSION="${LLVM_VERSION}" -DPONY_COMPILER="${CMAKE_C_COMPILER}" -DPONY_ARCH="${CMAKE_SYSTEM_PROCESSOR}" -DPONY_BUILD_CONFIG="release" -DPONY_USE_BIGINT -DPONY_VERSION="${PONYC_VERSION}" -DPONY_VERSION_STR="${PONYC_VERSION} [release]\\nCompiled with: LLVM ${LLVM_VERSION} -- ${CMAKE_C_COMPILER_ID}-${CMAKE_C_COMPILER_VERSION}-${CMAKE_C_COMPILER_ARCHITECTURE_ID}" -pthread -fexceptions -Werror -Wconversion -Wno-sign-conversion -Wno-atomic-alignment -Wextra -Wall ${_c_flags} -I. -I../common -emit-llvm -o "${libponyrt_BINARY_DIR}/${_src_file}.bc" -c ${_src_file}
            WORKING_DIRECTORY ${libponyrt_SOURCE_DIR}
            DEPENDS "${libponyrt_SOURCE_DIR}/${_src_file}"
            OUTPUT "${libponyrt_BINARY_DIR}/${_src_file}.bc"
            VERBATIM
        )
    endforeach()

    set(_ll_bc_all ${_ll_src_all})
    list(TRANSFORM _ll_bc_all PREPEND "${libponyrt_BINARY_DIR}/")
    list(TRANSFORM _ll_bc_all APPEND ".bc")
    list(APPEND _ll_bc_all ${_ll_except_src})

    add_custom_target(libponyrt_bc ALL
        ${libponyrt_SOURCE_DIR}/../../build/libs/bin/llvm-link -o "${libponyrt_BINARY_DIR}/libponyrt.bc" ${_ll_bc_all}
        DEPENDS ${_ll_bc_all}
    )

    add_custom_command(TARGET libponyrt_bc POST_BUILD
        COMMAND $<$<CONFIG:Debug>:${CMAKE_COMMAND}> ARGS -E copy ${libponyrt_BINARY_DIR}/libponyrt.bc ${CMAKE_BINARY_DIR}/../debug${PONY_OUTPUT_SUFFIX}/
        COMMAND $<$<CONFIG:Release>:${CMAKE_COMMAND}> ARGS -E copy ${libponyrt_BINARY_DIR}/libponyrt.bc ${CMAKE_BINARY_DIR}/../release${PONY_OUTPUT_SUFFIX}/
        COMMAND $<$<CONFIG:RelWithDebInfo>:${CMAKE_COMMAND}> ARGS -E copy ${libponyrt_BINARY_DIR}/libponyrt.bc ${CMAKE_BINARY_DIR}/../relwithdebinfo${PONY_OUTPUT_SUFFIX}/
        COMMAND $<$<CONFIG:MinSizeRel>:${CMAKE_COMMAND}> ARGS -E copy ${libponyrt_BINARY_DIR}/libponyrt.bc ${CMAKE_BINARY_DIR}/../minsizerel${PONY_OUTPUT_SUFFIX}/
    )
endif()
