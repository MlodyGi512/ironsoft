# Ekinox Service Test Plan

This checklist validates the standalone `ekinox_service` process on Linux/Raspberry Pi. It assumes Mosquitto is reachable on the same LAN and the config file `config/ekinox.json` points to the desired broker and drone ID.

---
## 1. Build
```bash
cd ~/ironsoft-uav
cmake -S services/ekinox -B build-ekinox -DCMAKE_BUILD_TYPE=Release
cmake --build build-ekinox -j
```

## 2. Launch telemetry taps
In terminal A, subscribe to all ekinox topics:
```bash
mosquitto_sub -h 127.0.0.1 -t "ironsoft/uav/drone001/ekinox/#" -v
```
Adjust host/drone ID to match `config/ekinox.json`.

## 3. Start the service
In terminal B:
```bash
cd ~/ironsoft-uav
./build-ekinox/ekinox_service --config config/ekinox.json
```
Expected:
- `mosquitto_sub` prints retained `presence` (state `online`) and `status`.
- Heartbeat messages arrive every second.
- Console logs show `[ekinox] Connected` and `state -> IDLE`.

## 4. Command tests
Use terminal C for MQTT publishes (replace broker address if needed).

### 4.1 Ping
```bash
mosquitto_pub -h 127.0.0.1 -t ironsoft/uav/drone001/ekinox/cmd \
  -m '{"type":"ping","id":"cmd-1"}'
```
Expect `ack` with `ok:true` and `message:"pong"`.

### 4.2 Start logger stub
```bash
mosquitto_pub -h 127.0.0.1 -t ironsoft/uav/drone001/ekinox/cmd \
  -m '{"type":"start_logger","id":"cmd-2"}'
```
- `ack` reports `logger starting`.
- `status` transitions `IDLE ? STARTING ? RECORDING` and sets `recording_active:true`.

### 4.3 Stop logger stub
```bash
mosquitto_pub -h 127.0.0.1 -t ironsoft/uav/drone001/ekinox/cmd \
  -m '{"type":"stop_logger","id":"cmd-3"}'
```
- `ack` reports `logger stopping`.
- `status` transitions `RECORDING ? STOPPING ? IDLE` and clears `recording_active`.

### 4.4 Invalid transition
While IDLE, send another `stop_logger` and expect `ack` with `ok:false` and `error:"INVALID_STATE"`; `status.last_error` updates.

## 5. Shutdown behavior
Press `Ctrl+C` in terminal B.
- Service publishes retained `presence` with `state:"offline"` and `reason:"shutdown"`.
- `status` retained payload shows `link_alive:false` and `state:"DISCONNECTED"`.

Record console logs and MQTT captures for regressions before finishing the test.
