add_executable(krul_host_server krul_host_server.c)
target_link_libraries(krul_host_server
    PRIVATE krul::protocol krul::transport)

if(WIN32)
    target_link_libraries(krul_host_server PRIVATE ws2_32)
endif()
