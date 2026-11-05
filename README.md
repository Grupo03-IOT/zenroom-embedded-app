# ZenRoom Embedded App

Firmware and embedded ML prototype for the ZenRoom temperature early-warning demo. The C++ inference runs on ESP32; this repository does not train or call a cloud ML service.

## End-to-end contract

The PlatformIO firmware publishes one room batch and one model forecast per sample:

- `comfort/site-1/room/sala-03/readings` — device, room, UTC timestamp, acoustic estimate, temperature, humidity, occupancy, sequence number.
- `comfort/site-1/device/zr-sala-03/forecast` — `device_id`, `probability` (0–1), `horizon_min` (15), `temperature_limit_c`, and `model_version`.

The Edge must use the same `SITE_CODE`, `MQTT_PREFIX`, and broker. It attaches the forecast to that room's minute aggregate and sends it to Cloud with the existing reading. Cloud returns it on the latest room reading for the web and mobile clients. A room and Edge/Cloud site must exist before the first batch is accepted.

`ROOM_ID`, `DEVICE_ID`, `TOPIC_BASE`, and `SAMPLE_MS` can be overridden as PlatformIO build flags. `TOPIC_BASE` is the common `comfort/<site>` prefix. Configure a private broker and TLS for a real installation; the demo's HiveMQ public broker and port 1883 are only for simulation.

## Model and evidence

The compact 5-16-16-1 network uses 385 parameters and predicts the chance that an occupied room crosses its temperature limit in 15 minutes. It is an early-warning probability, not an absolute future-temperature estimate. The ESP32 uses a probability threshold of 0.85.

The current notebook and validation data are based on the team's simulator. Reported 91% recall and 93% precision are simulator results, not measurements from a physical room. The weights export check is reproducible with:

```sh
python verificar_demo.py modelo
```

The Wokwi web folder preserves the accelerated simulator sketch and its existing captured evidence. Hardware still needs calibration and real-data evaluation; the current prototype substitutes a DHT22 for SHT31, a potentiometer for INMP441, and PIR for LD2410C. The PlatformIO firmware emits one 10-second batch per minute (the Edge expects six), so readings remain marked incomplete. Its acoustic payload is an estimated level rather than the Edge's full ten-second acoustic histogram; this is not production sensor telemetry.

## Build

```sh
python -m pip install platformio
pio run
```

The model verification does not require a device. A real MQTT integration test requires the Edge and Cloud services, a broker reachable by both, a configured room, and a device code matching `DEVICE_ID`. Without the board, validate the path with Wokwi and the team's Edge simulator.

## Branching

Submit device changes from feature branches through pull requests to the temporary `TF` branch.
After the full integration slice passes validation, promote it through `TF` → `develop` → `main`
using pull requests; do not commit directly to `main`.
