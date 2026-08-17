# make the current directory an include path.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fno-exceptions -fno-rtti)
elseif(MSVC)
    add_compile_options(/EHs-c- /GR-)
endif()

find_library(CIUL1_LIB ciul1 REQUIRED)
find_package(Threads REQUIRED)

add_library(waler STATIC
    waler/util.cpp
    waler/EfviInterface.cpp
    waler/EfviUdpSender.cpp
)

target_include_directories(waler
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(waler
    PUBLIC
        ${CIUL1_LIB}
        Threads::Threads
)

add_executable(pktgen
    pktgen/pktgen.cpp
)

target_link_libraries(pktgen
    PRIVATE
        waler
)

add_executable(exchange_strategy_bench
    bench/exchange_strategy.cpp
)

target_include_directories(exchange_strategy_bench
    PRIVATE
        ${CMAKE_SOURCE_DIR}/common
)

target_link_libraries(exchange_strategy_bench
    PRIVATE
        waler
)
