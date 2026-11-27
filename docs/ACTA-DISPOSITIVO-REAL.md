# Acta de aceptación del dispositivo real

Lista para seguir mientras se graba el video con el ESP32 armado. Cada prueba dice qué hacer, qué se debe ver y deja una columna para marcar si pasó. El firmware que se carga es el sketch de Arduino `arduino/zenroom_esp32/zenroom_esp32.ino`.

Antes de grabar se puede ensayar con la simulación pública del mismo sketch en Wokwi: https://wokwi.com/projects/481710853765583873 . Ahí la red es Wokwi-GUEST, el broker es HiveMQ y el modelo corre cada 10 s en vez de cada minuto; los pines, los botones y las luces son los mismos.

## Datos de la sesión

| Campo | Valor |
|---|---|
| Fecha y hora | |
| Responsable | |
| Commit del firmware (`git rev-parse --short HEAD`) | |
| Commit del Edge | |
| Puerto COM del ESP32 | |
| `SITE_CODE` / `DEVICE_ID` / `ROOM_ID` | |
| IP de la laptop con Mosquitto y el Edge | |

## 1. Cargar el sketch en el ESP32

1. Instalar Arduino IDE 2. En **Gestor de placas**, instalar **esp32 by Espressif Systems**, versión 3.3.x.
2. En **Gestor de bibliotecas**, instalar:
   - `ArduinoMqttClient` (Arduino), 0.1.8
   - `DHT sensor library` (Adafruit), 1.4.x
   - `Adafruit Unified Sensor` (la pide la anterior)
3. Abrir `arduino/zenroom_esp32/zenroom_esp32.ino`. Los `.h` de la misma carpeta se abren como pestañas.
4. Placa: **DOIT ESP32 DEVKIT V1**. Esa placa trae una tabla de particiones fija que ya incluye la partición SPIFFS, que es la que usa LittleFS para guardar los lotes cuando no hay conexión. Si se usa **ESP32 Dev Module**, en *Partition Scheme* elegir **Default 4MB with spiffs**. Sin esa partición el sketch avisa `LittleFS no monta` y solo guarda 6 lotes en RAM.
5. Conectar el ESP32 con un cable USB **de datos** y elegir su puerto COM. Si no aparece, falta el driver CP210x o CH340.
6. Configurar `DeviceSecrets.h` (sección 2) y pulsar **Subir**. Si se queda en `Connecting...`, mantener pulsado **BOOT** hasta que empiece a escribir.
7. Abrir el monitor serie a **115200**.

Con `arduino-cli` es lo mismo:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 arduino/zenroom_esp32
arduino-cli upload  --fqbn esp32:esp32:esp32doit-devkit-v1 -p COM5 arduino/zenroom_esp32
arduino-cli monitor -p COM5 -c baudrate=115200
```

## 2. Configurar `DeviceSecrets.h`

Copiar `DeviceSecrets.example.h` como `DeviceSecrets.h` en la misma carpeta. Ese archivo está en `.gitignore`: tiene la clave del Wi-Fi y no se sube nunca.

```cpp
#define WIFI_SSID "NombreDeLaRed"          // red de 2,4 GHz: el ESP32 no ve redes de 5 GHz
#define WIFI_PASSWORD "ClaveDeLaRed"
#define MQTT_HOST "192.168.1.50"           // IP de la laptop con Mosquitto y el Edge (ipconfig)
#define MQTT_PORT 1883
#define MQTT_USER "esp32"                  // el Mosquitto del Edge no acepta anónimos
#define MQTT_PASS "clave-del-broker"
#define SITE_CODE "c7iot-local"            // el mismo SITE_CODE del Edge y el código del local en Cloud
#define DEVICE_ID "esp32-sala-01"
#define ROOM_ID "sala-01"
```

La laptop y el ESP32 deben estar en la misma red. En Windows, permitir en el firewall las conexiones entrantes al puerto 1883.

## 3. Levantar Mosquitto, el Edge y la web

Todo desde la carpeta de `edge-api`.

**Mosquitto.** La primera vez se crean los usuarios `esp32` (el del dispositivo) y `edge-api`:

```sh
mosquitto_passwd -c broker/passwd esp32
mosquitto_passwd    broker/passwd edge-api
mkdir -p broker/data
mosquitto -c broker/mosquitto.conf -v
```

**Edge**, en otra terminal (PowerShell):

```powershell
.\.venv\Scripts\Activate.ps1
$env:MQTT_ENABLED = 'true'
$env:MQTT_USERNAME = 'edge-api'
$env:MQTT_PASSWORD = '<clave de edge-api>'
$env:SITE_CODE = 'c7iot-local'
$env:EDGE_CODE = 'edge-1'
$env:CLOUD_URL = 'http://127.0.0.1:8080/api/v1/readings'
$env:CLOUD_API_KEY = '<credencial del Cloud>'
python edge/app.py
```

El detalle de cada variable está en "How to run locally" del README de `edge-api`. El Cloud y la web se levantan como dice `cloud-api/README.md` y la guía de levantar y probar del equipo; el local `SITE_CODE` tiene que existir en el Cloud antes del primer lote.

**Ver los lotes en vivo** (deja esta terminal visible en el video):

```sh
mosquitto_sub -h localhost -u edge-api -P <clave> -t "comfort/#" -v
```

## 4. Pruebas

Al encender, el PIR necesita un minuto para estabilizarse y el sketch lo ignora ese minuto. El monitor serie imprime cada 2 s una línea como esta, que es la que se mira en casi todas las pruebas:

```
T=23.4 RH=55.0 ruido=41.2 ADC=650 presencia=0 distancia=nan cm hora=1795000123 wifi=1 mqtt=1 cola=0
```

Reglas de la luz de la puerta (`DeviceSignals.h`). El ruido solo cuenta si la sala está ocupada:

| Luz | Cuándo |
|---|---|
| Verde fijo | Óptima: 20 a 26 °C, 30 a 60 % de humedad y, si está ocupada, ruido de 55 dB o menos |
| Ámbar fijo | Moderada: fuera de 20 a 26 °C o de 30 a 60 %, o ocupada con más de 55 dB |
| Rojo parpadeando | No recomendada: fuera de 18 a 28 °C o de 25 a 70 %, o ocupada con más de 60 dB. Al entrar en rojo, el buzzer suena 4 s |
| Ámbar parpadeando | Aviso del modelo: probabilidad de 0,85 o más de llegar al límite de temperatura en 15 min |
| Apagadas | Sin lectura válida del DHT22 |

Si la humedad del ambiente pasa de 60 %, la luz queda en ámbar aunque el ruido esté al mínimo. No es una falla: se ve en `RH=` y hay que explicarlo en el video.

| # | Prueba | Cómo | Resultado esperado | ¿Pasó? |
|---|---|---|---|---|
| 1 | Arranque | Pulsar EN (reset) con el monitor serie abierto | `AUTOPRUEBA red C++ vs Keras: error maximo 1.19e-07 OK`, `ZenRoom listo. Esperando hora UTC y DHT22 validos.` y luego `[MQTT] conectado; lotes en comfort/<site>/room/<room>/readings`. La línea de estado muestra `wifi=1 mqtt=1` y una `hora=` mayor que 1577836800 | |
| 2 | Seis lotes por minuto | Mirar `mosquitto_sub` durante un minuto | Un mensaje en `comfort/<site>/room/<room>/readings` cada 10 s, con `ts` terminado en `:00`, `:10` … `:50`, y uno en `.../device/<id>/forecast` por minuto. En el serie, `Lote N guardado; pendientes 1` cada 10 s y `cola=0` entre lotes | |
| 3 | El Edge completa el minuto | `curl http://localhost:5000/api/v1/rooms/<room>/minutes?limit=3` | En los minutos cerrados, `"quality": {"batches": 6, "expected": 6}` y valores de `l10`, `l50` y `l90` | |
| 4 | Web | Entrar como administrador | **Dispositivos** muestra `<DEVICE_ID>` como «Conectado» y el detalle de la sala muestra temperatura, humedad y ruido del dispositivo en uno o dos minutos | |
| 5 | Presencia con el HC-SR04 | Poner la mano o una caja a menos de 150 cm frente al sensor | `distancia=` con el valor en cm y `presencia=1`. Al retirarla, `presencia=0` a los 30 s. En la web, la sala pasa a «Ocupada» en el minuto siguiente | |
| 6 | Presencia con el PIR | Pasado el primer minuto, mover la mano frente al PIR sin nada delante del HC-SR04 | `presencia=1`, se mantiene 30 s después del último movimiento | |
| 7 | Potenciómetro y luces | Con la sala ocupada (prueba 5 o botón 3), girar el potenciómetro de mínimo a máximo despacio | Mínimo: verde (`ruido=` cerca de 30). Pasando 55 dB (un poco más de un tercio del giro): ámbar fijo. Pasando 60 dB (un poco menos de la mitad): rojo parpadeando y el buzzer suena 4 s una vez | |
| 8 | Botón 1 (GPIO 13) | Pulsarlo una vez | Pitido corto y `Prueba manual del buzzer: 250 ms` | |
| 9 | Botón 2 (GPIO 25) | Con la luz en rojo, pulsarlo. Bajar el potenciómetro y volver a subirlo antes de 60 s. Repetirlo pasado el minuto | `Avisos automaticos silenciados durante 60 s` y el buzzer se calla. Dentro del minuto, volver a rojo no suena (la luz sí parpadea). Pasado el minuto, volver a rojo suena otra vez | |
| 10 | Botón 3 (GPIO 32) | Pulsarlo tres veces, con unos segundos entre cada una | `Presencia manual: ocupada (caduca en 60 s)` con `presencia=1`, luego `vacia` con `presencia=0` aunque haya algo delante del HC-SR04, y luego `automatica`. Si se deja en ocupada o vacía, vuelve sola a automática a los 60 s | |
| 11 | El pronóstico sube al calentar el DHT22 | Dejar el dispositivo al menos 11 minutos encendido con la sala ocupada (una caja frente al HC-SR04). Después calentar el DHT22 con la mano o el aliento, 3 a 5 minutos | Cada minuto aparece `modelo: prob=... limite=25.00 historia=lista`. Mientras `T=` sube hacia el límite, `prob=` sube; con 0,85 o más la luz pasa a ámbar parpadeando y la web muestra el aviso temprano de temperatura | |
| 12 | Desconectar y reconectar el USB | Quitar el cable 2 minutos y volver a ponerlo | Al quitarlo, `mosquitto_sub` recibe `offline` en `.../device/<id>/status` cuando el broker da por perdido al dispositivo (hasta 90 s) y la web lo pasa a «Desconectado». Al reconectar, el arranque de la prueba 1, `online` de nuevo y «Conectado» en la web | |
| 13 | Broker caído: los lotes se guardan y se reenvían | Detener Mosquitto (Ctrl+C) 2 minutos y volver a arrancarlo con el mismo comando | Durante la caída: `[MQTT] conexion perdida`, `[MQTT] sin conexion con <ip>:1883 ...; se reintenta` y `Lote N guardado; pendientes K` con K subiendo de 6 en 6 por minuto (`cola=K`). Al volver: `[MQTT] conectado`, la cola baja a 0 en pocos segundos y los minutos de la caída aparecen en el Edge con `batches: 6` | |
| 14 | Edge caído | Detener solo `python edge/app.py` 2 minutos y volver a arrancarlo | El dispositivo sigue publicando (`cola=0`): el broker guarda los lotes para la sesión persistente del Edge. Al volver, el Edge procesa esos lotes y los minutos del hueco quedan completos | |

Notas para las pruebas:

- **Prueba 11.** El modelo solo responde con la sala ocupada y por debajo del límite. Si `T=` llega al límite, `prob=` vuelve a 0 y manda la regla de la luz: es lo esperado. Para no tener que calentar tanto, se puede bajar el límite a un grado por encima de la temperatura actual: `mosquitto_pub -h localhost -u edge-api -P <clave> -t comfort/<site>/device/<id>/cmd -m '{"temp_limit": 24}'`. El serie confirma con `[MQTT] nuevo limite de temperatura: 24.00 C` (acepta de 18 a 30).
- **Prueba 13.** La cola guarda hasta 120 lotes (20 minutos). Pasado eso imprime `Cola llena: lote nuevo descartado`. La cola está en la flash, así que también sobrevive a la prueba 12: los lotes pendientes al quitar el cable se envían al volver.
- **Lectura fallida del DHT22.** Si una lectura sale mal, el serie dice `DHT22 sin lectura: se usa la ultima valida` y el lote sigue. Si el sensor no responde durante más de 10 s, dice `DHT22 invalido: revisar GPIO23; se omite la ventana`, las luces se apagan y no se publican lotes.

## 5. Problemas frecuentes

| Síntoma | Causa y arreglo |
|---|---|
| `Falta DeviceSecrets.h` al compilar | No se copió `DeviceSecrets.example.h` como `DeviceSecrets.h` en la carpeta del sketch |
| `wifi=0` todo el tiempo | Red de 5 GHz, SSID o clave mal escritos |
| `[MQTT] sin conexion con ... (error -1 o -2)` | IP del broker equivocada, firewall de Windows o Mosquitto apagado |
| `[MQTT] sin conexion con ... (error 4 o 5)` | Usuario o clave del broker incorrectos |
| `hora=` pequeña y no salen lotes | Todavía no hay hora por NTP: la red no tiene salida a internet |
| La web no muestra el dispositivo | `SITE_CODE` distinto entre el sketch, el Edge y el Cloud, o el Edge escucha otro broker |
| `LittleFS no monta` | Esquema de particiones sin SPIFFS (sección 1, paso 4) |
| `presencia=1` sin nadie | El PIR está en su primer minuto o tiene la sensibilidad alta; ajustar los potenciómetros del módulo |

## 6. Evidencia que se entrega

- El video con las pruebas 1 a 14 y el monitor serie visible.
- El log serie completo del arranque (copiar del monitor a un `.txt`).
- Capturas de la web con el código del dispositivo real: Dispositivos y detalle de la sala.
- La salida de `curl .../minutes?limit=3` de la prueba 3.
