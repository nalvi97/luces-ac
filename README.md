# Luces AC 🚐

Control por Bluetooth (BLE) de 3 tiras WS2812B en la autocaravana, desde iPhone con [Bluefy](https://apps.apple.com/us/app/bluefy-web-ble-browser/id1492822055). Sin WiFi, sin router, sin IPs.

## Arquitectura

```
iPhone (Bluefy) ──BLE──▶ ESP principal ──ESP-NOW──▶ ESP secundario
                          ├─ Tira A (Techo Izq)      └─ Tira C (Ambiente)
                          ├─ Tira B (Techo Dcha)
                          └─ 2 interruptores físicos (toggle on/off por tira)
```

- **`firmware/principal/`** — BLE (NimBLE) + 2 tiras + interruptores + puente ESP-NOW
- **`firmware/secundario/`** — receptor ESP-NOW + 1 tira
- **`docs/index.html`** — web-app del mando (Web Bluetooth), se sirve en GitHub Pages

## Cableado

| Función | ESP principal | ESP secundario |
|---|---|---|
| Datos Tira A | GPIO 16 | — |
| Datos Tira B | GPIO 17 | — |
| Datos Tira C | — | GPIO 16 |
| Interruptor A | GPIO 18 ↔ GND | — |
| Interruptor B | GPIO 21 ↔ GND | — |

Los interruptores son de 2 posiciones: **cualquier cambio de posición = encender/apagar su tira**.

## Protocolo (BLE y ESP-NOW comparten struct)

Servicio BLE `7e57c0de-a001-…5a01`, dispositivo `AC-Luces`:

| Característica | UUID (…a00X…) | Uso |
|---|---|---|
| CMD | a002 | write: paquete de 14 bytes (magic `0xAC1E`, op, mask, on, rgb, bri, fx, speed, count) |
| ESTADO | a003 | read/notify: 1 + 3×8 bytes (estado de las 3 zonas) |
| CONFIG | a004 | read/write: 3×uint16 LE = nº LEDs por tira (al escribir, el ESP guarda en NVS y se reinicia) |

Efectos = ids de modo de [WS2812FX](https://github.com/kitesurfer1404/WS2812FX). Escenas y presets viven en la web-app (cambiarlas no requiere reflashear).

## Panel Solar (Victron SmartSolar MPPT)

Desde fw8, el ESP secundario escucha los anuncios BLE *Instant Readout* del Victron (AES-CTR), los descifra y los reenvía por ESP-NOW; la app los muestra en la pestaña **Solar** (potencia, batería, corriente, kWh del día, estado, salida de cargas).

Configuración: en VictronConnect → producto → Info del producto → activar *Instant readout* y copiar la **clave de cifrado** (32 hex) → pegarla en la app en **Ajustes → Clave del Victron** → Guardar (ambos ESP se reinician). Clave vacía = función desactivada. Protocolo verificado contra [victron-ble](https://github.com/keshavdv/victron-ble).

## Publicar la web-app (una vez)

1. Crear repo `luces-ac` en GitHub y subir este proyecto:
   ```
   git remote add origin https://github.com/TU_USUARIO/luces-ac.git
   git push -u origin main
   ```
2. En GitHub: **Settings → Pages → Source: Deploy from a branch → main → /docs**.
3. La app queda en `https://TU_USUARIO.github.io/luces-ac/` → abrirla **en Bluefy** y guardarla en favoritos.

## Flashear

Ambos firmwares están **compilados y verificados** (core esp32 3.3.10, NimBLE-Arduino 2.5.0, WS2812FX 1.4.7, Adafruit NeoPixel 1.15.5). Hay dos rutas; la A no necesita Arduino.

### Ruta A — Navegador (recomendada, sin instalar nada)

1. En Chrome o Edge, abrir **https://espressif.github.io/esptool-js/**
2. Conectar el ESP por USB (cable de datos) → botón **Connect** → elegir el puerto.
   *Si no aparece ningún puerto: falta el driver CH340 o CP210x (habitual en placas clónicas).*
3. La consola dice qué chip es (p. ej. `Chip is ESP32-S3`). Elegir el `.bin` de `binarios/` que corresponda:
   - `principal-esp32.bin` / `principal-esp32s3.bin` → el ESP de los interruptores (2 tiras)
   - `secundario-esp32.bin` / `secundario-esp32s3.bin` → el ESP de la tira Ambiente
4. **Flash Address: `0x0`** → Program → esperar a "Done" → botón Reset de la placa.
5. **Etiquetar la placa** (principal / secundario) y repetir con la otra.

### Ruta B — Arduino IDE

1. Boards Manager → **esp32 by Espressif Systems 3.x** (con 2.x NO compila el secundario).
2. Library Manager → **NimBLE-Arduino** y **WS2812FX**.
3. Abrir el `.ino` → placa "ESP32 Dev Module" o "ESP32S3 Dev Module" según chip → Subir.
   (En S3, activar *USB CDC On Boot* para ver el monitor serie.)

### Actualizaciones posteriores: OTA por WiFi (sin cables)

Desde fw7, cada ESP puede actualizarse sin desmontarlo:

1. En la app: **Ajustes → "OTA principal"** (o **"OTA ambiente"**). Las tiras de ese ESP se ponen en azul tenue.
2. Conectarse a la WiFi que abre el ESP: **`LucesAC-OTA`** (o `LucesAC-OTA-AMB`), clave **`luces-ac`**.
3. Abrir **http://192.168.4.1** y subir **`binarios/ota-principal.bin`** (o `ota-secundario.bin`).
4. El ESP se reinicia solo con el firmware nuevo. Si no se sube nada en 5 min, vuelve a modo normal.

Ojo a la diferencia entre binarios:
- `principal-esp32.bin` / `secundario-esp32.bin` → imagen completa, **solo para flasheo por cable** a dirección `0x0`. Borra la configuración guardada (nº LEDs, orden de color).
- `ota-principal.bin` / `ota-secundario.bin` → **solo para OTA** por la página web. Conserva la configuración.

La compilación usa particiones `min_spiffs` (dos slots OTA de 1,9 MB):
`arduino-cli compile -b esp32:esp32:esp32:PartitionScheme=min_spiffs ...`
El salto a fw7 desde versiones anteriores debe hacerse **por cable** (cambia la tabla de particiones); a partir de ahí, siempre OTA.

### Comprobación tras flashear el principal

Sin conectar tiras aún: en Bluefy abrir `https://nalvi97.github.io/luces-ac/` → Conectar → debe aparecer **AC-Luces**. Si conecta, todo el camino BLE funciona.

Volver a WLED siempre es posible reflasheando desde https://install.wled.me.

## Estado del proyecto

- [x] Firmware principal y secundario escritos (pendiente compilar juntos en la sesión de flasheo)
- [x] Web-app con Web Bluetooth real
- [ ] Publicar en GitHub Pages
- [ ] Flashear y probar en vivo
- [ ] Contar LEDs reales y fijarlos desde Ajustes
