# IronSoft UAV – MQTT MVP (backend C++ on RPi, GUI Qt Widgets on Windows)

This repository contains a minimal, working MVP to validate MQTT connectivity between:
- **RPi-DRONE backend (C++ / Paho MQTT C++)**
- **Windows 11 GUI (Qt 6 Widgets + Paho MQTT C++ via vcpkg)****

It implements:
- MQTT connect/reconnect
- **LWT** (offline) + **presence online** (retained, QoS1)
- **heartbeat** (QoS0)
- **status** (retained, QoS1)
- **cmd → ack** (QoS1), including a **PING** command from GUI

Topics are namespaced by drone id: `ironsoft/uav/<drone_id>/...` (default `drone01`).

---

## Quick start – Raspberry Pi (backend)

### 1) Install dependencies
```bash
sudo apt update
sudo apt install -y git cmake g++ ninja-build pkg-config \
  mosquitto mosquitto-clients \
  libssl-dev libjsoncpp-dev \
  libpaho-mqtt-dev libpaho-mqttpp3-dev
```

> If your distro does not have `libpaho-mqttpp3-dev`, see `docs/rpi_setup.md` for building Paho from sources.

### 2) Ensure Mosquitto listens on LAN
Create:
```bash
sudo nano /etc/mosquitto/conf.d/ironsoft.conf
```
Paste:
```
listener 1883 0.0.0.0
allow_anonymous true
```
Restart:
```bash
sudo systemctl restart mosquitto
ss -lntp | grep 1883
```
You should see `0.0.0.0:1883` (not only `127.0.0.1:1883`).

### 3) Build and run backend
```bash
cd backend
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ironsoft_backend --config ../config/mqtt.dev.json
```

---

## Quick start – Windows 11 (GUI)

1. **Install dependencies**
   - vcpkg (x64-windows triplet) with: `paho-mqttpp3`, `paho-mqtt`, `openssl`, `qtbase` (Qt 6.6+).
   - Qt 6 MSVC kit (e.g. `C:\Qt\6.6.3\msvc2022_64`).
2. **Configure + build (Release)**
   ```powershell
   cmake -S gui -B gui/build -G "Visual Studio 17 2022" -A x64 ^
     -DCMAKE_TOOLCHAIN_FILE="C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake" ^
     -DVCPKG_TARGET_TRIPLET=x64-windows ^
     -DQt6_DIR="C:/Qt/6.6.3/msvc2022_64/lib/cmake/Qt6" ^
     -DCMAKE_BUILD_TYPE=Release
   cmake --build gui/build --config Release --target IronsoftGui
   ```
3. **Run** `gui\build\Release\IronsoftGui.exe`

The GUI CMakeLists runs `windeployqt` and copies all dependent DLLs next to the EXE, so a clean Windows VM can launch it without editing `PATH`. The copied runtime set includes:
   - Qt: `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, `Qt6Network.dll`, `Qt6Svg.dll` (if present) and the `platforms\qwindows.dll` plugin.
   - Paho MQTT: `paho-mqttpp3.dll`, `paho-mqtt3as.dll`, `paho-mqtt3a.dll`.
   - TLS deps (via vcpkg OpenSSL): `libssl-3-x64.dll`, `libcrypto-3-x64.dll`.

---

## Config
- `config/mqtt.dev.json` – LAN dev (1883, no TLS)
- `config/mqtt.vps.json` – template for VPS (8883, TLS enabled)
- `config/app.json` – heartbeat/status periods

---

## MVP behavior you can test
1. Start Mosquitto on RPi
2. Start backend – presence becomes **online**
3. Start GUI – it shows **Connected** and **ONLINE**
4. Kill backend (Ctrl+C) – GUI turns **OFFLINE** (via LWT)
5. Click **PING** – GUI receives `ack` with `"pong"`

