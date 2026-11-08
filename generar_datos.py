"""Genera colab/zenroom_minutos.csv: 28 dias, minuto a minuto, de 3 salas.

Usa la misma fisica que el simulador del Edge (edge-api/simulator/device_sim.py): ocupacion
segun la hora, la sala se calienta ~3 C con gente (constante de ~15 min) y el ruido sube
al ocuparse. Es el reemplazo de los datos reales mientras no haya hardware.

Uso: python generar_datos.py [ruta-a-edge-api]
"""
import csv, datetime, os, random, sys

EDGE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(__file__), '..', '..', '..', '..', 'referencias', 'semana2', 'grupo03', 'edge-api')
sys.path.insert(0, os.path.join(EDGE, 'simulator'))
import device_sim as ds

DIAS = 28
random.seed(7)
salida = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'colab', 'zenroom_minutos.csv')
with open(salida, 'w', newline='', encoding='utf-8') as f:
    w = csv.writer(f)
    w.writerow(['sala', 'minuto', 'hora', 'dia_semana', 'temp_c', 'rh_pct', 'ocupada', 'ruido_db'])
    for i, sala in enumerate(['sala-01', 'sala-02', 'sala-03']):
        r = ds.Room(sala, i)
        t = datetime.datetime(2026, 9, 7)
        for m in range(DIAS * 1440):
            r.step_occupancy(t, 60)
            r.step_climate(60)
            niveles = [lv for _ in range(10) for lv in r.window_levels()]   # 10 s de muestras por minuto
            w.writerow([sala, m, round(t.hour + t.minute / 60, 3), t.weekday(), round(r.temp, 3),
                        round(r.rh, 2), int(r.occupied), round(ds.leq(niveles), 2)])
            t += datetime.timedelta(minutes=1)
print('escrito', salida)
