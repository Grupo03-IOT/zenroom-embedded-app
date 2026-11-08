#pragma once
// Configuracion del dispositivo. Pines y reglas: los de Paula (montaje real).
// Lo que cambia de una red o de un equipo a otro (WiFi, broker, claves) va en DeviceSecrets.h,
// que NO se sube al repositorio (copiar DeviceSecrets.example.h).

#if __has_include("DeviceSecrets.h")
#include "DeviceSecrets.h"
#else
#error "Falta DeviceSecrets.h: copiar DeviceSecrets.example.h a DeviceSecrets.h y completarlo."
#endif

// ---- Identidad (se pueden fijar en DeviceSecrets.h)
#ifndef DEVICE_ID
#define DEVICE_ID "esp32-sala-01"
#endif
#ifndef ROOM_ID
#define ROOM_ID "sala-01"
#endif
#ifndef SITE_CODE
#define SITE_CODE "site-1"          // debe ser el codigo del local en Cloud y el que usa el Edge (SITE_CODE)
#endif
#ifndef FW_VERSION
#define FW_VERSION "0.4.0-mqtt"
#endif

// ---- Red
#ifndef WIFI_CHANNEL
#define WIFI_CHANNEL 0              // 0 = automatico; en Wokwi-GUEST es 6
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883              // sin TLS: solo red privada o simulacion
#endif
#ifndef MQTT_KEEPALIVE_MS
#define MQTT_KEEPALIVE_MS 60000L
#endif
// MQTT_USER / MQTT_PASS son opcionales: si no se definen se conecta sin credenciales.

// ---- Modelo de pronostico (red 5-16-16-1 en forecast_weights.h)
#ifndef ENABLE_FORECAST
#define ENABLE_FORECAST 1           // 0 = el dispositivo solo mide y publica lotes
#endif
#ifndef MODEL_STEP_MS
#define MODEL_STEP_MS 60000UL       // una inferencia por minuto, como en el entrenamiento; en simulacion 10000
#endif

// ---- Pines (montaje real, segun la Guia de conexiones)
constexpr int DHT_PIN = 23;
constexpr int PIR_PIN = 27;
constexpr int TRIG_PIN = 22;
constexpr int ECHO_PIN = 35;
constexpr int POT_PIN = 34;
constexpr int BUZZER_PIN = 26;
constexpr int LED_GREEN_PIN = 21;
constexpr int LED_AMBER_PIN = 19;
constexpr int LED_RED_PIN = 18;
constexpr int BUTTON_PINS[] = {13, 25, 32};

constexpr bool BUZZER_ACTIVE_LOW = true;
constexpr bool ENABLE_AUTOMATIC_BUZZER = true;
// El almacenamiento es solo una cola de reenvio: si no se puede montar (primer arranque con la
// particion vacia) se formatea. No se pierde nada que no se pueda volver a medir.
constexpr bool FORMAT_FILESYSTEM_ON_FAILURE = true;
constexpr unsigned MAX_QUEUED_BATCHES = 120;  // 20 minutos; limite visible por Serial
constexpr float PRESENCE_DISTANCE_CM = 150.0f;
constexpr unsigned long ECHO_TIMEOUT_US = 9000;  // 150 cm de ida y vuelta; antes 15 ms bloqueaba el muestreo
constexpr unsigned PRESENCE_HOLD_MS = 30000;
constexpr unsigned MANUAL_PRESENCE_MS = 60000;
constexpr unsigned MUTE_MS = 60000;
constexpr float TEMP_LIMIT_MIN = 18.0f, TEMP_LIMIT_MAX = 30.0f;  // rango del comando temp_limit
