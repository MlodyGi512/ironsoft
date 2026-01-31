cmake_minimum_required(VERSION 3.21)
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

if (WIN32)
  add_custom_command(TARGET IronsoftGui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:IronsoftGui>
      $<TARGET_FILE_DIR:IronsoftGui>
    COMMAND_EXPAND_LISTS
    COMMENT "Copying dependent runtime DLLs next to IronsoftGui.exe"
  )

  if (DEFINED Qt6_DIR)
    get_filename_component(_qt6_prefix "${Qt6_DIR}/../.." ABSOLUTE)
    find_program(QT_DEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt6_prefix}/bin")
    if (QT_DEPLOYQT_EXECUTABLE)
      add_custom_command(TARGET IronsoftGui POST_BUILD
        COMMAND "${QT_DEPLOYQT_EXECUTABLE}" --no-translations --no-system-dxc "$<TARGET_FILE:IronsoftGui>"
        COMMENT "Running windeployqt to copy Qt runtime files"
      )
    else()
      message(WARNING "windeployqt.exe not found; copy Qt runtime DLLs manually")
    endif()
  endif()
endif()
