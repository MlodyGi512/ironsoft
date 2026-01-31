cmake_minimum_required(VERSION 3.16)
project(ironsoft_ekinox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

add_library(ekinox_config STATIC
    src/ekinox_config.cpp
)

target_include_directories(ekinox_config
    PUBLIC
        include
    PRIVATE
        ${JSONCPP_INCLUDE_DIRS}
)

target_link_libraries(ekinox_config
    PRIVATE
        ${JSONCPP_LIBRARIES}
)

add_executable(ekinox_config_dump
    src/config_dump_main.cpp
)

target_link_libraries(ekinox_config_dump
    PRIVATE
        ekinox_config
)
