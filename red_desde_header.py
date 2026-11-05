"""Lee forecast_weights.h y hace la misma cuenta que redNeuronal() del ESP32 (para verificar sin simular)."""
import re, numpy as np
def cargar(path):
    src = open(path, encoding='utf-8').read()
    arr = {n: np.array([float(v.rstrip('f')) for v in body.split(',')], dtype=np.float32)
           for n, body in re.findall(r'const float (\w+)\[\d+\] = \{([^}]*)\};', src)}
    d = {n: int(v) for n, v in re.findall(r'#define (NN_IN|NN_H|REF_N) (\d+)', src)}
    d['UMBRAL'] = float(re.search(r'#define NN_UMBRAL ([\d.]+)f', src).group(1))
    return arr, d
def red(arr, d, x):
    x = (np.atleast_2d(x).astype(np.float32) - arr['NN_MU']) / arr['NN_SD']
    W1 = arr['NN_W1'].reshape(d['NN_H'], d['NN_IN']); W2 = arr['NN_W2'].reshape(d['NN_H'], d['NN_H'])
    h = np.maximum(x @ W1.T + arr['NN_B1'], 0); h = np.maximum(h @ W2.T + arr['NN_B2'], 0)
    return 1 / (1 + np.exp(-(h @ arr['NN_W3'] + arr['NN_B3'][0])))
