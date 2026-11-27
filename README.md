# ZenRoom Embedded App

Firmware and embedded ML prototype for the ZenRoom temperature early-warning demo. The C++ inference runs on ESP32; this repository does not train or call a cloud ML service.

There are two firmwares with the same Edge contract:

| Firmware | Where | Used for |
|---|---|---|
| Arduino sketch | `arduino/zenroom_esp32/` | **The physical device.** DHT22, PIR, HC-SR04, potentiometer, buzzer, door LEDs and 3 buttons; offline batch queue in LittleFS. Same sketch as the public Wokwi simulation: https://wokwi.com/projects/481710853765583873 |
| PlatformIO firmware | `src/main.cpp`, envs `zenroom` and `zenroom-sim` | Wokwi CI scenarios and model evidence (`verificar_demo.py`). No HC-SR04, no button actions and no offline queue |

The physical-device acceptance checklist is [`docs/ACTA-DISPOSITIVO-REAL.md`](docs/ACTA-DISPOSITIVO-REAL.md).

## End-to-end contract

Both firmwares publish **one 10-second batch every 10 seconds (six per UTC minute)** and run the forecast model once per model step (`SAMPLE_MS` in PlatformIO, `MODEL_STEP_MS` in the sketch; 60 s on the device, 10 s in simulation):

- `comfort/<site>/room/<room>/readings`, QoS 1 — one batch:
  ```json
  {"device_id":"zr-sala-03","room_id":"sala-03","boot_id":"<32 hex, new on every boot>","seq":1,
   "ts":"2026-11-18T11:07:00Z","window_s":10,"fw_version":"zenroom-forecast-v3",
   "acoustic":{"laeq_1s":[52.0, ... 10 values],"lmax":52.0,"lmin":52.0,
               "hist_min_db":30.0,"hist_bin_db":1.0,"hist":[... 70 counts, 80 in total]},
   "climate":{"temp_c":24.00,"rh_pct":55.0},
   "presence":{"state":"occupied","occupied_s":10.00,"transitions":0}}
  ```
  Noise is sampled every 125 ms: 8 windows give each `laeq_1s` value (energy average) and the 80 windows fill the 70-bin, 1 dB histogram from 30 dB that the Edge uses for L10/L50/L90. Once the clock is synced by NTP, each batch starts on a UTC second multiple of 10 (`:00`, `:10` … `:50`) and `ts` is that start, so every Edge minute gets its six batches (`quality.batches = 6`). Before NTP the batch carries `1970-01-01T00:00:00Z` and the Edge stores it as `clock_suspect` with its reception time. `boot_id` plus `seq` lets the Edge deduplicate retries and count lost batches across reboots.
- `comfort/<site>/device/<device>/forecast` — `device_id`, `probability` (0–1), `horizon_min` (15), `temperature_limit_c`, and `model_version`.
- `comfort/<site>/device/<device>/status` — `online`, retained; the broker publishes `offline` (last will) when the device drops.
- The device subscribes to `comfort/<site>/device/<device>/cmd` and accepts `{"temp_limit": 18..30}`.

The Edge must use the same `SITE_CODE`, `MQTT_PREFIX`, and broker. It attaches the forecast to that room's minute aggregate and sends it to Cloud with the existing reading. Cloud returns it on the latest room reading for the web and mobile clients. A room and Edge/Cloud site must exist before the first batch is accepted.

`ROOM_ID`, `DEVICE_ID`, `TOPIC_BASE`, and `SAMPLE_MS` can be overridden as PlatformIO build flags. `TOPIC_BASE` is the common `comfort/<site>` prefix. Configure a private broker and TLS for a real installation; the demo's HiveMQ public broker and port 1883 are only for simulation.

## Model and evidence

The compact 5-16-16-1 network uses 385 parameters and predicts the chance that an occupied room crosses its temperature limit in 15 minutes. It is an early-warning probability, not an absolute future-temperature estimate. The ESP32 uses a probability threshold of 0.85.

The current notebook and validation data are based on the team's simulator. Reported 91% recall and 93% precision are simulator results, not measurements from a physical room. The weights export check is reproducible with:

```sh
python verificar_demo.py modelo
```

The Wokwi web folder preserves the accelerated simulator sketch and its existing captured evidence. Hardware still needs calibration and real-data evaluation; the current prototype substitutes a DHT22 for SHT31, a potentiometer for INMP441, and PIR (plus HC-SR04 in the sketch) for LD2410C. The acoustic batch has the Edge's full format (ten 1-second levels and the 70-bin histogram), but the levels come from the potentiometer (30–90 dB(A)), not from a microphone; this is not production sensor telemetry.

## How to build and flash

### Arduino sketch (physical device)

Requirements: Arduino IDE 2 or `arduino-cli`, ESP32 core by Espressif **3.3.x**, and the libraries `ArduinoMqttClient` (0.1.8), `DHT sensor library` (Adafruit, 1.4.x) and `Adafruit Unified Sensor`.

1. Copy `arduino/zenroom_esp32/DeviceSecrets.example.h` to `DeviceSecrets.h` in the same folder and fill in Wi-Fi, broker IP of the Edge machine, broker user/password, `SITE_CODE`, `DEVICE_ID` and `ROOM_ID`. `DeviceSecrets.h` is in `.gitignore`: never commit it.
2. Board **DOIT ESP32 DEVKIT V1** (`esp32:esp32:esp32doit-devkit-v1`). Its fixed partition table already has the SPIFFS partition that LittleFS uses for the offline queue. With "ESP32 Dev Module" choose a partition scheme with SPIFFS ("Default 4MB with spiffs").
3. Compile and upload:
   ```sh
   arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 arduino/zenroom_esp32
   arduino-cli upload  --fqbn esp32:esp32:esp32doit-devkit-v1 -p COM5 arduino/zenroom_esp32
   arduino-cli monitor -p COM5 -c baudrate=115200
   ```
   If the upload stays on `Connecting...`, hold the **BOOT** button until it starts writing.

Current size (core 3.3.12): 1,012,376 bytes of flash (77 %), 50,108 bytes of RAM (15 %).

`python arduino/wokwi/generar_wokwi.py` builds a single-file copy of the sketch with simulation secrets (Wokwi-GUEST, HiveMQ, 10 s model step) in `arduino/wokwi/salida/` to paste into wokwi.com.

### PlatformIO firmware (simulation)

```sh
python -m pip install platformio
pio run -e zenroom -e zenroom-sim              # both envs
pio run -e zenroom -t upload && pio device monitor
```

`zenroom` takes Wi-Fi and broker from `secrets.ini` (copy `secrets.ini.example`; not versioned). `zenroom-sim` is the Wokwi profile (`wokwi.toml`). Current size: RAM 48,732 bytes (14.9 %), flash 776,093 bytes (59.2 %).

The model verification does not require a device. A real MQTT integration test requires the Edge and Cloud services, a broker reachable by both, a configured room, and a device code matching `DEVICE_ID`. Without the board, validate the path with Wokwi and the team's Edge simulator.

## Branching

Submit device changes from feature branches through pull requests to the temporary `TF` branch.
After the full integration slice passes validation, promote it through `TF` → `develop` → `main`
using pull requests; do not commit directly to `main`.
