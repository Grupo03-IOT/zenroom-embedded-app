// ZenRoom - dispositivo de sala con aviso anticipado de calor (Edge ML)
// 1ASI0572 Desarrollo de Soluciones IoT - SenseWork
//
// Mismo flujo que la Aplicacion 4: la red se entreno en Colab (zenroom_forecast_training.ipynb),
// sus pesos estan en forecast_weights.h y la inferencia se hace aqui en C++ puro.
//
// Cada muestra cuenta como un minuto de la sala (SAMPLE_MS acelera el tiempo, como --speed
// en el simulador del Edge). Con los ultimos 10 minutos la red estima la probabilidad de que
// la sala ocupada llegue a su limite de temperatura en los proximos 15 minutos.
//
// Luz de la puerta: verde = optima, ambar fijo = moderada, rojo = no recomendada,
// ambar parpadeando = "se esta calentando" (aviso del modelo), apagadas = sin datos.
// El limite de temperatura llega desde fuera por MQTT: el Edge le envia datos al dispositivo.

#include <Arduino.h>   // [PlatformIO] en wokwi.com no hace falta
#include <WiFi.h>
#include <ArduinoMqttClient.h>
#include <time.h>
#include <math.h>
#include "DHTesp.h"
#include "../colab/forecast_weights.h"

// ---------------------------------------------------------------- pines (seccion 5.6)
// Los pines dependen del montaje: Wokwi (perfil zenroom-sim) y la protoboard real (perfil zenroom,
// segun la Guia de conexiones). Se cambian con -DPIN_xxx en platformio.ini.
#ifndef PIN_DHT
#define PIN_DHT 15              // DHT22 (sustituye al SHT31). Montaje real: GPIO 23
#endif
#ifndef PIN_RUIDO
#define PIN_RUIDO 34            // potenciometro que simula el ruido (sustituye al INMP441)
#endif
#ifndef PIN_PIR
#define PIN_PIR 27              // PIR (sustituye al LD2410C)
#endif
#ifndef PIN_PRUEBA
#define PIN_PRUEBA -1           // boton de prueba del escenario automatico de Wokwi; -1 = no existe
#endif
#ifndef PIN_BUZZER
#define PIN_BUZZER -1           // modulo pasivo de disparo bajo (montaje real: GPIO 26); -1 = no existe
#endif
const uint8_t PIN_VERDE = 21, PIN_AMBAR = 19, PIN_ROJO = 18;

// ---------------------------------------------------------------- configuracion
#ifndef SAMPLE_MS
#define SAMPLE_MS 60000         // inferencia cada minuto, como en el conjunto de entrenamiento
#endif
#ifndef DEVICE_ID
#define DEVICE_ID "zr-sala-03"
#endif
#ifndef ROOM_ID
#define ROOM_ID "sala-03"
#endif
#ifndef TOPIC_BASE
#define TOPIC_BASE "comfort/site-1"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID "Wokwi-GUEST"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef WIFI_CHANNEL
#define WIFI_CHANNEL 6          // canal fijo de Wokwi-GUEST; en una red real se usa 0 (automatico)
#endif
#ifndef MQTT_HOST
#define MQTT_HOST "broker.hivemq.com"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883          // sin TLS: solo para simulacion o una red privada
#endif
const unsigned long PARPADEO_MS = 400;
const unsigned long REINTENTO_MQTT_MS = 5000;
#ifndef KEEPALIVE_MS
#define KEEPALIVE_MS 60000L     // hardware real: 60 s. Wokwi corre mas lento que el reloj y usa un valor mayor
#endif
const float LIMITE_MIN = 18.0f, LIMITE_MAX = 30.0f;

char topicCmd[96], topicForecast[96], topicReadings[96], topicStatus[96];

WiFiClient wifiClient;
MqttClient mqtt(wifiClient);
DHTesp dht;

// ---------------------------------------------------------------- estado
enum EstadoPuerta : uint8_t { SIN_DATOS, OPTIMA, AVISO_CALOR, MODERADA, NO_RECOMENDADA };

float limiteTemp = 25.0f;                       // lo cambia el comando MQTT
volatile uint16_t transicionesPir = 0;          // lo toca la interrupcion

// historia de los ultimos minutos (buffers circulares de tamano fijo, sin String ni heap)
const uint8_t HIST = NN_VENTANA + 1;            // temp necesita el valor de hace 10 minutos
float histTemp[HIST], histOcup[NN_VENTANA], histRuido[NN_VENTANA];
uint8_t posHist = 0;
uint16_t muestras = 0;

EstadoPuerta estado = SIN_DATOS;
float ultimaProb = 0.0f;

// ---------------------------------------------------------------- interrupcion del PIR
void IRAM_ATTR isrPir() { transicionesPir++; }

// ---------------------------------------------------------------- red neuronal (como neuralNet() de la App4)
float redNeuronal(const float *xCrudo) {
  float x[NN_IN], h1[NN_H], h2[NN_H];
  for (uint8_t i = 0; i < NN_IN; i++) x[i] = (xCrudo[i] - NN_MU[i]) / NN_SD[i];   // misma normalizacion que en Colab
  for (uint8_t j = 0; j < NN_H; j++) {                                            // capa 1: IN -> 16, ReLU
    float v = NN_B1[j];
    for (uint8_t i = 0; i < NN_IN; i++) v += NN_W1[j * NN_IN + i] * x[i];
    h1[j] = v > 0 ? v : 0;
  }
  for (uint8_t j = 0; j < NN_H; j++) {                                            // capa 2: 16 -> 16, ReLU
    float v = NN_B2[j];
    for (uint8_t i = 0; i < NN_H; i++) v += NN_W2[j * NN_H + i] * h1[i];
    h2[j] = v > 0 ? v : 0;
  }
  float y = NN_B3[0];                                                             // capa 3: 16 -> 1, sigmoide
  for (uint8_t i = 0; i < NN_H; i++) y += NN_W3[i] * h2[i];
  return 1.0f / (1.0f + expf(-y));
}

// Al arrancar se comprueba que el C++ da lo mismo que Keras con 8 casos exportados del notebook.
void autoprueba() {
  float maxErr = 0;
  for (uint8_t k = 0; k < REF_N; k++) {
    float e = fabsf(redNeuronal(&REF_X[k * NN_IN]) - REF_P[k]);
    if (e > maxErr) maxErr = e;
  }
  Serial.printf("AUTOPRUEBA red C++ vs Keras: error maximo %.2e %s\n", maxErr, maxErr < 1e-4 ? "OK" : "FALLA");
}

// ---------------------------------------------------------------- sensores
float leerRuidoDb() {                       // 0..4095 del potenciometro -> 30..90 dB(A)
  uint32_t suma = 0;
  for (uint8_t i = 0; i < 8; i++) suma += analogRead(PIN_RUIDO);
  return 30.0f + 60.0f * (suma / 8.0f) / 4095.0f;
}

float leqVentana(const float *db, uint8_t n) {   // los dB se promedian en energia, no aritmeticamente
  float e = 0;
  for (uint8_t i = 0; i < n; i++) e += powf(10.0f, db[i] / 10.0f);
  return 10.0f * log10f(e / n);
}

// ---------------------------------------------------------------- MQTT (el Edge envia datos al dispositivo)
void alRecibirMensaje(int /*tam*/) {      // un solo topic suscrito: no hace falta leerlo
  static char buf[96];                      // static: no se reserva en cada mensaje
  uint8_t n = 0;
  while (mqtt.available() && n < sizeof(buf) - 1) buf[n++] = (char)mqtt.read();
  buf[n] = '\0';
  const char *p = strstr(buf, "temp_limit");
  if (!p || !(p = strchr(p, ':'))) { Serial.println(F("[MQTT] comando sin temp_limit, ignorado")); return; }
  float nuevo = atof(p + 1);
  if (nuevo < LIMITE_MIN || nuevo > LIMITE_MAX) {
    Serial.printf("[MQTT] limite %.2f fuera de rango (%.0f a %.0f), ignorado\n", nuevo, LIMITE_MIN, LIMITE_MAX);
    return;
  }
  limiteTemp = nuevo;
  Serial.printf("[MQTT] nuevo limite de temperatura: %.2f C\n", limiteTemp);
}

void conectarMqtt() {
  static unsigned long ultimoIntento = 0;
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (ultimoIntento && millis() - ultimoIntento < REINTENTO_MQTT_MS) return;   // no bloquea el loop
  ultimoIntento = millis();
  mqtt.setId(DEVICE_ID);
  mqtt.setKeepAliveInterval(KEEPALIVE_MS);
  mqtt.beginWill(topicStatus, 7, true, 1);   // si el equipo se cae, el broker publica "offline" por el
  mqtt.print(F("offline"));
  mqtt.endWill();
  if (!mqtt.connect(MQTT_HOST, MQTT_PORT)) { Serial.println(F("[MQTT] sin conexion, se reintenta")); return; }
  mqtt.onMessage(alRecibirMensaje);
  mqtt.subscribe(topicCmd, 1);
  mqtt.beginMessage(topicStatus, 6, true, 1);
  mqtt.print(F("online"));
  mqtt.endMessage();
  Serial.printf("[MQTT] conectado, suscrito a %s\n", topicCmd);
}

void publicar(float prob) {
  if (!mqtt.connected()) return;
  static char msg[224];
  snprintf(msg, sizeof(msg), "{\"device_id\":\"%s\",\"probability\":%.3f,\"horizon_min\":15,\"temperature_limit_c\":%.2f,\"model_version\":\"zenroom-5x16x16-v1\"}",
           DEVICE_ID, prob, limiteTemp);
  mqtt.beginMessage(topicForecast, false, 1);
  mqtt.print(msg);
  mqtt.endMessage();
}

void publicarLectura(float temp, float humedad, float ruido, bool ocupada, uint16_t trans) {
  if (!mqtt.connected()) return;
  static uint32_t secuencia = 0;
  static char msg[384];
  time_t ahora = time(nullptr);
  struct tm utc;
  char ts[24] = "1970-01-01T00:00:00Z";
  if (ahora > 1577836800 && gmtime_r(&ahora, &utc)) {
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &utc);
  }
  snprintf(msg, sizeof(msg),
    "{\"device_id\":\"%s\",\"room_id\":\"%s\",\"seq\":%lu,\"ts\":\"%s\",\"fw_version\":\"zenroom-forecast-v1\",\"acoustic\":{\"laeq_1s\":[%.1f],\"hist\":[],\"lmax\":%.1f,\"lmin\":%.1f},\"climate\":{\"temp_c\":%.2f,\"rh_pct\":%.1f},\"presence\":{\"state\":\"%s\",\"occupied_s\":%.0f,\"transitions\":%u}}",
    DEVICE_ID, ROOM_ID, (unsigned long)++secuencia, ts, ruido, ruido, ruido,
    temp, humedad, ocupada ? "occupied" : "vacant", ocupada ? 10.0f : 0.0f, trans);
  mqtt.beginMessage(topicReadings, false, 1);
  mqtt.print(msg);
  mqtt.endMessage();
}

// ---------------------------------------------------------------- luz de la puerta
const char *nombreEstado(EstadoPuerta e) {
  switch (e) {
    case OPTIMA: return "OPTIMA";
    case AVISO_CALOR: return "AVISO_CALOR";
    case MODERADA: return "MODERADA";
    case NO_RECOMENDADA: return "NO_RECOMENDADA";
    default: return "SIN_DATOS";
  }
}

void actualizarLuz() {
  static unsigned long ultimoCambio = 0;
  static bool encendido = false;
  if (millis() - ultimoCambio >= PARPADEO_MS) { ultimoCambio = millis(); encendido = !encendido; }
  bool v = false, a = false, r = false;
  switch (estado) {                         // un estado, una salida: sin if anidados
    case OPTIMA: v = true; break;
    case AVISO_CALOR: a = encendido; break; // ambar parpadeando
    case MODERADA: a = true; break;
    case NO_RECOMENDADA: r = true; break;
    case SIN_DATOS: break;
  }
  digitalWrite(PIN_VERDE, v);
  digitalWrite(PIN_AMBAR, a);
  digitalWrite(PIN_ROJO, r);
}

// ---------------------------------------------------------------- un minuto de la sala
void procesarMuestra() {
  TempAndHumidity th = dht.getTempAndHumidity();
  if (isnan(th.temperature) || isnan(th.humidity)) { estado = SIN_DATOS; Serial.println(F("DHT22 sin lectura")); return; }
  bool presente = digitalRead(PIN_PIR);
#if PIN_PRUEBA >= 0
  presente = presente || digitalRead(PIN_PRUEBA);
#endif
  float ocupada = presente ? 1.0f : 0.0f;
  float ruido = leerRuidoDb();

  histTemp[posHist % HIST] = th.temperature;
  histOcup[posHist % NN_VENTANA] = ocupada;
  histRuido[posHist % NN_VENTANA] = ruido;
  posHist++;
  if (muestras < 0xFFFF) muestras++;

  float subida = 0, ocup10 = 0, ruido10 = ruido, prob = 0;
  bool hayHistoria = muestras >= HIST;
  if (hayHistoria) {
    subida = th.temperature - histTemp[posHist % HIST];   // el mas antiguo del buffer: hace 10 minutos
    for (uint8_t i = 0; i < NN_VENTANA; i++) ocup10 += histOcup[i];
    ocup10 /= NN_VENTANA;
    ruido10 = leqVentana(histRuido, NN_VENTANA);
  }

  // La red solo responde la pregunta para la que se entreno: sala ocupada y todavia por debajo del limite.
  if (hayHistoria && ocupada > 0 && th.temperature < limiteTemp) {
    float x[NN_IN] = {th.temperature - limiteTemp, subida, th.humidity, ocup10, ruido10};   // mismo orden que FEATURES en Colab
    prob = redNeuronal(x);
  }
  ultimaProb = prob;

  EstadoPuerta base;                        // la regla de siempre (la misma de la web app)
  if (!hayHistoria) base = SIN_DATOS;
  else if (ruido10 >= 60.0f) base = NO_RECOMENDADA;
  else if (th.temperature >= limiteTemp || ruido10 >= 50.0f) base = MODERADA;
  else base = OPTIMA;
  // el modelo solo adelanta el aviso; nunca tapa un rojo
  estado = ((base == OPTIMA || base == MODERADA) && prob >= NN_UMBRAL) ? AVISO_CALOR : base;

  noInterrupts();
  uint16_t trans = transicionesPir; transicionesPir = 0;
  interrupts();

  Serial.printf("min=%u temp=%.2f subida=%.2f hum=%.1f ocup=%.0f ocup10=%.1f ruido10=%.1f limite=%.2f prob=%.3f trans=%u estado=%s\n",
                muestras, th.temperature, subida, th.humidity, ocupada, ocup10, ruido10, limiteTemp, prob, trans,
                nombreEstado(estado));
  publicarLectura(th.temperature, th.humidity, ruido, ocupada > 0, trans);
  publicar(prob);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_VERDE, OUTPUT); pinMode(PIN_AMBAR, OUTPUT); pinMode(PIN_ROJO, OUTPUT);
  pinMode(PIN_PIR, INPUT_PULLDOWN);         // entradas protegidas: sin flotar, sin falsos disparos
#if PIN_PRUEBA >= 0
  pinMode(PIN_PRUEBA, INPUT_PULLDOWN);
#endif
#if PIN_BUZZER >= 0
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, HIGH);           // el modulo suena con nivel bajo: en reposo va en alto
#endif
  attachInterrupt(digitalPinToInterrupt(PIN_PIR), isrPir, CHANGE);
  dht.setup(PIN_DHT, DHTesp::DHT22);
  snprintf(topicCmd, sizeof(topicCmd), "%s/device/%s/cmd", TOPIC_BASE, DEVICE_ID);
  snprintf(topicForecast, sizeof(topicForecast), "%s/device/%s/forecast", TOPIC_BASE, DEVICE_ID);
  snprintf(topicReadings, sizeof(topicReadings), "%s/room/%s/readings", TOPIC_BASE, ROOM_ID);
  snprintf(topicStatus, sizeof(topicStatus), "%s/device/%s/status", TOPIC_BASE, DEVICE_ID);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println(F("ZenRoom - aviso anticipado de calor"));
  // El JSON de lectura supera el buffer MQTT predeterminado de 256 bytes.
  mqtt.setTxPayloadSize(512);
  autoprueba();
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
}

void loop() {
  static unsigned long ultimaMuestra = 0;
  static bool avisoWifi = false;
  if (WiFi.status() == WL_CONNECTED && !avisoWifi) { avisoWifi = true; Serial.println(F("WiFi conectado.")); }
  conectarMqtt();
  if (mqtt.connected()) mqtt.poll();
  if (millis() - ultimaMuestra >= SAMPLE_MS) { ultimaMuestra = millis(); procesarMuestra(); }
  actualizarLuz();
}
