add_library(runtime_libstdcxx STATIC)
add_library(runtime::libstdcxx ALIAS runtime_libstdcxx)

if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    set(RUNTIME_LIBSTDCXX_VENDOR_VERSION gcc-15.2.0)
else()
    set(RUNTIME_LIBSTDCXX_VENDOR_VERSION gcc-16.1.0)
endif()

set(RUNTIME_LIBSTDCXX_VENDOR_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/${RUNTIME_LIBSTDCXX_VENDOR_VERSION}
)

target_sources(runtime_libstdcxx
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/atomic.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/backend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/clock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/condition_variable.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/cxx_guard.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/future.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/once.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/random.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sleep.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terminate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/thread.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/thread_exit.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tls.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(runtime_libstdcxx
        PRIVATE
            ${RUNTIME_LIBSTDCXX_VENDOR_DIR}/c++17/fs_ops.cc
            ${RUNTIME_LIBSTDCXX_VENDOR_DIR}/c++17/fs_dir.cc
    )
endif()

if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
    # GCC 16 moved atomic wait/notify helpers out of the headers. Build the
    # matching implementation against this runtime's gthreads port: the Arm
    # archive omits it, while the Linux library's copy uses pthreads directly.
    target_sources(runtime_libstdcxx
        PRIVATE
            ${RUNTIME_LIBSTDCXX_VENDOR_DIR}/c++20/atomic.cc
    )
endif()

target_include_directories(runtime_libstdcxx
    BEFORE PUBLIC
        ${PROJECT_SOURCE_DIR}/platform/runtime
        $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Generic>:${RUNTIME_LIBSTDCXX_VENDOR_DIR}>
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(runtime_libstdcxx
    PRIVATE
        platform::hal
    PUBLIC
        runtime::libc
        threadx
        project_compiler_settings
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # GCC emits libatomic calls for std::atomic<T> objects that are wider
    # than the host's native lock-free operations.
    target_link_libraries(runtime_libstdcxx PUBLIC atomic)
endif()

target_compile_definitions(runtime_libstdcxx
    PUBLIC
        THREADX_STD_ENABLED=1
        _GLIBCXX_HAS_GTHREADS=1
        _GLIBCXX_HAVE_POSIX_SEMAPHORE=1
        _GLIBCXX_USE_SCHED_YIELD=1
)

# Keep the automatic runtime hook and the replacement guard ABI in the final
# link even when no ordinary C++ reference pulls their archive members first.
target_link_options(runtime_libstdcxx
    INTERFACE
        LINKER:--undefined=__cxa_guard_acquire
        LINKER:--undefined=runtime_install_terminate_handler
        LINKER:--undefined=runtime_libstdcxx_initialize
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_link_options(runtime_libstdcxx
        INTERFACE
            LINKER:--undefined=__aeabi_read_tp
            LINKER:--undefined=__cxa_get_globals
            LINKER:--undefined=__cxa_thread_atexit
    )
endif()
