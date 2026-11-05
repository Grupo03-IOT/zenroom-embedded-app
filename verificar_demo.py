"""Verificacion de la demo ZenRoom Edge ML (mismo metodo que verificar_app4.py).

  python verificar_demo.py modelo     # sin simulacion: el .h reproduce a Keras
  python verificar_demo.py simular    # Wokwi reproduce el episodio de colab/episodio_demo.csv y la PC manda el limite por MQTT
  python verificar_demo.py analizar   # lee los logs y escribe evidencia-verificacion.txt
  python verificar_demo.py graficar   # evidencia-dispositivo-vs-keras.png y evidencia-luz-puerta.png

Requiere: WOKWI_CLI_TOKEN, `python -m platformio run`, paho-mqtt, numpy, matplotlib.
"""
import csv, json, os, re, subprocess, sys, time
import numpy as np
from red_desde_header import cargar, red

AQUI = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.expanduser(r"~/.wokwi/bin/wokwi-cli.exe")
HEADER = os.path.join(AQUI, 'src', 'forecast_weights.h')
EPISODIO = os.path.join(AQUI, 'colab', 'episodio_demo.csv')
ESCENARIO = os.path.join(AQUI, 'escenario-episodio.yaml')
DIAG_VERIF = 'diagram-verificacion.json'
LOG = os.path.join(AQUI, 'evidencia-serial.log')
VCD = os.path.join(AQUI, 'evidencia-luz.vcd')
LOG_MQTT = os.path.join(AQUI, 'evidencia-mqtt.jsonl')
BROKER = 'broker.hivemq.com'
TOPIC_BASE = 'upc/iot/sensework-verif-gh7'      # el mismo de platformio.ini
DEVICE = 'zr-sala-03'
LINEA = re.compile(r'min=(\d+) temp=([-\d.]+) subida=([-\d.]+) hum=([-\d.]+) ocup=(\d) ocup10=([\d.]+) '
                   r'ruido10=([\d.]+) limite=([\d.]+) prob=([\d.]+) trans=(\d+) estado=(\w+)')


def episodio():
    with open(EPISODIO, encoding='utf-8') as f:
        return list(csv.DictReader(f))


# ------------------------------------------------------------------ modelo
def modelo():
    a, d = cargar(HEADER)
    ref = red(a, d, a['REF_X'].reshape(-1, d['NN_IN']))
    print(f"[modelo] pesos del .h vs Keras en los 8 casos de referencia: error maximo {np.abs(ref - a['REF_P']).max():.2e}")
    print(f"[modelo] arquitectura {d['NN_IN']}-{d['NN_H']}-{d['NN_H']}-1, umbral {d['UMBRAL']}")


# ------------------------------------------------------------------ escenario
PERIODO_MS = 2200   # SAMPLE_MS del firmware


def escribir_escenario():
    """Pasos de tiempo simulado: cada fila del episodio se aplica a mitad de un periodo de muestreo,
    asi nunca coincide con una lectura. Sincronizar por texto pierde lineas (probado)."""
    filas = episodio()
    pasos = ['  - wait-serial: "AUTOPRUEBA red C++ vs Keras"', '  - wait-serial: "[MQTT] conectado"',
             '  - wait-serial: "min="', f'  - delay: {PERIODO_MS // 2}ms']
    for f in filas:
        pos = min(1.0, max(0.0, (float(f['ruido_db']) - 30.0) / 60.0))
        pasos += [f'  - set-control: {{ part-id: dht1, control: temperature, value: {float(f["temp_c"]):.2f} }}',
                  f'  - set-control: {{ part-id: dht1, control: humidity, value: {float(f["rh_pct"]):.1f} }}',
                  f'  - set-control: {{ part-id: pot1, control: position, value: {pos:.4f} }}',
                  f'  - set-control: {{ part-id: btnPrueba, control: pressed, value: {int(f["ocupada"])} }}',
                  f'  - delay: {PERIODO_MS}ms']
    # parte 2: el limite llega por MQTT (lo publica la PC al ver el final del episodio en el log)
    pasos += ['  - wait-serial: "[MQTT] nuevo limite de temperatura: 24.00 C"',
              '  - wait-serial: "limite=24.00"',
              '  - wait-serial: "[MQTT] limite 45.00 fuera de rango"']
    cabecera = ['name: ZenRoom - episodio de la sala-03 y limite por MQTT', 'version: 1',
                'author: Gilbert Huarcaya - 1ASI0572', 'steps:']
    return '\n'.join(cabecera + pasos) + '\n'


def min0_del_log():
    """Primer minuto impreso despues de conectar MQTT: es el que espera el escenario antes de reproducir."""
    vio = False
    for ln in open(LOG, encoding='utf-8', errors='ignore'):
        if '[MQTT] conectado' in ln: vio = True
        m = re.match(r'min=(\d+) ', ln)
        if vio and m: return int(m.group(1))


def simular():
    import paho.mqtt.client as mqtt
    for p in (LOG, VCD, LOG_MQTT):
        if os.path.exists(p): os.remove(p)
    # El primer "min=" que ve el escenario es el minuto en que arranca la reproduccion; el dispositivo
    # tarda lo que tarde el WiFi, asi que los numeros se fijan despues: se reescribe al vuelo.
    base = escribir_escenario()
    filas = episodio()
    recibidos = []
    cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f'verif-zenroom-{os.getpid()}')
    cli.on_message = lambda c, u, m: recibidos.append({'t': time.time(), 'topic': m.topic, 'payload': m.payload.decode()})
    cli.connect(BROKER, 1883, 60); cli.loop_start()
    cli.subscribe(f'{TOPIC_BASE}/{DEVICE}/forecast', 1)

    open(ESCENARIO, 'w', encoding='utf-8').write(base)

    sim = subprocess.Popen([CLI, AQUI, '--diagram-file', DIAG_VERIF, '--scenario', ESCENARIO, '--timeout', '900000',
                            '--serial-log-file', LOG, '--vcd-file', VCD], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    publicado = False
    while sim.poll() is None:
        time.sleep(0.5)
        if publicado or not os.path.exists(LOG): continue
        m0 = min0_del_log()
        if m0 and f'min={m0 + len(filas) + 1} ' in open(LOG, encoding='utf-8', errors='ignore').read():
            cli.publish(f'{TOPIC_BASE}/{DEVICE}/cmd', json.dumps({'temp_limit': 24.0}), qos=1)
            time.sleep(4)
            cli.publish(f'{TOPIC_BASE}/{DEVICE}/cmd', json.dumps({'temp_limit': 45}), qos=1)
            publicado = True
    salida = sim.stdout.read().decode(errors='ignore')
    time.sleep(2); cli.loop_stop()
    with open(LOG_MQTT, 'w', encoding='utf-8') as f:
        for r in recibidos: f.write(json.dumps(r) + '\n')
    print(salida[-600:])
    print(f'[simular] codigo {sim.returncode}, mensajes MQTT recibidos en la PC: {len(recibidos)}, MIN0={min0_del_log()}')


# ------------------------------------------------------------------ analisis
def lineas_dispositivo():
    out = []
    for ln in open(LOG, encoding='utf-8', errors='ignore'):
        m = LINEA.search(ln)
        if m:
            g = m.groups()
            out.append({'min': int(g[0]), 'temp': float(g[1]), 'subida': float(g[2]), 'hum': float(g[3]), 'ocup': int(g[4]),
                        'ocup10': float(g[5]), 'ruido10': float(g[6]), 'limite': float(g[7]), 'prob': float(g[8]),
                        'trans': int(g[9]), 'estado': g[10]})
    return out


def vcd_cambios(path):
    """{senal: [(t_ns, valor), ...]} de un VCD de Wokwi."""
    ids, cambios, t = {}, {}, 0
    for ln in open(path, encoding='utf-8', errors='ignore'):
        ln = ln.strip()
        if ln.startswith('$var'):
            p = ln.split(); ids[p[3]] = p[4]; cambios[p[4]] = []
        elif ln.startswith('#'):
            t = int(ln[1:])
        elif ln[:1] in '01' and ln[1:] in ids:
            cambios[ids[ln[1:]]].append((t, int(ln[0])))
    return cambios


def analizar():
    res = []
    ok = lambda c, txt: res.append(('PASS' if c else 'FAIL') + '  ' + txt)
    a, d = cargar(HEADER)
    log = open(LOG, encoding='utf-8', errors='ignore').read()
    m = re.search(r'AUTOPRUEBA red C\+\+ vs Keras: error maximo ([\d.e+-]+) (\w+)', log)
    ok(m and m.group(2) == 'OK', f'Autoprueba en el ESP32 (8 casos exportados de Keras): error maximo {m.group(1) if m else "?"}')
    ok('WiFi conectado.' in log and '[MQTT] conectado' in log, 'WiFi y MQTT conectados (broker.hivemq.com)')

    MIN0 = min0_del_log()
    filas = episodio(); disp = {x['min']: x for x in lineas_dispositivo()}
    pares = []
    for r, f in enumerate(filas):
        x = disp.get(MIN0 + 1 + r)
        if x: pares.append((r, f, x))
    ok(len(pares) == len(filas), f'Minutos del episodio leidos por el dispositivo: {len(pares)}/{len(filas)}')

    dt = [abs(x['temp'] - float(f['temp_c'])) for _, f, x in pares]
    ok(max(dt) <= 0.06, f'Temperatura que lee el DHT22 vs la del episodio: diferencia maxima {max(dt):.2f} C (resolucion del sensor 0,1 C)')
    dr = [abs(x['ocup'] - int(f['ocupada'])) for _, f, x in pares]
    ok(sum(dr) == 0, f'Presencia leida igual a la del episodio en {len(pares) - sum(dr)}/{len(pares)} minutos')

    # 1) la cuenta en C++ es la de la red: se recalcula con numpy desde las entradas que imprime el ESP32
    err = []
    for _, f, x in pares:
        if x['prob'] > 0:
            p = red(a, d, [[x['temp'] - x['limite'], x['subida'], x['hum'], x['ocup10'], x['ruido10']]])[0]
            err.append(abs(p - x['prob']))
    ok(err and max(err) < 0.02, f'prob del ESP32 vs numpy con sus mismas entradas impresas: error maximo {max(err):.3f} ({len(err)} minutos; las entradas se imprimen redondeadas)')

    # 2) comportamiento frente a Keras sobre el episodio original
    pk = np.array([float(f['prob_keras']) for _, f, _ in pares]); pd_ = np.array([x['prob'] for *_, x in pares])
    rel = np.array([int(f['minuto_rel']) for _, f, _ in pares])
    ok(np.abs(pk - pd_).mean() < 0.05, f'prob del ESP32 vs Keras en el episodio: diferencia media {np.abs(pk - pd_).mean():.3f}, maxima {np.abs(pk - pd_).max():.3f}')
    av_k = rel[pk > d['UMBRAL']].min(); av_d = rel[pd_ >= d['UMBRAL']].min()
    llega = min(int(f['minuto_rel']) for f in filas if float(f['temp_c']) >= 25.0)
    ok(abs(av_k - av_d) <= 1, f'Primer aviso: Keras en el minuto {av_k}, ESP32 en el minuto {av_d}; la sala llega a 25 C en el minuto {llega} ({llega - av_d} min de anticipacion)')
    est = {r: x['estado'] for r, _, x in pares}
    ok(all(est[r] != 'AVISO_CALOR' for r in est if int(filas[r]['minuto_rel']) < av_d), 'Antes del aviso no hay parpadeo ambar (sin falsas alarmas en el episodio)')
    ok(all(x['estado'] == 'MODERADA' for _, f, x in pares if x['temp'] >= 25.0 and x['ruido10'] < 60),
       'Al pasar el limite la luz queda ambar fija (MODERADA), como la regla de la web app')

    # 3) luz de la puerta en el analizador logico
    c = vcd_cambios(VCD)
    ambar = next((v for k, v in c.items() if k.endswith('D1')), [])
    flancos = [t for t, v in ambar if v == 1]
    periodos = np.diff(flancos) / 1e6 if len(flancos) > 2 else np.array([])
    parpadeo = periodos[(periodos > 600) & (periodos < 1000)]
    ok(len(parpadeo) >= 5, f'Ambar parpadeando en el analizador logico (GPIO 19): {len(parpadeo)} periodos de {np.median(parpadeo) if len(parpadeo) else 0:.0f} ms (esperado 800 ms)')

    # 4) MQTT en los dos sentidos
    msgs = [json.loads(json.loads(l)['payload']) for l in open(LOG_MQTT, encoding='utf-8')] if os.path.exists(LOG_MQTT) else []
    ok(len(msgs) >= len(filas) // 2, f'La PC recibio {len(msgs)} mensajes del dispositivo en {TOPIC_BASE}/{DEVICE}/forecast')
    ok(any(m.get('prob', 0) >= d['UMBRAL'] for m in msgs), 'Al menos un mensaje MQTT lleva la probabilidad por encima del umbral')
    ok('[MQTT] nuevo limite de temperatura: 24.00 C' in log, 'Comando {"temp_limit": 24.0} enviado desde la PC y aplicado por el dispositivo')
    ok('limite=24.00' in log and any(m.get('temp_limit') == 24.0 for m in msgs), 'El dispositivo usa y publica el nuevo limite')
    ok('[MQTT] limite 45.00 fuera de rango' in log, 'Comando {"temp_limit": 45} rechazado (fuera de 18 a 30 C)')

    txt = '\n'.join(res)
    n_ok = sum(r.startswith('PASS') for r in res)
    txt += f'\n\n{n_ok} PASS / {len(res) - n_ok} FAIL\n'
    open(os.path.join(AQUI, 'evidencia-verificacion.txt'), 'w', encoding='utf-8').write(txt)
    print(txt)


def graficar():
    import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
    MIN0 = min0_del_log()
    filas = episodio(); disp = {x['min']: x for x in lineas_dispositivo()}
    rel = [int(f['minuto_rel']) for f in filas]
    xs = [disp.get(MIN0 + 1 + r) for r in range(len(filas))]
    fig, ax = plt.subplots(3, 1, figsize=(10, 7), sharex=True)
    ax[0].plot(rel, [float(f['temp_c']) for f in filas], label='episodio (simulador del Edge)')
    ax[0].plot(rel, [x['temp'] if x else np.nan for x in xs], '.', label='DHT22 en Wokwi')
    ax[0].axhline(25, color='r', ls='--', label='límite'); ax[0].set_ylabel('°C'); ax[0].legend()
    ax[1].plot(rel, [float(f['prob_keras']) for f in filas], 'b', label='Keras (Colab)')
    ax[1].plot(rel, [x['prob'] if x else np.nan for x in xs], 'r.', label='ESP32 (C++)')
    ax[1].axhline(0.85, color='gray', ls=':', label='umbral'); ax[1].set_ylabel('probabilidad'); ax[1].legend()
    codigo = {'SIN_DATOS': 0, 'OPTIMA': 1, 'AVISO_CALOR': 2, 'MODERADA': 3, 'NO_RECOMENDADA': 4}
    ax[2].step(rel, [codigo[x['estado']] if x else np.nan for x in xs], where='mid')
    ax[2].set_yticks(list(codigo.values())); ax[2].set_yticklabels(['sin datos', 'verde', 'ámbar parpadea', 'ámbar', 'rojo'])
    ax[2].set_xlabel('minutos desde que entra gente')
    fig.suptitle('ZenRoom en Wokwi: el ESP32 reproduce a la red de Colab y avisa antes de pasar el límite')
    fig.savefig(os.path.join(AQUI, 'evidencia-dispositivo-vs-keras.png'), dpi=120, bbox_inches='tight')
    print('evidencia-dispositivo-vs-keras.png')


if __name__ == '__main__':
    {'modelo': modelo, 'simular': simular, 'analizar': analizar, 'graficar': graficar}[sys.argv[1]]()
