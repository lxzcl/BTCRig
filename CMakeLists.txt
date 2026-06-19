cmake_minimum_required(VERSION 3.16)

project(BTCRig C)

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

set(BTC_MINER_SHA256D_SOURCES src/sha256d.c)
set(BTC_MINER_ARM_SHA2_FLAGS "")

check_c_compiler_flag("-march=native" HAS_MARCH_NATIVE)
check_c_compiler_flag("-mcpu=native" HAS_MCPU_NATIVE)
check_c_compiler_flag("-march=armv8-a+crypto" HAS_MARCH_ARMV8_CRYPTO)
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

target_compile_definitions(btc_bench PRIVATE
    OPENSSL_SUPPRESS_DEPRECATED
)

if(HAS_MARCH_NATIVE)
    target_compile_options(btc_bench PRIVATE -march=native)
endif()

if(HAS_ARM_SHA2_INTRINSICS)
    target_compile_definitions(btc_bench PRIVATE BTC_MINER_ARM_SHA2)
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

if(BTC_MINER_NATIVE AND HAS_MARCH_NATIVE)
    target_compile_options(btc_stratum PRIVATE -march=native)
endif()

target_compile_definitions(btc_stratum PRIVATE
    OPENSSL_SUPPRESS_DEPRECATED
)

if(HAS_ARM_SHA2_INTRINSICS)
    target_compile_definitions(btc_stratum PRIVATE BTC_MINER_ARM_SHA2)
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

target_link_libraries(btc_proxy PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::JANSSON
    Threads::Threads
)

if(WIN32)
    target_link_libraries(btc_proxy PRIVATE ws2_32)
endif()
