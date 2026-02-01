# Ekinox Service Test Plan

This checklist validates the standalone `ekinox_service` process on Linux/Raspberry Pi. It assumes Mosquitto is reachable on the same LAN and the config file `config/ekinox.json` points to the desired broker and drone ID.

> **REST endpoints**: the service talks to `http://<ekinox-ip>:<rest_port>/api/v1/dataLogger`.
> - `GET /api/v1/dataLogger` returns the status JSON (fields: `status`, `mode`, `sessionName`, `writeSpeed`, ...).
> - `POST /api/v1/dataLogger/start` requires `Content-Type: application/json` and body `{"sessionName":"<name>"}`.
> - `POST /api/v1/dataLogger/stop` accepts an empty JSON body (`{}`) and stops the current session.
> Endpoints such as `/api/logger/status` are invalid and must not be used (404 response).

---
## Dependencies (RPi/Debian)
```bash
sudo apt-get update && sudo apt-get install -y libcurl4-openssl-dev
```

## 1. Build
```bash
cd ~/ironsoft-uav
cmake -S services/ekinox -B build-ekinox -DCMAKE_BUILD_TYPE=Release
cmake --build build-ekinox -j
```

## 2. Launch telemetry taps
In terminal A, subscribe to all drone topics:
```bash
mosquitto_sub -h 127.0.0.1 -t "ironsoft/uav/drone001/#" -v
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
mosquitto_pub -h 127.0.0.1 -t ironsoft/uav/drone001/cmd \
  -m '{"type":"ping","id":"cmd-1","ts":'$((`date +%s`))'}'
```
Expect `ack` with `type:"ping"`, `ok:true`, `http_code:200`, `message:"pong"`.

### 4.2 Logger start
```bash
mosquitto_pub -h 127.0.0.1 -t ironsoft/uav/drone001/cmd \
  -m '{"type":"logger.start","id":"cmd-2","ts":'$((`date +%s`))',"sessionName":"IronSoft_demo"}'
```
- `ack` contains `type:"logger.start"`, `ok:true`, `http_code:200`.
- `ack.message` is `"recording started"` and `ack.err` is empty when the REST call succeeded.
- `status` retained payload shows `mode:"RECORDING"`, `recording:true`, `api_ok:true`, and by default the generated session name follows `IronSoft_YYYYMMDD_HHMMSS` (UTC) if none was provided.

### 4.3 Logger stop
```bash
mosquitto_pub -h 127.0.0.1 -t ironsoft/uav/drone001/cmd \
  -m '{"type":"logger.stop","id":"cmd-3","ts":'$((`date +%s`))'}'
```
- `ack` reports `type:"logger.stop"`, `ok:true`, `message:"recording stopped"`.
- `status` returns to `mode:"IDLE"`, `recording:false`.

### 4.4 Logger status
```bash
mosquitto_pub -h 127.0.0.1 -t ironsoft/uav/drone001/cmd \
  -m '{"type":"logger.status","id":"cmd-4","ts":'$((`date +%s`))'}'
```
- `ack` message mirrors the REST `status` field (e.g., `"recording"` or `"ready"`) and `ack.err` stays empty when HTTP 200 is returned.
- `status.api_ok` remains true when the sensor replies.

### 4.5 Invalid transition
While IDLE, send another `logger.stop` and expect `ack` with `ok:false`, `message:"no active session"`, and `err` echoing the REST body (for example `HTTP_409`); `status.last_error` updates.

## 5. Shutdown behavior
Press `Ctrl+C` in terminal B.
- Service publishes retained `presence` with `state:"offline"` and `reason:"shutdown"`.
- `status` retained payload shows `link_alive:false` and `state:"DISCONNECTED"`.

## 6. SBG UDP link (hardware required)
1. Ensure the SBG Ekinox sensor is reachable from the Raspberry Pi (default IP `192.168.100.2`).
2. When the service starts, `presence` should transition from `offline` (`reason:"connecting"`) to `online` once the UDP link is established; `status.state` switches to `IDLE` and `api_ok:true`.
3. Unplug the Ekinox Ethernet cable (or power off the device):
   - Within `rx_dead_ms` (~1.5 s) the service publishes `presence` `offline` with `reason:"rx timeout"` and `status` flips to `CONNECTING` with `link_alive:false`, `api_ok:false`.
4. Plug the sensor back in: observe `presence` returning to `online` and `status` back to `IDLE` without restarting the process.

## 7. Logger MQTT soak (hardware required)
1. With the sensor recording, yank Ethernet/power:
   - `presence` flips to offline with `reason:"rx timeout"`.
   - `logger.status` commands now return `ok:false`, `err:"sensor offline"`, and `status.api_ok:false`.
2. Restore the link and issue `logger.start` again to confirm the FSM recovers to `RECORDING` without restarting the service.

Record console logs and MQTT captures for regressions before finishing the test.
