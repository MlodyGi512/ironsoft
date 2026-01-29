# Windows GUI setup (Qt Widgets + Paho MQTT C++ via vcpkg)

This GUI uses **Qt 6 Widgets** for UI and **Paho MQTT C++** for MQTT (no Qt MQTT module required).

## Prereqs
- Visual Studio 2022 (MSVC x64) + CMake
- Qt 6.10.1 `msvc2022_64`
- vcpkg

## 1) Install Paho with vcpkg
```bat
cd /d C:\Users\danie\source\repos
git clone https://github.com/microsoft/vcpkg
cd vcpkg
bootstrap-vcpkg.bat
vcpkg install paho-mqttpp3[ssl]:x64-windows
```

## 2) Configure + build GUI
From `ironsoft-uav\gui`:

```bat
cd /d C:\Users\danie\source\repos\ironsoft-uav\gui
rmdir /s /q build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DQt6_DIR="C:\Qt\6.10.1\msvc2022_64\lib\cmake\Qt6" ^
  -DCMAKE_TOOLCHAIN_FILE="C:\Users\danie\source\repos\vcpkg\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release
```

## 3) Run
```bat
.\build\Release\IronsoftGui.exe
```

If you see missing Qt DLLs, run:
```bat
"C:\Qt\6.10.1\msvc2022_64\bin\windeployqt.exe" "C:\Users\danie\source\repos\ironsoft-uav\gui\build\Release\IronsoftGui.exe"
```
