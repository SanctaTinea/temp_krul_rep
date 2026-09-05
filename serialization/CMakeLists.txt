cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(surge LANGUAGES C)
    include(CTest)
endif()

add_library(serde)
add_library(surge::serde ALIAS serde)
set_property(TARGET serde PROPERTY C_STANDARD 23)
set_property(TARGET serde PROPERTY C_STANDARD_REQUIRED ON)

target_sources(serde
    PRIVATE
        src/serde.c
        src/serde_bson.c
        src/serde_cbor.c
        src/serde_json.c
)

target_include_directories(serde
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

if(BUILD_TESTING)
    add_executable(serde_json_test tests/serde_json_test.c)
    target_link_libraries(serde_json_test PRIVATE surge::serde)
    add_test(NAME serde_json_test COMMAND serde_json_test)

    add_executable(serde_bson_test tests/serde_bson_test.c)
    target_link_libraries(serde_bson_test PRIVATE surge::serde)
    add_test(NAME serde_bson_test COMMAND serde_bson_test)

    add_executable(serde_cbor_test tests/serde_cbor_test.c)
    target_link_libraries(serde_cbor_test PRIVATE surge::serde)
    add_test(NAME serde_cbor_test COMMAND serde_cbor_test)
endif()
