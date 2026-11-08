"""Sirve arduino/wokwi/salida en http://127.0.0.1:8765 con CORS, para que la pagina de wokwi.com cargue
sketch.ino y diagram.json con fetch() en vez de pegar 40 KB en la consola."""
import functools
import http.server
from pathlib import Path

CARPETA = Path(__file__).resolve().parent / 'salida'


class Cors(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        super().end_headers()


http.server.ThreadingHTTPServer(('127.0.0.1', 8765), functools.partial(Cors, directory=str(CARPETA))).serve_forever()
