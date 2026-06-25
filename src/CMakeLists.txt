# make the current directory an include path.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fno-exceptions -fno-rtti)
elseif(MSVC)
    add_compile_options(/EHs-c- /GR-)
endif()

add_library(waler STATIC
    waler/util.cpp
)

target_include_directories(waler
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(pktgen
    pktgen/pktgen.cpp
)

target_link_libraries(pktgen
    PRIVATE
        waler
)
