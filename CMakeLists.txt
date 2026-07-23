cmake_minimum_required(VERSION 3.16)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BTCRIG_VERSION_RAW)
string(STRIP "${BTCRIG_VERSION_RAW}" BTCRIG_VERSION)
string(REGEX REPLACE "^v" "" BTCRIG_VERSION "${BTCRIG_VERSION}")
set(BTCRIG_VERSION_TAG "v${BTCRIG_VERSION}")

project(BTCRig VERSION "${BTCRIG_VERSION}" LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(BTC_MINER_NATIVE "Enable native CPU tuning for this machine" OFF)
option(BTCRIG_OPENCL "Enable optional OpenCL mining backend when the SDK is available" ON)
option(BTCRIG_CUDA "Enable optional CUDA driver backend when the NVIDIA driver is available" ON)

if(BTC_MINER_NATIVE AND
   (CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_C_COMPILER_TARGET MATCHES "android"))
    message(WARNING "BTC_MINER_NATIVE is disabled on Android to avoid optional ISA instructions across heterogeneous CPU clusters")
    set(BTC_MINER_NATIVE OFF)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
if(BTCRIG_OPENCL)
    find_package(OpenCL QUIET)
endif()

pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)

include(CheckCCompilerFlag)
include(CheckCSourceCompiles)
include(CheckIncludeFile)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/btcrig_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/btcrig_version.h"
    @ONLY
)

set(BTC_MINER_SHA256D_SOURCES src/sha256d.c)
set(BTC_MINER_ARM_SHA2_FLAGS "")
set(BTC_MINER_X86_SHA_NI_FLAGS "")
set(BTC_MINER_NATIVE_FLAG "")
set(BTC_MINER_ARM_SHA2_ENABLED FALSE)
set(BTCRIG_OPENCL_ENABLED FALSE)
set(BTCRIG_OPENCL_HEADERS_FOUND FALSE)
set(BTCRIG_OPENCL_INCLUDE_DIRS "")
set(BTCRIG_CUDA_ENABLED FALSE)

check_c_compiler_flag("-march=native" HAS_MARCH_NATIVE)
check_c_compiler_flag("-mcpu=native" HAS_MCPU_NATIVE)
check_c_compiler_flag("-march=armv8-a+crypto" HAS_MARCH_ARMV8_CRYPTO)
check_c_compiler_flag("-msha" HAS_MSHA)
check_c_compiler_flag("-mssse3" HAS_MSSSE3)
check_c_compiler_flag("-msse4.1" HAS_MSSE41)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND HAS_MARCH_ARMV8_CRYPTO)
    set(BTC_MINER_ARM_SHA2_FLAGS "-march=armv8-a+crypto")
endif()

if(BTC_MINER_NATIVE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND HAS_MCPU_NATIVE)
        set(BTC_MINER_NATIVE_FLAG "-mcpu=native")
    elseif(HAS_MARCH_NATIVE)
        set(BTC_MINER_NATIVE_FLAG "-march=native")
    endif()
endif()

if(BTC_MINER_ARM_SHA2_FLAGS)
    set(CMAKE_REQUIRED_FLAGS_SAVE "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} ${BTC_MINER_ARM_SHA2_FLAGS}")
    check_c_source_compiles("
        #include <arm_neon.h>
        int main(void) {
            uint32x4_t x = vdupq_n_u32(0);
            x = vsha256hq_u32(x, x, x);
            return (int)vgetq_lane_u32(x, 0);
        }
    " HAS_ARM_SHA2_INTRINSICS)
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS_SAVE}")
    if(HAS_ARM_SHA2_INTRINSICS)
        set(BTC_MINER_ARM_SHA2_ENABLED TRUE)
    endif()
endif()

if(BTC_MINER_ARM_SHA2_ENABLED)
    list(APPEND BTC_MINER_SHA256D_SOURCES src/sha256d_arm_sha2.c)
    set_source_files_properties(src/sha256d_arm_sha2.c PROPERTIES
        COMPILE_OPTIONS "${BTC_MINER_ARM_SHA2_FLAGS}"
    )
endif()

if(HAS_MSHA AND HAS_MSSSE3 AND HAS_MSSE41)
    set(BTC_MINER_X86_SHA_NI_FLAGS "-msha;-mssse3;-msse4.1")
    set(CMAKE_REQUIRED_FLAGS_SAVE "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -msha -mssse3 -msse4.1")
    check_c_source_compiles("
        #include <immintrin.h>
        int main(void) {
            __m128i x = _mm_setzero_si128();
            x = _mm_sha256rnds2_epu32(x, x, x);
            return _mm_cvtsi128_si32(x);
        }
    " HAS_X86_SHA_NI_INTRINSICS)
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS_SAVE}")
endif()

if(HAS_X86_SHA_NI_INTRINSICS)
    list(APPEND BTC_MINER_SHA256D_SOURCES src/sha256d_x86_sha_ni.c)
    set_source_files_properties(src/sha256d_x86_sha_ni.c PROPERTIES
        COMPILE_OPTIONS "${BTC_MINER_X86_SHA_NI_FLAGS}"
    )
endif()

set(BTCRIG_OPENCL_SOURCES "")
if(BTCRIG_OPENCL)
    set(BTCRIG_REQUIRED_INCLUDES_SAVE "${CMAKE_REQUIRED_INCLUDES}")
    if(OpenCL_FOUND AND OpenCL_INCLUDE_DIRS)
        set(CMAKE_REQUIRED_INCLUDES ${OpenCL_INCLUDE_DIRS})
    endif()
    check_include_file("CL/cl.h" BTCRIG_HAS_CL_CL_H)
    check_include_file("OpenCL/opencl.h" BTCRIG_HAS_OPENCL_OPENCL_H)
    set(CMAKE_REQUIRED_INCLUDES "${BTCRIG_REQUIRED_INCLUDES_SAVE}")

    if(BTCRIG_HAS_CL_CL_H OR BTCRIG_HAS_OPENCL_OPENCL_H)
        set(BTCRIG_OPENCL_HEADERS_FOUND TRUE)
    endif()
    if(OpenCL_FOUND AND OpenCL_INCLUDE_DIRS)
        set(BTCRIG_OPENCL_INCLUDE_DIRS ${OpenCL_INCLUDE_DIRS})
        set(BTCRIG_OPENCL_HEADERS_FOUND TRUE)
    endif()
endif()

if(BTCRIG_OPENCL_HEADERS_FOUND)
    set(BTCRIG_OPENCL_ENABLED TRUE)
    set(BTCRIG_OPENCL_SOURCES src/opencl_miner.c src/opencl_loader.c)
    message(STATUS "OpenCL headers found: GPU backend will use runtime OpenCL loading")
elseif(BTCRIG_OPENCL)
    message(STATUS "OpenCL headers not found: building CPU-only miner")
endif()

set(BTCRIG_CUDA_SOURCES "")
if(BTCRIG_CUDA)
    set(BTCRIG_CUDA_ENABLED TRUE)
    set(BTCRIG_CUDA_SOURCES src/cuda_miner.c)
    set_source_files_properties(src/cuda_miner.c PROPERTIES
        COMPILE_OPTIONS "-Wno-overlength-strings"
    )
    message(STATUS "CUDA driver backend will be built with runtime driver loading")
endif()

function(btcrig_apply_c_warnings target optimization)
    target_compile_options(${target} PRIVATE
        ${optimization}
        -Wall
        -Wextra
        -Wpedantic
    )
endfunction()

function(btcrig_apply_generated_include target)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )
endfunction()

function(btcrig_apply_sha_backend target)
    target_compile_definitions(${target} PRIVATE
        OPENSSL_SUPPRESS_DEPRECATED
    )

    if(BTC_MINER_NATIVE_FLAG)
        target_compile_options(${target} PRIVATE "${BTC_MINER_NATIVE_FLAG}")
    endif()

    if(BTC_MINER_ARM_SHA2_ENABLED)
        target_compile_definitions(${target} PRIVATE BTC_MINER_ARM_SHA2)
    endif()

    if(HAS_X86_SHA_NI_INTRINSICS)
        target_compile_definitions(${target} PRIVATE BTC_MINER_X86_SHA_NI)
    endif()
endfunction()

function(btcrig_link_opencl target)
    if(BTCRIG_OPENCL_ENABLED)
        target_compile_definitions(${target} PRIVATE BTC_MINER_OPENCL)
        if(BTCRIG_OPENCL_INCLUDE_DIRS)
            target_include_directories(${target} PRIVATE ${BTCRIG_OPENCL_INCLUDE_DIRS})
        endif()
        if(CMAKE_DL_LIBS)
            target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
        endif()
    endif()
endfunction()

function(btcrig_link_cuda target)
    if(BTCRIG_CUDA_ENABLED)
        target_compile_definitions(${target} PRIVATE BTC_MINER_CUDA)
        if(CMAKE_DL_LIBS)
            target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
        endif()
    endif()
endfunction()

function(btcrig_link_windows_sockets target)
    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32)
    endif()
endfunction()

add_executable(btc_bench
    src/main.c
    src/console.c
    src/cpu_info.c
    src/miner.c
    ${BTC_MINER_SHA256D_SOURCES}
    ${BTCRIG_OPENCL_SOURCES}
    ${BTCRIG_CUDA_SOURCES}
)

btcrig_apply_c_warnings(btc_bench -O3)
btcrig_apply_generated_include(btc_bench)
btcrig_apply_sha_backend(btc_bench)
btcrig_link_opencl(btc_bench)
btcrig_link_cuda(btc_bench)

target_link_libraries(btc_bench PRIVATE
    OpenSSL::Crypto
    Threads::Threads
)

add_executable(btc_stratum
    src/stratum_main.c
    src/stratum.c
    src/console.c
    src/cpu_info.c
    src/donation.c
    src/miner.c
    ${BTC_MINER_SHA256D_SOURCES}
    ${BTCRIG_OPENCL_SOURCES}
    ${BTCRIG_CUDA_SOURCES}
)

btcrig_apply_c_warnings(btc_stratum -O3)
btcrig_apply_generated_include(btc_stratum)
btcrig_apply_sha_backend(btc_stratum)
btcrig_link_opencl(btc_stratum)
btcrig_link_cuda(btc_stratum)

target_link_libraries(btc_stratum PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::JANSSON
    Threads::Threads
)

btcrig_link_windows_sockets(btc_stratum)

add_executable(btc_proxy
    src/proxy_main.c
)

btcrig_apply_c_warnings(btc_proxy -O2)
btcrig_apply_generated_include(btc_proxy)

target_link_libraries(btc_proxy PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::JANSSON
    Threads::Threads
)

btcrig_link_windows_sockets(btc_proxy)
