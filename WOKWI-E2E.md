# Wokwi E2E (rama aislada)

Esta rama prueba el mismo firmware de `TF` con una cadencia acelerada para cumplir el contrato de seis lecturas por minuto de Edge. El perfil `zenroom` de produccion no cambia. Los mensajes se aislan en `comfort/c7iot-e2e-20261105`.

## Preparacion

1. Abrir esta carpeta del repo `zenroom-embedded-app` en VS Code. En la raiz estan `wokwi.toml` y `diagram.json`.
2. Configurar Edge para HiveMQ (`broker.hivemq.com:1883`), prefijo MQTT `comfort` y `SITE_CODE=c7iot-e2e-20261105`. Crear en la base Cloud local desechable el sitio con ese mismo codigo y la sala `sala-03`, y registrar el dispositivo `zr-sala-03` antes de comenzar.
3. Instalar PlatformIO y compilar el perfil de simulacion:

   ```powershell
   pio run -e wokwi
   ```

4. Con Wokwi CLI autenticado y con cuota CI disponible:

   ```powershell
   wokwi-cli . --timeout 900000 --serial-log-file evidence-wokwi-e2e.log
   ```

5. Confirmar en el log WiFi/MQTT conectados, JSON de lecturas validos, agregado Edge con seis lotes, upload Cloud y pronostico visible en `/api/v1/rooms` y en las apps.

Los valores enviados por Wokwi son sinteticos. No publicar datos personales. El namespace de prueba esta autorizado solo para `comfort/c7iot-e2e-20261105`.

## Firmware de Arduino real

Para el hardware real, usar `TF` y compilar `pio run -e zenroom`. Ese perfil conserva la frecuencia de produccion y sus topics `comfort/site-1`; esta rama no se debe fusionar a `TF`. Las credenciales WiFi y el broker con TLS para el hardware real aun deben configurarse por ambiente.
