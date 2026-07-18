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

## Publicar la web-app (una vez)

1. Crear repo `luces-ac` en GitHub y subir este proyecto:
   ```
   git remote add origin https://github.com/TU_USUARIO/luces-ac.git
   git push -u origin main
   ```
2. En GitHub: **Settings → Pages → Source: Deploy from a branch → main → /docs**.
3. La app queda en `https://TU_USUARIO.github.io/luces-ac/` → abrirla **en Bluefy** y guardarla en favoritos.

## Flashear (sesión con USB)

1. Conectar el ESP por USB (cable de datos) y detectar placa y puerto:
   `arduino-cli board list` (o `esptool chip_id` para identificar el chip exacto).
2. Instalar core y librerías (una vez):
   ```
   arduino-cli core install esp32:esp32
   arduino-cli lib install "NimBLE-Arduino" "WS2812FX" "Adafruit NeoPixel"
   ```
3. Compilar y subir (FQBN según chip: `esp32:esp32:esp32` o `esp32:esp32:esp32s3`):
   ```
   arduino-cli compile -b esp32:esp32:esp32 firmware/principal
   arduino-cli upload  -b esp32:esp32:esp32 -p COMX firmware/principal
   ```
4. Igual con `firmware/secundario` en el otro ESP.

Volver a WLED siempre es posible reflasheando desde https://install.wled.me.

## Estado del proyecto

- [x] Firmware principal y secundario escritos (pendiente compilar juntos en la sesión de flasheo)
- [x] Web-app con Web Bluetooth real
- [ ] Publicar en GitHub Pages
- [ ] Flashear y probar en vivo
- [ ] Contar LEDs reales y fijarlos desde Ajustes
