"""Prepara el sketch de arduino/zenroom_esp32 para probarlo en wokwi.com (editor web, sin cuota de CLI).

Genera en arduino/wokwi/salida/:
  sketch.ino      el sketch con sus .h incrustados (Wokwi web acepta varios archivos, pero uno solo es mas facil de pegar)
                  y DeviceSecrets.h de simulacion (Wokwi-GUEST, canal 6, HiveMQ, modelo cada 10 s)
  diagram.json    montaje real de la Guia de conexiones: DHT22 23, PIR 27, HC-SR04 22/35, pot 34, buzzer 26,
                  LEDs 21/19/18 y pulsadores 13/25/32
  libraries.txt   lista de bibliotecas
  cargar.js       script para pegar en la consola de la pagina del proyecto (Monaco): carga sketch y diagrama

Uso: python generar_wokwi.py [--site c7iot-local] [--device zr-paula-01] [--room sala-01] [--broker broker.hivemq.com]
"""
import argparse
import json
import re
from pathlib import Path

AQUI = Path(__file__).resolve().parent
SKETCH = AQUI.parent / 'zenroom_esp32'
SALIDA = AQUI / 'salida'

ap = argparse.ArgumentParser()
ap.add_argument('--site', default='c7iot-local')
ap.add_argument('--device', default='esp32-sala-01')
ap.add_argument('--room', default='sala-01')
ap.add_argument('--broker', default='broker.hivemq.com')
args = ap.parse_args()

secretos = f'''// DeviceSecrets.h de SIMULACION (Wokwi). En el montaje real se usa el propio, fuera del repositorio.
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""
#define WIFI_CHANNEL 6
#define MQTT_HOST "{args.broker}"
#define MQTT_PORT 1883
#define MQTT_KEEPALIVE_MS 600000L
#define MODEL_STEP_MS 10000UL
#define SITE_CODE "{args.site}"
#define DEVICE_ID "{args.device}"
#define ROOM_ID "{args.room}"
'''

incrustados = {
    'DeviceSecrets.h': secretos,
    'DeviceConfig.h': (SKETCH / 'DeviceConfig.h').read_text(encoding='utf8'),
    'DeviceSignals.h': (SKETCH / 'DeviceSignals.h').read_text(encoding='utf8'),
    'ForecastModel.h': (SKETCH / 'ForecastModel.h').read_text(encoding='utf8'),
    'forecast_weights.h': (SKETCH / 'forecast_weights.h').read_text(encoding='utf8'),
}
vistos = set()


def incrustar(texto):
    def sustituir(m):
        nombre = m.group(1)
        if nombre not in incrustados:
            return m.group(0)
        if nombre in vistos:
            return ''
        vistos.add(nombre)
        cuerpo = incrustados[nombre]
        # DeviceConfig.h comprueba si existe DeviceSecrets.h: ya esta incrustado arriba
        cuerpo = re.sub(r'#if __has_include\("DeviceSecrets.h"\).*?#endif\n', '', cuerpo, flags=re.S)
        cuerpo = cuerpo.replace('#pragma once\n', '')
        return f'// ===== {nombre} =====\n' + incrustar(cuerpo)
    return re.sub(r'#include "([^"]+)"\n', sustituir, texto)


principal = (SKETCH / 'zenroom_esp32.ino').read_text(encoding='utf8')
# DeviceSecrets.h se incrusta primero (lo pide DeviceConfig.h)
principal = principal.replace('#include "DeviceConfig.h"\n', '#include "DeviceSecrets.h"\n#include "DeviceConfig.h"\n', 1)
sketch = incrustar(principal)

# diagrama: parte del existente y agrega lo que el montaje real tiene y la simulacion anterior no
diagrama = json.loads((AQUI.parent.parent / 'diagram.json').read_text(encoding='utf8'))
diagrama['parts'] += [
    {'type': 'wokwi-hc-sr04', 'id': 'ultra1', 'top': -220, 'left': -40, 'attrs': {'distance': '300'}},
    {'type': 'wokwi-pushbutton', 'id': 'btnSilencio', 'top': 90, 'left': -230, 'attrs': {'color': 'yellow', 'label': 'silenciar'}},
    {'type': 'wokwi-pushbutton', 'id': 'btnPresencia', 'top': 150, 'left': -230, 'attrs': {'color': 'green', 'label': 'presencia manual'}},
]
for p in diagrama['parts']:
    if p['id'] == 'btnPrueba':
        p['attrs']['label'] = 'prueba buzzer'
diagrama['connections'] += [
    ['ultra1:VCC', 'esp:5V', 'red', []],
    ['ultra1:GND', 'esp:GND.1', 'black', []],
    ['ultra1:TRIG', 'esp:22', 'green', []],
    ['ultra1:ECHO', 'esp:35', 'blue', []],
    ['btnSilencio:1.l', 'esp:25', 'yellow', []],
    ['btnSilencio:2.l', 'esp:GND.1', 'black', []],
    ['btnPresencia:1.l', 'esp:32', 'green', []],
    ['btnPresencia:2.l', 'esp:GND.1', 'black', []],
]
diag_txt = json.dumps(diagrama, indent=2, ensure_ascii=False)

SALIDA.mkdir(exist_ok=True)
(SALIDA / 'sketch.ino').write_text(sketch, encoding='utf8')
(SALIDA / 'diagram.json').write_text(diag_txt, encoding='utf8')
(SALIDA / 'libraries.txt').write_text('# Wokwi Library List\nArduinoMqttClient\nDHT sensor library\nAdafruit Unified Sensor\n', encoding='utf8')

js = ('async () => {\n  const m = {};\n  window.monaco.editor.getModels().forEach(x => { m[x.uri.toString()] = x; });\n'
      '  const claves = Object.keys(m);\n'
      '  const sk = m[claves.find(c => c.endsWith("sketch.ino"))];\n'
      '  const dgKey = claves.find(c => c.endsWith("diagram.json")); const dg = dgKey ? m[dgKey] : null;\n'
      '  sk.setValue(' + json.dumps(sketch) + ');\n  if (dg) dg.setValue(' + json.dumps(diag_txt) + ');\n'
      '  return { modelos: claves, lineasSketch: sk.getLineCount() };\n}')
(SALIDA / 'cargar.js').write_text(js, encoding='utf8')
print('sketch.ino:', len(sketch.splitlines()), 'lineas;', 'diagram.json:', len(diag_txt), 'bytes;', 'cargar.js:', len(js), 'bytes ->', SALIDA)
