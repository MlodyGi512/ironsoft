# MQTT topics (MVP)

Drone id: `drone01` (configurable)

Prefix:
- `ironsoft/uav/<drone_id>/`

## Published by DRONE (backend) → subscribed by GUI
- `.../presence`  (QoS1, retained)
  - online (published on connect): `{"state":"online","ts":...}`
  - offline (LWT, retained): `{"state":"offline","ts":...,"reason":"lwt"}`
- `.../status`    (QoS1, retained)  – e.g. `{"mode":"IDLE","api_ok":true,"last_error":""}`
- `.../heartbeat` (QoS0)            – e.g. `{"seq":12,"uptime_s":33}`
- `.../ack`       (QoS1)            – e.g. `{"id":"cmd-0001","ok":true,"message":"pong"}`

## Published by GUI → subscribed by DRONE (backend)
- `.../cmd` (QoS1) – e.g. `{"id":"cmd-0001","type":"ping","params":{}}`

