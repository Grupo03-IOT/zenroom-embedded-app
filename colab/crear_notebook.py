"""Crea zenroom_forecast_training.ipynb con la misma secuencia de celdas que Data_training_App4.ipynb.

Uso: python crear_notebook.py   (luego se ejecuta en Colab o con nbconvert --execute)
"""
import nbformat as nbf

celdas = [
    ('md', '# ZenRoom · Aviso anticipado de calor en una sala\n\n'
           'Red pequeña que, con lo que mide el dispositivo de la sala, estima la probabilidad de que '
           'la temperatura pase el límite de la sala en los próximos 15 minutos. Mismo flujo que la '
           'Aplicación 4: se entrena aquí, se exportan los pesos a un `.h` y el ESP32 hace la inferencia en C++.'),
    ('md', '***Versión de TensorFlow:***'),
    ('code', 'import tensorflow as tf\nprint(tf.__version__)'),
    ('md', '***Importar paquetes necesarios:***'),
    ('code', 'import numpy as np\nimport pandas as pd\nimport matplotlib.pyplot as plt\nfrom tensorflow.keras import layers'),
    ('md', '***Configuraciones:***'),
    ('code', "CSV = 'zenroom_minutos.csv'      # datos por minuto (generar_datos.py, fisica del simulador del Edge)\n"
             "HORIZONTE = 15                  # minutos hacia adelante\n"
             "VENTANA = 10                    # minutos de historia que guarda el dispositivo\n"
             "LIMITES = [24.0, 24.5, 25.0]    # limites de temperatura de sala con los que se entrena\n"
             "val_ratio, test_ratio = 0.2, 0.2\n"
             "FEATURES = ['temp_menos_limite', 'subida_10min', 'humedad', 'ocupacion_10min', 'ruido_10min']\n"
             "tf.keras.utils.set_random_seed(1234)"),
    ('md', '***Cargar los datos:*** en Colab, subir `zenroom_minutos.csv` al panel de archivos.'),
    ('code', "df = pd.read_csv(CSV)\nprint(df.shape)\ndf.head()"),
    ('md', '***Ver un día de una sala:*** la temperatura sube cuando hay gente y baja cuando se va.'),
    ('code', "d = df[(df.sala == 'sala-03') & (df.minuto.between(1440, 2880))]\n"
             "fig, ax = plt.subplots(2, 1, figsize=(10, 5), sharex=True)\n"
             "ax[0].plot(d.hora, d.temp_c); ax[0].axhline(25, color='r', ls='--'); ax[0].set_ylabel('°C')\n"
             "ax[1].fill_between(d.hora, d.ocupada, step='mid', alpha=.4); ax[1].plot(d.hora, d.ruido_db / 100, lw=.5)\n"
             "ax[1].set_xlabel('hora'); ax[0].set_title('sala-03, un día'); plt.show()"),
    ('md', '***Construir entradas y etiqueta:*** una muestra por minuto en que la sala está ocupada y todavía '
           'por debajo del límite. La etiqueta es 1 si en los próximos 15 minutos llega al límite. No hay que '
           'etiquetar a mano: el futuro de la serie es la respuesta.'),
    ('code', "def leq(x):\n"
             "    return 10 * np.log10(np.mean(10 ** (np.asarray(x) / 10)))   # los dB se promedian en energia\n\n"
             "partes = {'train': [], 'val': [], 'test': []}\n"
             "for sala, g in df.groupby('sala'):\n"
             "    g = g.sort_values('minuto').reset_index(drop=True)\n"
             "    t = g.temp_c\n"
             "    futuro = t[::-1].rolling(HORIZONTE, min_periods=HORIZONTE).max()[::-1].shift(-1)\n"
             "    base = pd.DataFrame({\n"
             "        'sala': sala, 'minuto': g.minuto, 'temp': t, 'subida_10min': t - t.shift(VENTANA), 'humedad': g.rh_pct,\n"
             "        'ocupacion_10min': g.ocupada.rolling(VENTANA).mean(), 'ocupada_ahora': g.ocupada,\n"
             "        'ruido_10min': g.ruido_db.rolling(VENTANA).apply(leq, raw=True), 'futuro': futuro,\n"
             "        'temp_c': g.temp_c, 'rh_pct': g.rh_pct, 'ocupada': g.ocupada, 'ruido_db': g.ruido_db})\n"
             "    n = len(base); cortes = {'train': (0, .6), 'val': (.6, .8), 'test': (.8, 1.0)}\n"
             "    for nombre, (a, b) in cortes.items():          # particion temporal, nunca aleatoria\n"
             "        trozo = base.iloc[int(a * n):int(b * n)].dropna()\n"
             "        for L in LIMITES:\n"
             "            c = trozo[(trozo.ocupada_ahora == 1) & (trozo.temp < L)].copy()\n"
             "            c['temp_menos_limite'] = c.temp - L; c['limite'] = L\n"
             "            c['y'] = (c.futuro >= L).astype(float)\n"
             "            partes[nombre].append(c)\n"
             "train, val, test = (pd.concat(partes[k]) for k in ('train', 'val', 'test'))\n"
             "for k, p in (('train', train), ('val', val), ('test', test)):\n"
             "    print(f'{k}: {len(p)} muestras, {int(p.y.sum())} llegan al limite ({p.y.mean():.1%})')"),
    ('md', '***Normalizar:*** la media y la desviación se guardan para que el ESP32 haga lo mismo.'),
    ('code', "mu = train[FEATURES].mean().values; sd = train[FEATURES].std().values + 1e-6\n"
             "norm = lambda p: ((p[FEATURES].values - mu) / sd).astype('float32')\n"
             "x_train, x_val, x_test = norm(train), norm(val), norm(test)\n"
             "y_train, y_val, y_test = train.y.values, val.y.values, test.y.values"),
    ('md', '***Crear modelo:*** mismo tamaño que la red de la Aplicación 4, con 6 entradas y salida sigmoide.'),
    ('code', "model = tf.keras.Sequential([\n"
             "    layers.Input(shape=(len(FEATURES),)),\n"
             "    layers.Dense(16, activation='relu'),\n"
             "    layers.Dense(16, activation='relu'),\n"
             "    layers.Dense(1, activation='sigmoid')])\n"
             "model.summary()"),
    ('md', '***Compilar:*** los casos positivos son pocos, por eso pesan más en la pérdida.'),
    ('code', "peso_pos = (y_train == 0).sum() / (y_train == 1).sum()\n"
             "model.compile(optimizer='adam', loss='binary_crossentropy', metrics=['AUC'])\n"
             "print('peso de la clase 1:', round(peso_pos, 1))"),
    ('md', '***Entrenar:***'),
    ('code', "history = model.fit(x_train, y_train, epochs=40, batch_size=256, verbose=0,\n"
             "                    validation_data=(x_val, y_val), class_weight={0: 1.0, 1: float(peso_pos)})\n"
             "print('perdida final train/val:', round(history.history['loss'][-1], 4), round(history.history['val_loss'][-1], 4))"),
    ('md', '***Trazar el historial de entrenamiento:***'),
    ('code', "plt.plot(history.history['loss'], 'bo', label='Pérdida de entrenamiento')\n"
             "plt.plot(history.history['val_loss'], 'b', label='Pérdida de validación')\n"
             "plt.title('Pérdida de entrenamiento y validación'); plt.xlabel('época'); plt.legend()\n"
             "plt.savefig('grafica-perdida.png', dpi=120, bbox_inches='tight'); plt.show()"),
    ('md', '***Elegir el umbral con validación:*** el que deja al menos 85 % de precisión con el mayor número de avisos.'),
    ('code', "p_val = model.predict(x_val, verbose=0)[:, 0]\n"
             "def medir(alarma, y):\n"
             "    tp = int((alarma & (y == 1)).sum()); fp = int((alarma & (y == 0)).sum())\n"
             "    return tp / max(1, (y == 1).sum()), tp / max(1, tp + fp)\n"
             "UMBRAL = 0.5\n"
             "for u in np.arange(0.5, 0.96, 0.05):\n"
             "    rec, pre = medir(p_val > u, y_val)\n"
             "    if pre >= 0.85: UMBRAL = round(float(u), 2); break\n"
             "print('umbral elegido:', UMBRAL)"),
    ('md', '***Comparar contra la regla sin modelo (en prueba, datos que la red nunca vio):*** si la red no '
           'le gana a «estar cerca del límite y subiendo», no tiene sentido usarla.'),
    ('code', "p_test = model.predict(x_test, verbose=0)[:, 0]\n"
             "filas = [('Red neuronal', p_test > UMBRAL)]\n"
             "for m in (0.3, 0.5):\n"
             "    filas.append((f'Regla: temp >= limite-{m} y subiendo',\n"
             "                  (test.temp_menos_limite.values >= -m) & (test.subida_10min.values > 0)))\n"
             "filas.append(('Regla: temp >= limite-0.3', test.temp_menos_limite.values >= -0.3))\n"
             "tabla = pd.DataFrame([(n, *medir(a, y_test)) for n, a in filas], columns=['metodo', 'avisos anticipados', 'precision'])\n"
             "tabla['avisos anticipados'] = (tabla['avisos anticipados'] * 100).round(0).astype(int).astype(str) + ' %'\n"
             "tabla['precision'] = (tabla['precision'] * 100).round(0).astype(int).astype(str) + ' %'\n"
             "print(f'{int(y_test.sum())} cruces del limite en prueba'); tabla"),
    ('md', '***Trazar predicciones contra lo que pasó:*** un episodio de prueba de la sala-03 con límite de 25 °C: '
           'la sala estaba vacía, entra gente, se calienta y pasa el límite.'),
    ('code', "s3 = df[df.sala == 'sala-03'].sort_values('minuto').reset_index(drop=True)\n"
             "inicio_test = int(.8 * len(s3))\n"
             "t = s3.temp_c.values; o = s3.ocupada.values\n"
             "# primer cruce de 25 C en prueba que empieza con la sala vacia y fria\n"
             "cruce = next(i for i in range(inicio_test + 40, len(s3)) if t[i] >= 25 > t[i - 1] and t[i - 35] < 23.5 and o[i - 35] == 0)\n"
             "entrada = max(i for i in range(cruce - 60, cruce) if o[i] == 1 and o[i - 1] == 0)\n"
             "ep = s3.iloc[entrada - 12:cruce + 6].copy()\n"
             "def entradas(win, L=25.0):\n"
             "    tt, oo, rr = win.temp_c.values, win.ocupada.values, win.ruido_db.values\n"
             "    filas = []\n"
             "    for k in range(len(win)):\n"
             "        if k < VENTANA or oo[k] == 0 or tt[k] >= L: filas.append(None); continue\n"
             "        filas.append([tt[k] - L, tt[k] - tt[k - VENTANA], win.rh_pct.values[k], oo[k - VENTANA + 1:k + 1].mean(), leq(rr[k - VENTANA + 1:k + 1])])\n"
             "    return filas\n"
             "fil = entradas(ep)\n"
             "ep['prob_keras'] = [float(model.predict(((np.array([f]) - mu) / sd).astype('float32'), verbose=0)[0, 0]) if f else 0.0 for f in fil]\n"
             "ep['minuto_rel'] = np.arange(len(ep)) - 12\n"
             "ep.to_csv('episodio_demo.csv', index=False)\n"
             "aviso = ep[ep.prob_keras > UMBRAL].minuto_rel.min(); llega = int(ep[ep.temp_c >= 25].minuto_rel.min())\n"
             "print(f'entra gente en el minuto 0, la red avisa en el minuto {aviso}, la sala llega a 25 C en el minuto {llega}')\n"
             "fig, ax = plt.subplots(2, 1, figsize=(10, 5), sharex=True)\n"
             "ax[0].plot(ep.minuto_rel, ep.temp_c, label='temperatura'); ax[0].axhline(25, color='r', ls='--', label='límite')\n"
             "ax[0].fill_between(ep.minuto_rel, 21, 21 + ep.ocupada, step='mid', alpha=.2, label='ocupada'); ax[0].legend(); ax[0].set_ylabel('°C')\n"
             "ax[1].plot(ep.minuto_rel, ep.prob_keras, 'r', label='probabilidad (red)'); ax[1].axhline(UMBRAL, color='gray', ls=':', label='umbral')\n"
             "ax[1].axvline(llega, color='r', ls='--'); ax[1].legend(); ax[1].set_xlabel('minutos desde que entra gente')\n"
             "plt.suptitle('Comparación de predicciones con valores reales')\n"
             "plt.savefig('grafica-predicciones.png', dpi=120, bbox_inches='tight'); plt.show()"),
    ('md', '***Exportar los pesos de la red (forecast_weights.h) para el ESP32 en Wokwi:*** igual que '
           '`signal_weights.h`. Los kernels de Keras son (entrada, salida) y el bucle de C++ recorre '
           '(salida, entrada), por eso se transponen. Se añaden la normalización, el umbral y 8 casos de '
           'referencia para que el ESP32 compruebe al arrancar que da lo mismo que Keras.'),
    ('code', "w = model.get_weights()\n"
             "nombres = ['NN_W1', 'NN_B1', 'NN_W2', 'NN_B2', 'NN_W3', 'NN_B3']\n"
             "arr = lambda n, a: 'const float %s[%d] = {%s};\\n' % (n, a.size, ', '.join('%.9ff' % v for v in a.flatten()))\n"
             "out = '#ifndef FORECAST_WEIGHTS_H\\n#define FORECAST_WEIGHTS_H\\n\\n'\n"
             "out += '// Red %d-16-16-1 (Dense ReLU, Dense ReLU, Dense sigmoide) entrenada en zenroom_forecast_training.ipynb\\n' % len(FEATURES)\n"
             "out += '// Entradas: ' + ', '.join(FEATURES) + '\\n\\n'\n"
             "out += '#define NN_IN %d\\n#define NN_H 16\\n#define NN_UMBRAL %.2ff\\n#define NN_HORIZONTE_MIN %d\\n#define NN_VENTANA %d\\n\\n' % (len(FEATURES), UMBRAL, HORIZONTE, VENTANA)\n"
             "for i, (n, a) in enumerate(zip(nombres, w)):\n"
             "    out += arr(n, a.T if i % 2 == 0 else a)\n"
             "out += arr('NN_MU', mu.astype('float32')) + arr('NN_SD', sd.astype('float32'))\n"
             "ref = pd.concat([test[test.y == 1].sample(4, random_state=1), test[test.y == 0].sample(4, random_state=1)])\n"
             "out += arr('REF_X', ref[FEATURES].values.astype('float32'))\n"
             "out += arr('REF_P', model.predict(norm(ref), verbose=0)[:, 0])\n"
             "out += '#define REF_N 8\\n\\n#endif\\n'\n"
             "open('forecast_weights.h', 'w').write(out)\n"
             "print(out[:600])"),
    ('md', '***Comprobar la exportación:*** la misma cuenta que hará el ESP32, en numpy.'),
    ('code', "def red_c(x):\n"
             "    W1, B1, W2, B2, W3, B3 = w\n"
             "    h = np.maximum(((x - mu) / sd) @ W1 + B1, 0); h = np.maximum(h @ W2 + B2, 0)\n"
             "    return 1 / (1 + np.exp(-(h @ W3 + B3)[:, 0]))\n"
             "print('diferencia maxima numpy vs Keras:', float(np.abs(red_c(test[FEATURES].values) - p_test).max()))"),
]

nb = nbf.v4.new_notebook()
nb.cells = [nbf.v4.new_markdown_cell(c) if t == 'md' else nbf.v4.new_code_cell(c) for t, c in celdas]
nb.metadata['kernelspec'] = {'name': 'python3', 'display_name': 'Python 3', 'language': 'python'}
nbf.write(nb, 'zenroom_forecast_training.ipynb')
print('ok')
