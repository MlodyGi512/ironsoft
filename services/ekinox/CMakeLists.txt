cmake_minimum_required(VERSION 3.16)
project(ironsoft_ekinox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
find_package(PahoMqttCpp CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(ekinox_config STATIC
    src/ekinox_config.cpp
)

target_include_directories(ekinox_config
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ekinox_config
    PUBLIC
        PkgConfig::JSONCPP
)

add_executable(ekinox_config_dump
    src/config_dump_main.cpp
)

target_link_libraries(ekinox_config_dump
    PRIVATE
        ekinox_config
)

add_executable(ekinox_service
    src/main.cpp
    src/ekinox_service.cpp
    src/ekinox_state.cpp
)

target_include_directories(ekinox_service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ekinox_service
    PRIVATE
        ekinox_config
        PkgConfig::JSONCPP
        PahoMqttCpp::paho-mqttpp3
        Threads::Threads
)
