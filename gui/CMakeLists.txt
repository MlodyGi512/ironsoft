cmake_minimum_required(VERSION 3.19)
project(IronsoftGui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets)
find_package(PahoMqttCpp CONFIG REQUIRED)

qt_standard_project_setup()

qt_add_executable(IronsoftGui WIN32
  src/main.cpp
  src/mainwindow.cpp
  src/mainwindow.h
  src/mqtt_paho_client.cpp
  src/mqtt_paho_client.h
  src/mainwindow.ui
  src/app.qrc
)

target_link_libraries(IronsoftGui PRIVATE
  Qt::Core
  Qt::Widgets
  PahoMqttCpp::paho-mqttpp3
)

target_compile_definitions(IronsoftGui PRIVATE PAHO_MQTTPP_IMPORTS)

target_include_directories(IronsoftGui PRIVATE src)
