# Ekinox MQTT Contract

This document defines the MQTT topics and JSON payloads that connect the Ekinox sensor service, backend, and GUI. The topic root is `ironsoft/uav/<drone_id>/ekinox`.

## Topics
- `.../presence`
  - **Direction:** service ? broker ? GUI/backend
  - **QoS:** 1 (retained)
  - **Purpose:** broadcasts whether the service process is alive, plus optional reason for shutdowns.
- `.../status`
  - **Direction:** service ? broker ? GUI/backend
  - **QoS:** 1
  - **Retained:** yes
  - **Purpose:** publishes the current high-level state so late subscribers immediately know sensor availability.
- `.../heartbeat`
  - **Direction:** service ? broker ? GUI/backend
  - **QoS:** 0
  - **Retained:** no
  - **Purpose:** frequent liveness pings that carry timing metadata for latency monitoring.
- `.../cmd`
  - **Direction:** GUI/backend ? service
  - **QoS:** 1
  - **Retained:** no
  - **Purpose:** issues control commands (ping + recording controls).
- `.../ack`
  - **Direction:** service ? GUI/backend
  - **QoS:** 1
  - **Retained:** no
  - **Purpose:** correlates responses to `cmd` via the `id` field.

## JSON Payloads
### Presence (`.../presence`)
```json
{
  "state": "online",
  "ts": 0,
  "reason": ""
}
```
- `state`: either `"online"` or `"offline"`.
- `ts`: Unix epoch seconds when the presence payload was emitted.
- `reason`: optional human-readable explanation (e.g., `"shutdown"`).

### Status (`.../status`)
```json
{
  "state": "...",
  "link_alive": true,
  "api_ok": true,
  "recording_active": false,
  "last_error": "",
  "last_error_ts": 0
}
```
- `state`: uppercase string defined by `ServiceState` (`DISCONNECTED`, `CONNECTING`, `IDLE`, `STARTING`, `RECORDING`, `STOPPING`, `ERROR`).
- `link_alive`: `true` if the UDP transport still receives frames within its timeout.
- `api_ok`: `true` when the Ekinox API responds without errors.
- `recording_active`: `true` when the device is currently logging IMU data.
- `last_error`: textual description of the most recent failure (empty when none).
- `last_error_ts`: Unix epoch seconds or milliseconds (implementation must document) of `last_error` update.

### Heartbeat (`.../heartbeat`)
```json
{
  "seq": 0,
  "rx_age_ms": 0
}
```
- `seq`: monotonically increasing integer per heartbeat publish.
- `rx_age_ms`: transport-specific estimate of how old the last Ekinox frame was when sampled.

### Command (`.../cmd`)
```json
{
  "id": "uuid-or-monotonic",
  "type": "ping|start_logger|stop_logger"
}
```
- `id`: caller-supplied identifier echoed by acknowledgments.
- `type`: command verb. `ping` should yield an `ack` immediately; `start_logger` / `stop_logger` toggle Ekinox logging.

### Acknowledgment (`.../ack`)
```json
{
  "id": "uuid-or-monotonic",
  "ok": true,
  "message": "",
  "error": ""
}
```
- `id`: copied from the triggering command.
- `ok`: indicates success (`true`) or failure (`false`).
- `message`: human-readable success text (non-empty only when `ok == true`).
- `error`: human-readable failure reason (non-empty only when `ok == false`). Only one of `message` / `error` should be non-empty per payload.

## GUI Expectations
- Treat `presence.state` + `status.state`/`link_alive` as primary indicators: when either breaks, show the Ekinox widget as disconnected (red) and display `last_error`.
- Show `recording_active` as a dedicated indicator or toggle so operators know whether logging is running.
- Surface `last_error` text verbatim near the sensor card and reset it when the service clears it (empty string). Keep the previous timestamp visible for troubleshooting.
