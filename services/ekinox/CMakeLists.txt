cmake_minimum_required(VERSION 3.16)
project(ironsoft_ekinox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SBG_ECOM_ROOT "" CACHE PATH "Path to the sbgECom SDK root (Linux only)")

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
find_package(PahoMqttCpp CONFIG REQUIRED)
find_package(Threads REQUIRED)

set(EKINOX_HAS_SBG 0)
set(EKINOX_SBG_TARGET "")

if(UNIX)
    if(NOT SBG_ECOM_ROOT)
        message(FATAL_ERROR "SBG_ECOM_ROOT not set. Configure with -DSBG_ECOM_ROOT=/home/ironsoft/sbgECom")
    endif()
    if(NOT EXISTS "${SBG_ECOM_ROOT}")
        message(FATAL_ERROR "SBG_ECOM_ROOT path does not exist: ${SBG_ECOM_ROOT}")
    endif()

    add_subdirectory(${SBG_ECOM_ROOT} ${CMAKE_BINARY_DIR}/_deps/sbgECom EXCLUDE_FROM_ALL)

    if(TARGET sbgECom)
        set(EKINOX_SBG_TARGET sbgECom)
    elseif(TARGET sbgEComLib)
        set(EKINOX_SBG_TARGET sbgEComLib)
    elseif(TARGET sbgECom::sbgECom)
        set(EKINOX_SBG_TARGET sbgECom::sbgECom)
    else()
        get_property(_allTargets GLOBAL PROPERTY TARGETS)
        message(FATAL_ERROR "Cannot find sbgECom target after add_subdirectory. Available targets: ${_allTargets}")
    endif()

    set(EKINOX_HAS_SBG 1)
endif()

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
    src/ekinox_logger_api.cpp
    src/ekinox_udp_session.cpp
    src/sbg_smoke.cpp
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

if(EKINOX_HAS_SBG)
    target_link_libraries(ekinox_service PRIVATE ${EKINOX_SBG_TARGET})
    target_include_directories(ekinox_service PRIVATE
        ${SBG_ECOM_ROOT}
        ${SBG_ECOM_ROOT}/src
        ${SBG_ECOM_ROOT}/src/commands
        ${SBG_ECOM_ROOT}/common
        ${SBG_ECOM_ROOT}/common/interfaces
        ${SBG_ECOM_ROOT}/common/network
    )
endif()

target_compile_definitions(ekinox_service PRIVATE DEKINOX_HAS_SBG=${EKINOX_HAS_SBG})
