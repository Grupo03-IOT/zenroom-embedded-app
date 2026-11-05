# Guion para mostrar la demo de ZenRoom con el modelo entrenado

Entre corchetes, lo que se muestra en pantalla. Antes de empezar: abrir el proyecto, arrancar
la simulación y esperar `WiFi conectado.`; tener abierta la pestaña de HiveMQ conectada a
`broker.hivemq.com` y el cuaderno de Colab con las gráficas.

Proyecto Wokwi de referencia: https://wokwi.com/projects/477265645250624513 · Topics de integración: `comfort/site-1/room/sala-03/readings` y `comfort/site-1/device/zr-sala-03/forecast`

1. **Qué hicimos.** "Profe, tomamos lo de la Aplicación 4 y lo llevamos a nuestro proyecto. En
   vez de predecir un seno, la red predice si una sala con gente se va a pasar de temperatura
   en los próximos 15 minutos, y la luz de la puerta avisa antes."
2. **Datos** [primera gráfica del cuaderno]. "Como todavía no tenemos el hardware, generamos 28
   días de tres salas con el simulador de nuestro Edge. Aquí se ve cómo la sala se calienta
   cuando entra gente y se enfría cuando se va."
3. **Colab** [celda del modelo y tabla de comparación]. "Es la misma red de la App4, 16 y 16
   neuronas, pero con cinco entradas: la temperatura, cuánto subió en 10 minutos, la humedad,
   la ocupación y el ruido. La etiqueta sale sola, porque sabemos qué pasó 15 minutos
   después. La comparamos con una regla sin IA: la red avisa en el 91 % de los casos y
   acierta el 93 %, la regla se queda en 67 y 79."
4. **Pesos** [`forecast_weights.h`]. "Exportamos los pesos igual que `signal_weights.h`. Y al
   arrancar, el ESP32 prueba ocho casos de Colab y compara: sale error de una diezmillonésima."
   [señalar `AUTOPRUEBA ... OK` en el monitor serie]
5. **Circuito** [diagrama]. "Son los pines de nuestra sección 5.6: el DHT22 por el SHT31, el
   potenciómetro por el micrófono y el PIR por el radar. A la derecha la luz de la puerta."
6. **Borde** [función `redNeuronal` y `procesarMuestra`]. "Cada minuto guarda los últimos 10
   valores y, si la sala está ocupada y todavía no llega al límite, corre la red aquí mismo.
   No necesita internet para decidir."
7. **Demo** [PIR: Simulate motion; subir poco a poco la temperatura del DHT22]. "Entra gente y
   la sala se va calentando... a 24,7 grados la red ya da 0,85 y el ámbar empieza a
   parpadear, y todavía no llega a 25. En nuestra prueba avisó 10 minutos antes."
8. **Nube** [HiveMQ: publicar `{"temp_limit": 26}`]. "Y esto es lo que nos dijo en la primera
   observación, que también hay que mandarle datos al dispositivo. Desde aquí le cambio el
   límite de la sala por MQTT... el dispositivo lo aplica y vuelve a verde. Si mando 45 lo
   rechaza porque está fuera de rango."
9. **Cierre.** "Lo entrenamos con datos del simulador, entonces cuando tengamos el hardware hay
   que reentrenarlo con datos reales. Si con esos datos no le gana a la regla simple, nos
   quedamos con la regla."

## Si pregunta

| Pregunta | Respuesta |
|---|---|
| ¿Por qué no TensorFlow Lite? | Igual que en la App4: con los pesos en un `.h` el resultado es el mismo y compila rápido en Wokwi. |
| ¿Por qué un clasificador y no predecir la temperatura? | Probamos las dos. Prediciendo el valor el error es chico, pero solo anticipa el 35 % de los cruces porque tira al promedio. |
| ¿Por qué solo cuando hay gente? | Es la pregunta para la que se entrenó. Con la sala vacía nadie sufre el calor. |
| ¿Por qué parpadea y vuelve a verde un momento? | El DHT22 mide de a 0,1 °C y la probabilidad queda justo en el borde. Se arregla con histéresis. |
| ¿Por qué 2,2 segundos por minuto? | Es para acelerar la demo, y el DHT22 no se puede leer más rápido que cada 2 s. |
| ¿Y si se cae el internet? | La luz y el aviso siguen funcionando; solo deja de llegar el límite nuevo y los mensajes a la nube. |
| ¿El ruido no importa? | Casi no: bajándolo 12 dB la red avisa en el mismo minuto. |
