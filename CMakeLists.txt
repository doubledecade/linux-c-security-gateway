cmake_minimum_required(VERSION 3.0)
project(linux_c_security_gateway C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(linux_c_security_gateway main.c
        01_service_framework/config.c
        01_service_framework/config.h
        01_service_framework/log.c
        01_service_framework/log.h
        01_service_framework/main_service.c
        01_service_framework/main_service.h)

configure_file(test.conf test.conf COPYONLY)

target_compile_definitions(linux_c_security_gateway PRIVATE _DEFAULT_SOURCE)
target_link_libraries(linux_c_security_gateway PRIVATE Threads::Threads)
