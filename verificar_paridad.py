"""Comprueba que la simulacion en Wokwi y el dispositivo real ejecutan el mismo firmware sobre los mismos pines.

Uso:  python verificar_paridad.py
Sale con codigo 1 si el diagrama de Wokwi y los pines del firmware no coinciden, o si los perfiles
difieren en algo distinto de lo permitido (red, cadencia, pulsador de pruebas).
"""
import json
import re
import subprocess
import sys

PERMITIDO = {"WIFI_SSID", "WIFI_PASS", "WIFI_CHANNEL", "MQTT_HOST", "MQTT_PORT", "TOPIC_BASE", "ROOM_ID",
             "DEVICE_ID", "KEEPALIVE_MS", "SAMPLE_MS", "PIN_PRUEBA", "CORE_DEBUG_LEVEL"}
fallos = []

# 1) Pines por omision del firmware (#define PIN_xxx n) frente a las conexiones de diagram.json
codigo = open("src/main.cpp", encoding="utf8").read()
pines = {m.group(1): int(m.group(2)) for m in re.finditer(r"#define (PIN_\w+) (\d+)", codigo)}
pines.update({n: int(v) for n, v in re.findall(r"(PIN_(?:VERDE|AMBAR|ROJO)) = (\d+)", codigo)})
for n, a, b in re.findall(r"PIN_VERDE = (\d+), PIN_AMBAR = (\d+), PIN_ROJO = (\d+)", codigo):
    pines.update({"PIN_VERDE": int(n), "PIN_AMBAR": int(a), "PIN_ROJO": int(b)})
diagrama = json.load(open("diagram.json", encoding="utf8"))
conectados = {int(c[i].split(":")[1]) for c in diagrama["connections"] for i in (0, 1)
              if c[i].startswith("esp:") and c[i].split(":")[1].isdigit()}
esperados = {"PIN_DHT": 23, "PIN_PIR": 27, "PIN_RUIDO": 34, "PIN_BUZZER": 26, "PIN_VERDE": 21, "PIN_AMBAR": 19,
             "PIN_ROJO": 18}
for nombre, gpio in esperados.items():
    if pines.get(nombre) != gpio:
        fallos.append(f"{nombre}: el firmware usa {pines.get(nombre)} y la guia de conexiones dice {gpio}")
    if gpio not in conectados:
        fallos.append(f"{nombre}: GPIO {gpio} no esta conectado en diagram.json")
if 13 not in conectados:
    fallos.append("el pulsador de pruebas (GPIO 13) no esta en diagram.json")

# 2) Banderas de compilacion de cada perfil
def banderas(env):
    salida = subprocess.run(["pio", "project", "config", "--json-output"], capture_output=True, text=True, check=True).stdout
    for seccion, opciones in json.loads(salida):
        if seccion == f"env:{env}":
            texto = " ".join(v if isinstance(v, str) else " ".join(v) for k, v in opciones if k == "build_flags")
            return dict(re.findall(r"-D(\w+)=?(\S*)", texto.replace("'", "")))
    return {}

real, sim = banderas("zenroom"), banderas("zenroom-sim")
for clave in sorted(set(real) | set(sim)):
    if real.get(clave) != sim.get(clave) and clave not in PERMITIDO:
        fallos.append(f"diferencia no permitida entre perfiles en {clave}: real={real.get(clave)} sim={sim.get(clave)}")

print("Diferencias permitidas entre zenroom (real) y zenroom-sim (Wokwi):")
for clave in sorted(set(real) | set(sim)):
    if real.get(clave) != sim.get(clave):
        print(f"  {clave:14} real={real.get(clave)!s:12} sim={sim.get(clave)}")
if fallos:
    print("\nFALLA:")
    print("\n".join("  - " + f for f in fallos))
    sys.exit(1)
print("\nOK: mismo firmware, mismos pines; Wokwi solo cambia red y cadencia.")
