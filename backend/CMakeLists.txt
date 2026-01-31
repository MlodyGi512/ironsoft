cmake_minimum_required(VERSION 3.19)
project(ironsoft_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ORTHO_ENABLE_GRPC "Enable gRPC transport" OFF)

find_package(PahoMqttCpp REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

if(ORTHO_ENABLE_GRPC)
  find_package(gRPC CONFIG REQUIRED)
endif()

add_executable(ironsoft_backend
  src/main.cpp
  src/mqtt_client.cpp
  src/state.cpp
)

target_include_directories(ironsoft_backend PRIVATE
  ${JSONCPP_INCLUDE_DIRS}
  include
)

target_link_libraries(ironsoft_backend PRIVATE
  PahoMqttCpp::paho-mqttpp3
  ${JSONCPP_LIBRARIES}
)

if(ORTHO_ENABLE_GRPC)
  target_link_libraries(ironsoft_backend PRIVATE gRPC::grpc++)
endif()

target_compile_definitions(ironsoft_backend PRIVATE ORTHO_ENABLE_GRPC=$<BOOL:${ORTHO_ENABLE_GRPC}>)

target_compile_options(ironsoft_backend PRIVATE -Wall -Wextra -Wpedantic)
