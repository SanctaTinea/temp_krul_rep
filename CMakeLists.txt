cmake_minimum_required(VERSION 3.22)

set(KRUL_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(KRUL_IS_TOP_LEVEL ON)
    project(Krul LANGUAGES C)
    include(CTest)
endif()

add_subdirectory(serialization)
add_subdirectory(transport)
add_subdirectory(command-interface)

option(KRUL_BUILD_HOST_TOOLS [=[Build Krul host examples]=] ${KRUL_IS_TOP_LEVEL})
if(KRUL_BUILD_HOST_TOOLS)
    add_subdirectory(Tools)
endif()

option(KRUL_BUILD_DOCS [=[Add the Doxygen documentation target]=] OFF)
if(KRUL_BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT [=[Generating Krul API documentation]=]
        VERBATIM
    )
endif()
