cmake_minimum_required(VERSION 3.0)
project(linux_c_security_gateway C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(linux_c_security_gateway main.c
        02_tcp_udp_proxy/config.c
        02_tcp_udp_proxy/config.h
        02_tcp_udp_proxy/log.c
        02_tcp_udp_proxy/log.h
        02_tcp_udp_proxy/main_service.c
        02_tcp_udp_proxy/main_service.h)

configure_file(server.conf server.conf COPYONLY)

target_compile_definitions(linux_c_security_gateway PRIVATE
        _DEFAULT_SOURCE
        _POSIX_C_SOURCE=200809L
        _XOPEN_SOURCE=700)
target_link_libraries(linux_c_security_gateway PRIVATE Threads::Threads)

add_executable(echo_test tools/echo_test.c)
target_compile_definitions(echo_test PRIVATE
        _DEFAULT_SOURCE
        _POSIX_C_SOURCE=200809L)
