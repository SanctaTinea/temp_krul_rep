cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(krul_transport LANGUAGES C)
    include(CTest)
endif()

add_library(krul_transport)
add_library(krul::transport ALIAS krul_transport)
set_property(TARGET krul_transport PROPERTY C_STANDARD 23)
set_property(TARGET krul_transport PROPERTY C_STANDARD_REQUIRED ON)

target_sources(krul_transport PRIVATE src/krul_transport.c)
target_include_directories(krul_transport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

if(BUILD_TESTING)
    add_executable(krul_transport_test tests/krul_transport_test.c)
    target_link_libraries(krul_transport_test PRIVATE krul::transport)
    add_test(NAME krul_transport_test COMMAND krul_transport_test)
endif()
