/*
 * Luces AC — ESP SECUNDARIO
 * ──────────────────────────
 * Controla la Tira C (Ambiente). No tiene BLE ni botones: solo escucha
 * por ESP-NOW las órdenes que reenvía el ESP principal.
 *
 * Placa:      ESP32 clásico o ESP32-S3 (Arduino core 3.x)
 * Librerías:  WS2812FX (+ Adafruit NeoPixel)
 *
 * Cableado previsto: GPIO16 → datos Tira C
 */

#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WS2812FX.h>

#define PIN_TIRA   16
#define LEDS_DEF   60
#define ESPNOW_CANAL 1

#define MAGIC 0xAC1E
#define OP_SET    1
#define OP_CONFIG 2

struct __attribute__((packed)) Pkt {
  uint16_t magic;
  uint8_t  op;
  uint8_t  mask;    // bit2 = Tira C (esta)
  uint8_t  on;
  uint8_t  r, g, b;
  uint8_t  bri;
  uint8_t  fx;
  uint16_t speed;
  uint16_t count;
  uint8_t  orden;   // 0=GRB 1=RGB 2=BGR 3=BRG 4=GBR 5=RBG
};

neoPixelType ordenTipo(uint8_t o) {
  switch (o) {
    case 1:  return NEO_RGB + NEO_KHZ800;
    case 2:  return NEO_BGR + NEO_KHZ800;
    case 3:  return NEO_BRG + NEO_KHZ800;
    case 4:  return NEO_GBR + NEO_KHZ800;
    case 5:  return NEO_RBG + NEO_KHZ800;
    default: return NEO_GRB + NEO_KHZ800;
  }
}

Preferences prefs;
WS2812FX* tira;

// El callback de ESP-NOW corre en el hilo WiFi: guardamos y aplicamos en loop()
volatile bool pendiente = false;
Pkt ultimo;

void alRecibir(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < (int)sizeof(Pkt)) return;
  Pkt p; memcpy(&p, data, sizeof(Pkt));
  if (p.magic != MAGIC || !(p.mask & 0b100)) return;
  ultimo = p;
  pendiente = true;
}

void aplicar(const Pkt& p) {
  if (p.op == OP_CONFIG) {
    prefs.putUShort("n2", p.count);
    prefs.putUChar("o2", p.orden);
    delay(100);
    ESP.restart();
    return;
  }
  // Quirk WS2812FX: brillo 0 = "sin escalado" (=máximo). Apagar = stop().
  if (!p.on) { tira->stop(); return; }
  if (!tira->isRunning()) tira->start();
  tira->setBrightness(p.bri > 0 ? p.bri : 1);
  tira->setColor(((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | p.b);
  tira->setMode(p.fx);
  tira->setSpeed(p.speed);
}

void setup() {
  Serial.begin(115200);

  prefs.begin("lucesac");
  uint16_t n = prefs.getUShort("n2", LEDS_DEF);
  uint8_t  o = prefs.getUChar("o2", 0);

  tira = new WS2812FX(n, PIN_TIRA, ordenTipo(o));
  tira->init();
  tira->start();
  // Arranque: blanco cálido suave hasta que llegue una orden
  tira->setBrightness(150);
  tira->setColor(0xFFAA50);
  tira->setMode(FX_MODE_STATIC);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESPNOW_CANAL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(alRecibir);
    Serial.println("[OK] Secundario escuchando ESP-NOW");
  } else {
    Serial.println("[ESP-NOW] error al iniciar");
  }
}

void loop() {
  tira->service();
  if (pendiente) {
    pendiente = false;
    aplicar(ultimo);
  }
}
