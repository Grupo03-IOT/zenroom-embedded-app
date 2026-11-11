// ZenRoom - firmware del montaje real (ESP32 clasico, Arduino-ESP32 3.x, DHT sensor library de Adafruit).
// Base: el sketch de Paula (firmware/zenroom_esp32 de edge-api, rama feature/iot-connection): sensores, pines,
// reglas de confort, botones, buzzer y cola de lotes en LittleFS se conservan.
// Cambios: (1) el envio es por MQTT (QoS 1, con estado online/offline retenido y testamento) en vez de HTTP,
// (2) aviso anticipado de calor con la red neuronal (ForecastModel.h), (3) WiFi y claves fuera del repositorio
// (DeviceSecrets.h), (4) el sondeo del HC-SR04 ya no bloquea 15 ms.
// Bibliotecas (Gestor de bibliotecas): "ArduinoMqttClient", "DHT sensor library" + "Adafruit Unified Sensor".
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoMqttClient.h>
#include <LittleFS.h>
#include <DHT.h>
#include <esp_system.h>
#include <time.h>
#include <math.h>
#include <atomic>
#include "DeviceConfig.h"
#include "DeviceSignals.h"
#include "ForecastModel.h"

DHT dht(DHT_PIN, DHT22);
WiFiClient wifiClient;
MqttClient mqtt(wifiClient);             // solo lo toca networkWorker
ForecastModel model;
QueueHandle_t forecastQueue = nullptr;   // loop() -> networkWorker, mensajes ya armados
constexpr size_t FORECAST_MSG = 240;
std::atomic<float> limitC{25.0f};
std::atomic<bool> forecastWarning{false};
std::atomic<unsigned> pendingBatches{0};
std::atomic<bool> mqttUp{false};
char topicReadings[96], topicStatus[96], topicForecast[96], topicCmd[96];
SemaphoreHandle_t fsMutex;
QueueHandle_t buzzerQueue = nullptr;
std::atomic<bool> automaticBuzzerAllowed{false};
bool fsReady = false, pwmReady = false;
String bootId;
uint32_t sequence = 0;
float temperature = NAN, humidity = NAN, distanceCm = NAN;
float noiseLevel = NAN;
unsigned potAdc = 0;
bool climateValid = false, occupied = false, detected = false;
uint32_t lastDetectionMs = 0, manualUntil = 0, mutedUntil = 0;
int manualPresence = 0;  // 0 automatico, 1 ocupado, 2 vacio
ComfortState currentComfort = ComfortState::NoData;
RedAlertLatch redAlert;
uint32_t redSinceMs = 0;
bool buttonRaw[3] = {HIGH, HIGH, HIGH}, buttonStable[3] = {HIGH, HIGH, HIGH};
uint32_t buttonChanged[3] = {0, 0, 0};
float secondsLeq[10], occupiedSeconds = 0, secondEnergy = 0;
float minLevel = 140, maxLevel = 0;
unsigned histogram[70] = {0}, sampleCount = 0, transitions = 0;
time_t batchStart = 0;

bool pendingTime(uint32_t deadline) {
  return deadline != 0 && int32_t(deadline - millis()) > 0;
}

String quoted(const char *value) {
  String result = "\"";
  for (const char *p = value; *p; ++p) {
    if (*p == '"' || *p == '\\') result += '\\';
    if (uint8_t(*p) < 32) result += ' '; else result += *p;
  }
  return result + '"';
}

void beep(unsigned durationMs, bool automatic = false) {
  if (!buzzerQueue) return;
  BuzzerCommand command{BuzzerAction::Tone, durationMs, automatic};
  xQueueSend(buzzerQueue, &command, 0);
}

void stopBeep(bool automaticOnly = false) {
  if (!buzzerQueue) return;
  if (!automaticOnly) xQueueReset(buzzerQueue);
  BuzzerCommand command{automaticOnly ? BuzzerAction::StopAutomatic : BuzzerAction::StopAll, 0, false};
  xQueueSendToFront(buzzerQueue, &command, 0);
}

void buzzerWorker(void *) {
  BuzzerWindow window;
  bool automatic = false;
  for (;;) {
    uint32_t now = millis();
    if (window.expired(now)) {
      ledcWrite(BUZZER_PIN, 0);
      window.stop(now);
    }
    TickType_t wait = window.active()
      ? max(TickType_t(1), pdMS_TO_TICKS(window.remaining(now))) : portMAX_DELAY;
    BuzzerCommand command;
    if (xQueueReceive(buzzerQueue, &command, wait) != pdTRUE) continue;
    now = millis();
    if (window.expired(now)) {
      ledcWrite(BUZZER_PIN, 0);
      window.stop(now);
    }
    if (command.action == BuzzerAction::StopAll
        || (command.action == BuzzerAction::StopAutomatic && automatic)) {
      ledcWrite(BUZZER_PIN, 0);
      window.stop(now);
    } else if (command.action == BuzzerAction::Tone
               && (!command.automatic || automaticBuzzerAllowed.load())
               && window.start(now, command.durationMs)) {
      automatic = command.automatic;
      ledcWriteTone(BUZZER_PIN, 1600);
    }
  }
}

void updateIndicators(float level) {
  ComfortState next = comfortState(climateValid, temperature, humidity, occupied, level);
  automaticBuzzerAllowed.store(next == ComfortState::Red && !pendingTime(mutedUntil)
                              && ENABLE_AUTOMATIC_BUZZER);
  bool enteredRed = next == ComfortState::Red && currentComfort != ComfortState::Red;
  if (enteredRed) redSinceMs = millis();
  if (next != ComfortState::Red && currentComfort == ComfortState::Red) stopBeep(true);
  currentComfort = next;
  // El modelo solo adelanta el aviso: ambar parpadeando si la sala esta en optima o moderada; nunca tapa un rojo.
  bool early = forecastWarning.load() && (next == ComfortState::Optimal || next == ComfortState::Moderate);
  bool blink = (millis() / 400) % 2 == 0;
  digitalWrite(LED_GREEN_PIN, next == ComfortState::Optimal && !early ? HIGH : LOW);
  digitalWrite(LED_AMBER_PIN, early ? (blink ? HIGH : LOW) : (next == ComfortState::Moderate ? HIGH : LOW));
  bool redOn = next == ComfortState::Red && (millis() - redSinceMs) % 1000 < 500;
  digitalWrite(LED_RED_PIN, redOn ? HIGH : LOW);
  if (redAlert.update(next, pendingTime(mutedUntil)) && ENABLE_AUTOMATIC_BUZZER)
    beep(BuzzerWindow::MaxMs, true);
}

void handleButtons() {
  uint32_t now = millis();
  for (unsigned i = 0; i < 3; ++i) {
    bool value = digitalRead(BUTTON_PINS[i]);
    if (value != buttonRaw[i]) { buttonRaw[i] = value; buttonChanged[i] = now; }
    if (now - buttonChanged[i] < 30 || value == buttonStable[i]) continue;
    buttonStable[i] = value;
    if (value != LOW) continue;
    if (i == 0) {
      beep(250);
      Serial.println("Prueba manual del buzzer: 250 ms");
    } else if (i == 1) {
      mutedUntil = now + MUTE_MS;
      automaticBuzzerAllowed.store(false);
      stopBeep();
      Serial.println("Avisos automaticos silenciados durante 60 s");
    } else {
      manualPresence = (manualPresence + 1) % 3;
      manualUntil = manualPresence ? now + MANUAL_PRESENCE_MS : 0;
      Serial.printf("Presencia manual: %s (caduca en 60 s)\n",
                    manualPresence == 1 ? "ocupada" : manualPresence == 2 ? "vacia" : "automatica");
    }
  }
  if (manualPresence && !pendingTime(manualUntil)) manualPresence = 0;
  if (mutedUntil && !pendingTime(mutedUntil)) mutedUntil = 0;
}

void readPresence() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long echo = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  distanceCm = echo ? echo * 0.0343f / 2.0f : NAN;
  // El PIR puede estabilizarse durante el primer minuto.
  bool motion = millis() >= 60000 && digitalRead(PIR_PIN) == HIGH;
  if (motion || (isfinite(distanceCm) && distanceCm >= 2 && distanceCm < PRESENCE_DISTANCE_CM)) {
    detected = true;
    lastDetectionMs = millis();
  }
  bool next = detected && millis() - lastDetectionMs < PRESENCE_HOLD_MS;
  if (manualPresence) next = manualPresence == 1;
  if (next != occupied) ++transitions;
  occupied = next;
}

void resetBatch() {
  sampleCount = 0;
  secondEnergy = 0;
  occupiedSeconds = 0;
  transitions = 0;
  minLevel = 140;
  maxLevel = 0;
  memset(histogram, 0, sizeof(histogram));
}

String makeBatch() {
  struct tm utc;
  gmtime_r(&batchStart, &utc);
  char timestamp[24];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
  String body;
  body.reserve(1400);
  body = "{\"device_id\":" + quoted(DEVICE_ID) + ",\"room_id\":" + quoted(ROOM_ID);
  body += ",\"boot_id\":\"" + bootId + "\",\"fw_version\":" + quoted(FW_VERSION);
  body += ",\"seq\":" + String(++sequence) + ",\"ts\":\"" + String(timestamp);
  body += "\",\"window_s\":10,\"acoustic\":{\"laeq_1s\":[";
  for (unsigned i = 0; i < 10; ++i) { if (i) body += ','; body += String(secondsLeq[i], 1); }
  body += "],\"lmax\":" + String(maxLevel, 1) + ",\"lmin\":" + String(minLevel, 1);
  body += ",\"hist_min_db\":30,\"hist_bin_db\":1,\"hist\":[";
  for (unsigned i = 0; i < 70; ++i) { if (i) body += ','; body += String(histogram[i]); }
  body += "]},\"climate\":{\"temp_c\":" + String(temperature, 2);
  body += ",\"rh_pct\":" + String(humidity, 1) + "},\"presence\":{\"state\":\"";
  body += occupied ? "occupied" : "vacant";
  body += "\",\"occupied_s\":" + String(occupiedSeconds, 3);
  body += ",\"transitions\":" + String(transitions) + "}}";
  return body;
}

extern unsigned ramCount;
unsigned queueSize() {
  if (!fsReady) return ramCount;
  unsigned count = 0;
  File root = LittleFS.open("/");
  for (File file = root.openNextFile(); file; file = root.openNextFile()) {
    if (String(file.name()).endsWith(".json")) ++count;
    file.close();
  }
  return count;
}

// Respaldo si LittleFS no monta (particion ausente, como en Wokwi, o flash danada): cola en RAM, solo los ultimos lotes.
constexpr unsigned RAM_QUEUE_MAX = 6;
String ramQueue[RAM_QUEUE_MAX];
unsigned ramCount = 0;

void enqueueBatch(const String &body) {
  if (!fsReady) {
    xSemaphoreTake(fsMutex, portMAX_DELAY);
    if (ramCount == RAM_QUEUE_MAX) {                 // llena: se descarta el mas viejo
      for (unsigned i = 1; i < RAM_QUEUE_MAX; ++i) ramQueue[i - 1] = ramQueue[i];
      --ramCount;
      pendingBatches.fetch_sub(1);
      Serial.println("Cola en RAM llena: se descarta el lote mas antiguo");
    }
    ramQueue[ramCount++] = body;
    pendingBatches.fetch_add(1);
    xSemaphoreGive(fsMutex);
    Serial.printf("Lote %lu en cola de RAM (sin LittleFS); pendientes %u\n", (unsigned long)sequence, pendingBatches.load());
    return;
  }
  xSemaphoreTake(fsMutex, portMAX_DELAY);
  if (queueSize() >= MAX_QUEUED_BATCHES) {
    Serial.println("Cola llena: lote nuevo descartado; recuperar conexion al Edge");
  } else {
    char name[64];
    snprintf(name, sizeof(name), "/%010lld_%010lu_%s", (long long)batchStart,
             (unsigned long)sequence, bootId.substring(0, 8).c_str());
    String temp = String(name) + ".tmp", target = String(name) + ".json";
    File file = LittleFS.open(temp, "w");
    bool saved = file && file.print(body) == body.length();
    file.close();
    if (!saved || !LittleFS.rename(temp, target)) {
      LittleFS.remove(temp);
      Serial.println("No se pudo guardar el lote; comprobar espacio LittleFS");
    } else {
      pendingBatches.fetch_add(1);
      Serial.printf("Lote %lu guardado; pendientes %u\n", (unsigned long)sequence, pendingBatches.load());
    }
  }
  xSemaphoreGive(fsMutex);
}

// ---- MQTT: todo el trafico de red vive en esta tarea (ArduinoMqttClient no es seguro entre hilos)
void onCommand(int) {
  char buf[96];
  size_t n = 0;
  while (mqtt.available() && n < sizeof(buf) - 1) buf[n++] = (char)mqtt.read();
  buf[n] = '\0';
  const char *p = strstr(buf, "temp_limit");
  if (!p || !(p = strchr(p, ':'))) { Serial.println("[MQTT] comando sin temp_limit, ignorado"); return; }
  float value = atof(p + 1);
  if (value < TEMP_LIMIT_MIN || value > TEMP_LIMIT_MAX) {
    Serial.printf("[MQTT] limite %.2f fuera de rango (%.0f a %.0f), ignorado\n", value, TEMP_LIMIT_MIN, TEMP_LIMIT_MAX);
    return;
  }
  limitC.store(value);
  Serial.printf("[MQTT] nuevo limite de temperatura: %.2f C\n", value);
}

bool mqttPublish(const char *topic, const String &body, bool retain, int qos) {
  if (!mqtt.beginMessage(topic, body.length(), retain, qos)) return false;
  mqtt.print(body);
  return mqtt.endMessage() == 1;     // con QoS 1 espera el PUBACK del broker
}

bool mqttConnect() {
  mqtt.setId(DEVICE_ID);
  mqtt.setKeepAliveInterval(MQTT_KEEPALIVE_MS);
#ifdef MQTT_USER
  mqtt.setUsernamePassword(MQTT_USER, MQTT_PASS);
#endif
  mqtt.beginWill(topicStatus, 7, true, 1);   // si el equipo se cae, el broker publica "offline"
  mqtt.print("offline");
  mqtt.endWill();
  if (!mqtt.connect(MQTT_HOST, MQTT_PORT)) {
    Serial.printf("[MQTT] sin conexion con %s:%d (error %d); se reintenta\n", MQTT_HOST, MQTT_PORT, mqtt.connectError());
    return false;
  }
  mqtt.onMessage(onCommand);
  mqtt.subscribe(topicCmd, 1);
  mqttPublish(topicStatus, "online", true, 1);
  Serial.printf("[MQTT] conectado; lotes en %s\n", topicReadings);
  return true;
}

String oldestBatch(String &body) {
  String oldest;
  xSemaphoreTake(fsMutex, portMAX_DELAY);
  if (!fsReady) {
    if (ramCount) { body = ramQueue[0]; oldest = "ram"; }
    xSemaphoreGive(fsMutex);
    return oldest;
  }
  File root = LittleFS.open("/");
  for (File file = root.openNextFile(); file; file = root.openNextFile()) {
    String name = file.name();
    if (!name.startsWith("/")) name = "/" + name;
    if (name.endsWith(".json") && (oldest.isEmpty() || name.compareTo(oldest) < 0)) oldest = name;
    file.close();
  }
  if (!oldest.isEmpty()) {
    File file = LittleFS.open(oldest, "r");
    body = file.readString();
    file.close();
  }
  xSemaphoreGive(fsMutex);
  return oldest;
}

void networkWorker(void *) {
  uint32_t lastWifi = 0, lastMqtt = 0;
  for (;;) {
    bool worked = false;
    if (WiFi.status() != WL_CONNECTED) {
      mqttUp.store(false);
      if (millis() - lastWifi >= 10000) {
        lastWifi = millis();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
      }
    } else {
      if (!mqtt.connected()) {
        if (mqttUp.load()) Serial.println("[MQTT] conexion perdida");
        mqttUp.store(false);
        if (lastMqtt == 0 || millis() - lastMqtt >= 5000) {
          lastMqtt = millis();
          mqttUp.store(mqttConnect());
        }
      }
      if (mqtt.connected()) {
        mqtt.poll();                                   // keepalive y comandos
        char forecast[FORECAST_MSG];
        while (forecastQueue && xQueueReceive(forecastQueue, forecast, 0) == pdTRUE)
          mqttPublish(topicForecast, String(forecast), false, 1);   // un pronostico viejo no sirve: si falla se pierde
        if (pendingBatches.load() > 0) {
          String body;
          String oldest = oldestBatch(body);
          if (oldest.isEmpty() || body.isEmpty()) {
            pendingBatches.store(queueSize());          // la cuenta se desfaso: se recalcula
          } else if (mqttPublish(topicReadings, body, false, 1)) {
            xSemaphoreTake(fsMutex, portMAX_DELAY);
            if (!fsReady) {
              for (unsigned i = 1; i < ramCount; ++i) ramQueue[i - 1] = ramQueue[i];
              if (ramCount) ramQueue[--ramCount] = "";
            } else LittleFS.remove(oldest);
            xSemaphoreGive(fsMutex);
            pendingBatches.fetch_sub(1);
            worked = true;                              // hay mas: seguir sin esperar
          } else Serial.println("[MQTT] lote no confirmado: se conserva para reintentar");
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(worked ? 20 : 200));
  }
}

float readNoiseLevel() {
  unsigned adc = 0;
  for (unsigned i = 0; i < 8; ++i) adc += analogRead(POT_PIN);
  potAdc = adc / 8;
  return 30.0f + (adc / 8.0f) * (69.9f / 4095.0f);
}

// Fin de ventana de 10 s: se guarda el lote y, una vez por MODEL_STEP_MS, corre el modelo.
void closeBatch() {
  enqueueBatch(makeBatch());
#if ENABLE_FORECAST
  static uint32_t lastModel = 0;
  if (lastModel == 0 || millis() - lastModel >= MODEL_STEP_MS) {
    lastModel = millis();
    float energy = 0;
    for (unsigned i = 0; i < 10; ++i) energy += powf(10.0f, secondsLeq[i] / 10.0f);
    float leq = 10.0f * log10f(energy / 10);
    model.limitC = limitC.load();
    float prob = model.step(temperature, humidity, occupiedSeconds >= 5.0f ? 1.0f : 0.0f, leq);
    forecastWarning.store(model.warning());
    char msg[FORECAST_MSG];
    snprintf(msg, sizeof(msg),
             "{\"device_id\":\"%s\",\"probability\":%.3f,\"horizon_min\":%d,\"temperature_limit_c\":%.2f,\"model_version\":\"zenroom-5x16x16-v1\"}",
             DEVICE_ID, prob, NN_HORIZONTE_MIN, model.limitC);
    if (forecastQueue) xQueueSend(forecastQueue, msg, 0);
    Serial.printf("modelo: prob=%.3f limite=%.2f historia=%s\n", prob, model.limitC, model.historyReady ? "lista" : "juntando");
  }
#endif
}

void sampleNoise(float level) {
  time_t now = time(nullptr);
  if (now < 1577836800 || !climateValid) { resetBatch(); return; }
  if (sampleCount == 0) {
    if (now % 10 != 0) return;  // ventanas alineadas al minuto UTC
    batchStart = now;
    transitions = 0;
  }
  minLevel = min(minLevel, level);
  maxLevel = max(maxLevel, level);
  ++histogram[constrain(int(level - 30), 0, 69)];
  secondEnergy += powf(10.0f, level / 10.0f);
  if (occupied) occupiedSeconds += 0.125f;
  ++sampleCount;
  if (sampleCount % 8 == 0) {
    secondsLeq[sampleCount / 8 - 1] = 10 * log10f(secondEnergy / 8);
    secondEnergy = 0;
  }
  if (sampleCount == 80) { closeBatch(); resetBatch(); }
}

void setup() {
  Serial.begin(115200);
  for (int pin : BUTTON_PINS) pinMode(pin, INPUT_PULLUP);
  for (int pin : {LED_GREEN_PIN, LED_AMBER_PIN, LED_RED_PIN}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
  pinMode(PIR_PIN, INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_LOW ? HIGH : LOW);
  pwmReady = ledcAttach(BUZZER_PIN, 1600, 8);
  if (pwmReady) {
    ledcOutputInvert(BUZZER_PIN, BUZZER_ACTIVE_LOW);
    ledcWrite(BUZZER_PIN, 0);
    buzzerQueue = xQueueCreate(8, sizeof(BuzzerCommand));
    if (!buzzerQueue || xTaskCreate(buzzerWorker, "buzzer", 3072, nullptr, 3, nullptr) != pdPASS) {
      buzzerQueue = nullptr;
      Serial.println("Buzzer deshabilitado: no se pudo iniciar su temporizador");
    }
  }
  analogReadResolution(12);
  analogSetPinAttenuation(POT_PIN, ADC_11db);
  dht.begin();
  char boot[33];
  snprintf(boot, sizeof(boot), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
  bootId = boot;
  fsMutex = xSemaphoreCreateMutex();
  fsReady = LittleFS.begin(FORMAT_FILESYSTEM_ON_FAILURE);
  if (fsReady) pendingBatches.store(queueSize());
  if (!fsReady) Serial.println("LittleFS no monta: se usa una cola en RAM (se pierde al reiniciar). En la placa real elegir un esquema de particiones con SPIFFS.");
  snprintf(topicReadings, sizeof(topicReadings), "comfort/%s/room/%s/readings", SITE_CODE, ROOM_ID);
  snprintf(topicStatus, sizeof(topicStatus), "comfort/%s/device/%s/status", SITE_CODE, DEVICE_ID);
  snprintf(topicForecast, sizeof(topicForecast), "comfort/%s/device/%s/forecast", SITE_CODE, DEVICE_ID);
  snprintf(topicCmd, sizeof(topicCmd), "comfort/%s/device/%s/cmd", SITE_CODE, DEVICE_ID);
  forecastQueue = xQueueCreate(2, FORECAST_MSG);
#if ENABLE_FORECAST
  float testError = model.selfTest();
  Serial.printf("AUTOPRUEBA red C++ vs Keras: error maximo %.2e %s\n", testError, testError < 1e-4 ? "OK" : "FALLA");
#endif
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  xTaskCreate(networkWorker, "edge-mqtt", 10240, nullptr, 1, nullptr);
  Serial.println("ZenRoom listo. Esperando hora UTC y DHT22 validos.");
}

void loop() {
  static uint32_t lastClimate = 0, lastPresence = 0, nextSample = 0, lastLog = 0;
  uint32_t now = millis();
  handleButtons();
  if (now - lastClimate >= 2200) {
    lastClimate = now;
    temperature = dht.readTemperature();
    humidity = dht.readHumidity();
    climateValid = isfinite(temperature) && isfinite(humidity)
                   && temperature >= -40 && temperature <= 80 && humidity >= 0 && humidity <= 100;
    if (!climateValid) Serial.println("DHT22 invalido: revisar GPIO23; se omite la ventana");
  }
  if (now - lastPresence >= 250) { lastPresence = now; readPresence(); }
  if (int32_t(now - nextSample) >= 0) {
    if (now - nextSample > 250) { resetBatch(); nextSample = now; }
    nextSample += 125;
    noiseLevel = readNoiseLevel();
    updateIndicators(noiseLevel);
    sampleNoise(noiseLevel);
  }
  if (now - lastLog >= 2000) {
    lastLog = now;
    Serial.printf("T=%.1f RH=%.1f ruido=%.1f ADC=%u presencia=%d distancia=%.1f cm hora=%lld wifi=%d mqtt=%d cola=%u\n",
                  temperature, humidity, noiseLevel, potAdc,
                  occupied, distanceCm, (long long)time(nullptr),
                  WiFi.status() == WL_CONNECTED, (int)mqttUp.load(), pendingBatches.load());
  }
  delay(2);
}
