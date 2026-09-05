

cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(krul LANGUAGES C)
    include(CTest)

    set(SURGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../serialization"
        CACHE PATH "Path to the Surge/serde source tree")
    if(NOT TARGET serde)
        if(EXISTS "${SURGE_SOURCE_DIR}/CMakeLists.txt")
            add_subdirectory("${SURGE_SOURCE_DIR}"
                             "${CMAKE_CURRENT_BINARY_DIR}/surge")
        else()
            message(FATAL_ERROR
                "Krul requires Surge. Set SURGE_SOURCE_DIR to its source tree.")
        endif()
    endif()
endif()

add_library(krul_protocol)
add_library(krul::protocol ALIAS krul_protocol)
set_property(TARGET krul_protocol PROPERTY CMAKE_C_STANDARD 23)
set_property(TARGET krul_protocol PROPERTY CMAKE_C_STANDARD_REQUIRED ON)

target_sources(krul_protocol
    PRIVATE
        src/krul.c
        src/krul_discovery.c
        src/krul_dispatch.c
        src/krul_runtime.c
        src/krul_result.c
        src/krul_schema.c
)
target_include_directories(krul_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
target_link_libraries(krul_protocol PUBLIC surge::serde)

if(BUILD_TESTING)
    add_executable(krul_host_test tests/krul_host_test.c)
    target_link_libraries(krul_host_test PRIVATE krul::protocol)
    add_test(NAME krul_host_test COMMAND krul_host_test)
endif()
