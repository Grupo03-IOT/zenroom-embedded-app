#pragma once
// Copiar este archivo a DeviceSecrets.h (esta en .gitignore) y completar. NUNCA subir DeviceSecrets.h.

#define WIFI_SSID "NombreDeLaRed"
#define WIFI_PASSWORD "ClaveDeLaRed"

// Broker MQTT: el Mosquitto del Edge (edge-api/broker). Es la IP de la laptop que corre el Edge en esa red.
#define MQTT_HOST "192.168.1.50"
#define MQTT_PORT 1883
// Si el broker exige usuario (el del Edge lo exige), descomentar:
// #define MQTT_USER "esp32"
// #define MQTT_PASS "clave-del-broker"

// Debe coincidir con el SITE_CODE del Edge y con el local creado en Cloud.
#define SITE_CODE "c7iot-local"
// #define DEVICE_ID "esp32-sala-01"
// #define ROOM_ID "sala-01"
