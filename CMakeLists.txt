cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BTCRIG_VERSION_RAW)
string(STRIP "${BTCRIG_VERSION_RAW}" BTCRIG_VERSION)
string(REGEX REPLACE "^v" "" BTCRIG_VERSION "${BTCRIG_VERSION}")
set(BTCRIG_VERSION_TAG "v${BTCRIG_VERSION}")

project(BTCRig VERSION "${BTCRIG_VERSION}" LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(BTC_MINER_NATIVE "Enable native CPU tuning for this machine" OFF)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)

include(CheckCCompilerFlag)
include(CheckCSourceCompiles)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/btcrig_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/btcrig_version.h"
    @ONLY
)

set(BTC_MINER_SHA256D_SOURCES src/sha256d.c)
set(BTC_MINER_ARM_SHA2_FLAGS "")
set(BTC_MINER_X86_SHA_NI_FLAGS "")

check_c_compiler_flag("-march=native" HAS_MARCH_NATIVE)
check_c_compiler_flag("-mcpu=native" HAS_MCPU_NATIVE)
check_c_compiler_flag("-march=armv8-a+crypto" HAS_MARCH_ARMV8_CRYPTO)
check_c_compiler_flag("-msha" HAS_MSHA)
check_c_compiler_flag("-mssse3" HAS_MSSSE3)
check_c_compiler_flag("-msse4.1" HAS_MSSE41)
if(BTC_MINER_NATIVE AND HAS_MCPU_NATIVE)
    set(BTC_MINER_ARM_SHA2_FLAGS "-mcpu=native")
elseif(HAS_MARCH_ARMV8_CRYPTO)
    set(BTC_MINER_ARM_SHA2_FLAGS "-march=armv8-a+crypto")
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
endif()

if(HAS_ARM_SHA2_INTRINSICS)
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

add_executable(btc_bench
    src/main.c
    src/console.c
    src/cpu_info.c
    ${BTC_MINER_SHA256D_SOURCES}
)

target_compile_options(btc_bench PRIVATE
    -O3
    -Wall
    -Wextra
    -Wpedantic
)

target_include_directories(btc_bench PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_definitions(btc_bench PRIVATE
    OPENSSL_SUPPRESS_DEPRECATED
)

if(HAS_MARCH_NATIVE)
    target_compile_options(btc_bench PRIVATE -march=native)
endif()

if(HAS_ARM_SHA2_INTRINSICS)
    target_compile_definitions(btc_bench PRIVATE BTC_MINER_ARM_SHA2)
endif()

if(HAS_X86_SHA_NI_INTRINSICS)
    target_compile_definitions(btc_bench PRIVATE BTC_MINER_X86_SHA_NI)
endif()

target_link_libraries(btc_bench PRIVATE
    OpenSSL::Crypto
    Threads::Threads
)

add_executable(btc_stratum
    src/stratum_main.c
    src/stratum.c
    src/console.c
    src/cpu_info.c
    src/miner.c
    ${BTC_MINER_SHA256D_SOURCES}
)

target_compile_options(btc_stratum PRIVATE
    -O3
    -Wall
    -Wextra
    -Wpedantic
)

target_include_directories(btc_stratum PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(BTC_MINER_NATIVE AND HAS_MARCH_NATIVE)
    target_compile_options(btc_stratum PRIVATE -march=native)
endif()

target_compile_definitions(btc_stratum PRIVATE
    OPENSSL_SUPPRESS_DEPRECATED
)

if(HAS_ARM_SHA2_INTRINSICS)
    target_compile_definitions(btc_stratum PRIVATE BTC_MINER_ARM_SHA2)
endif()

if(HAS_X86_SHA_NI_INTRINSICS)
    target_compile_definitions(btc_stratum PRIVATE BTC_MINER_X86_SHA_NI)
endif()

target_link_libraries(btc_stratum PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::JANSSON
    Threads::Threads
)

if(WIN32)
    target_link_libraries(btc_stratum PRIVATE ws2_32)
endif()

add_executable(btc_proxy
    src/proxy_main.c
)

target_compile_options(btc_proxy PRIVATE
    -O2
    -Wall
    -Wextra
    -Wpedantic
)

target_include_directories(btc_proxy PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(btc_proxy PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::JANSSON
    Threads::Threads
)

if(WIN32)
    target_link_libraries(btc_proxy PRIVATE ws2_32)
endif()
