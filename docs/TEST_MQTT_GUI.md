# End-to-end MQTT GUI Test Plan

This checklist verifies the full chain **PC GUI ? Mosquitto ? Raspberry Pi backend**. Follow it top-to-bottom on a clean LAN.

---
## 1. Prerequisites
- Raspberry Pi with Ubuntu/Debian, reachable from Windows PC.
- Mosquitto installed (`sudo apt install mosquitto mosquitto-clients`).
- Backend built on Pi (`ironsoft_backend`).
- Windows GUI build ready (`IronsoftGui.exe`).
- Shared drone ID (default `drone01`).

Sample MQTT config for the GUI (save as `mqtt.rpi.json` next to the EXE):
```json
{
  "broker": {
    "host": "192.168.1.42",
    "port": 1883
  },
  "client": {
    "drone_id": "drone01",
    "keepalive_s": 20
  },
  "auth": {
    "username": "",
    "password": ""
  },
  "tls": {
    "enabled": false,
    "ca_file": ""
  }
}
```
Replace `192.168.1.42` with the Pi address and update credentials if using authenticated brokers.

---
## 2. Start broker on Raspberry Pi
```bash
sudo systemctl stop mosquitto
sudo mosquitto -c /etc/mosquitto/mosquitto.conf -v
```
(Use a separate terminal or `tmux` pane; keep it running to observe logs.)

If you prefer a custom listener file:
```bash
echo -e "listener 1883 0.0.0.0\nallow_anonymous true" | sudo tee /etc/mosquitto/conf.d/ironsoft.conf
sudo systemctl restart mosquitto
```
Verify:
```bash
ss -lntp | grep 1883
```

---
## 3. Launch backend on Raspberry Pi
```bash
cd ~/ironsoft-uav/backend
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ironsoft_backend --config ../config/mqtt.dev.json
```
Expect console output:
- `[mqtt] connecting to ...`
- `subscribed ...`
- Heartbeat logs every second.

---
## 4. Launch GUI on Windows PC
1. Copy `mqtt.rpi.json` into the GUI folder.
2. Run `IronsoftGui.exe`.
3. Browse for `mqtt.rpi.json` via **Config ? Browse**.
4. Click **Connect**.

**Expected UI:**
- Status label: `Connecting…` ? `Connected`.
- Presence LED: green.
- Backend LED: green (after heartbeat).
- Mode/API/Error fields reflect backend status (`IDLE`, `API OK`, no error).

---
## 5. Scenario checklist

### 5.1 Ping/ACK round-trip
- Click **PING**.
- Log shows `-> cmd ping id=...` and `pong id=... (XX ms)`.
- "Last Ping RTT" displays the measured time in ms.

### 5.2 Broker outage / reconnect
1. On Pi, stop broker:
   ```bash
   sudo systemctl stop mosquitto
   ```
2. GUI should switch to `Connecting…` and keep retrying (presence + backend LEDs go red).
3. Restart broker:
   ```bash
   sudo systemctl start mosquitto
   ```
4. GUI reconnects automatically, returning to `Connected` with LEDs green.

### 5.3 Backend crash watchdog
1. With broker running, stop backend (Ctrl+C in backend terminal).
2. GUI stays `Connected` (MQTT link alive) but:
   - Presence LED turns red (offline retained message).
   - Backend LED becomes yellow then red after 3?s (heartbeat timeout).
   - Status panel freezes until backend returns.
3. Restart backend using the command from section 3.
4. GUI updates back to `Mode = IDLE`, LEDs green, heartbeat counter resumes.

### 5.4 Unsolicited ACK (optional)
From any machine with mosquitto-clients:
```bash
mosquitto_pub -h 192.168.1.42 -t ironsoft/uav/drone01/ack -m '{"id":"fake","ok":true,"message":"pong"}'
```
GUI log shows `unsolicited ack id=fake ...`; RTT field remains unchanged.

---
## 6. Pass criteria
- All steps above reproduce as described without manual DLL copying or config tweaks beyond IP/drone ID.
- GUI transitions correctly for connect/disconnect/heartbeat/ping.
- Backend and broker commands succeed without additional intervention.

Record any deviations plus logs (GUI text log, backend console, mosquitto output) before closing the test.
