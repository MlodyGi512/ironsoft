cmake_minimum_required(VERSION 3.19)
project(ironsoft_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PahoMqttCpp REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

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

target_compile_options(ironsoft_backend PRIVATE -Wall -Wextra -Wpedantic)
