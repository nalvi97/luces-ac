/*
 * Luces AC — ESP PRINCIPAL
 * ─────────────────────────
 * Controla: Tira A (Techo Izq) + Tira B (Techo Dcha) + 2 interruptores físicos.
 * Expone servidor BLE (NimBLE) para la web-app (Bluefy) y reenvía por ESP-NOW
 * las órdenes de la Tira C (Ambiente) al ESP secundario.
 *
 * Placa:      ESP32 clásico o ESP32-S3 (Arduino core 3.x)
 * Librerías:  NimBLE-Arduino 2.x · WS2812FX (+ Adafruit NeoPixel)
 *
 * Cableado previsto (se confirma al flashear):
 *   GPIO16 → datos Tira A      GPIO18 → interruptor A (a GND)
 *   GPIO17 → datos Tira B      GPIO21 → interruptor B (a GND)
 */

#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WS2812FX.h>
#include <NimBLEDevice.h>

// ── Pines y valores por defecto ─────────────────────────────
#define PIN_TIRA_A 16
#define PIN_TIRA_B 17
#define PIN_SW_A   18
#define PIN_SW_B   21
#define LEDS_DEF   60          // hasta que se configure desde la app
#define ESPNOW_CANAL 1

// ── UUIDs BLE (los mismos que usa la web-app) ───────────────
static const char* UUID_SVC = "7e57c0de-a001-4f0e-9a2b-1c2d3e4f5a01";
static const char* UUID_CMD = "7e57c0de-a002-4f0e-9a2b-1c2d3e4f5a01";
static const char* UUID_EST = "7e57c0de-a003-4f0e-9a2b-1c2d3e4f5a01";
static const char* UUID_CFG = "7e57c0de-a004-4f0e-9a2b-1c2d3e4f5a01";

// ── Protocolo compartido (BLE y ESP-NOW, little-endian) ─────
#define MAGIC 0xAC1E
#define OP_SET    1
#define OP_CONFIG 2

struct __attribute__((packed)) Pkt {
  uint16_t magic;
  uint8_t  op;      // OP_SET | OP_CONFIG
  uint8_t  mask;    // bit0 = Tira A, bit1 = Tira B, bit2 = Tira C
  uint8_t  on;
  uint8_t  r, g, b;
  uint8_t  bri;     // 0-255
  uint8_t  fx;      // modo WS2812FX
  uint16_t speed;   // ms por ciclo WS2812FX (mayor = más lento)
  uint16_t count;   // OP_CONFIG: nº de LEDs
};

struct Zona { bool on; uint8_t r, g, b, bri, fx; uint16_t speed; };
Zona zonas[3];      // A, B, C — C es solo "espejo" de lo enviado al secundario

Preferences prefs;
uint16_t numLeds[3] = {LEDS_DEF, LEDS_DEF, LEDS_DEF};

WS2812FX* tiraA;
WS2812FX* tiraB;

NimBLECharacteristic* chEst = nullptr;
uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Estado → BLE (25 bytes: versión + 3 zonas × 8) ──────────
void notificarEstado() {
  uint8_t buf[25];
  buf[0] = 1;
  for (int i = 0; i < 3; i++) {
    uint8_t* p = buf + 1 + i * 8;
    p[0] = zonas[i].on; p[1] = zonas[i].r; p[2] = zonas[i].g; p[3] = zonas[i].b;
    p[4] = zonas[i].bri; p[5] = zonas[i].fx;
    p[6] = zonas[i].speed & 0xFF; p[7] = zonas[i].speed >> 8;
  }
  if (chEst) { chEst->setValue(buf, sizeof(buf)); chEst->notify(); }
}

// ── Aplicar estado de una zona local a su tira ──────────────
void aplicarLocal(int i) {
  WS2812FX* t = (i == 0) ? tiraA : tiraB;
  if (!zonas[i].on) { t->setBrightness(0); return; }
  t->setBrightness(zonas[i].bri);
  t->setColor(((uint32_t)zonas[i].r << 16) | ((uint32_t)zonas[i].g << 8) | zonas[i].b);
  t->setMode(zonas[i].fx);
  t->setSpeed(zonas[i].speed);
}

void enviarEspNow(const Pkt& p) {
  esp_now_send(bcast, (const uint8_t*)&p, sizeof(p));
}

// ── Procesar un comando (venga de BLE o de un interruptor) ──
void procesarSet(const Pkt& p) {
  for (int i = 0; i < 3; i++) {
    if (!(p.mask & (1 << i))) continue;
    zonas[i].on = p.on; zonas[i].r = p.r; zonas[i].g = p.g; zonas[i].b = p.b;
    zonas[i].bri = p.bri; zonas[i].fx = p.fx; zonas[i].speed = p.speed;
    if (i < 2) aplicarLocal(i);
  }
  if (p.mask & 0b100) enviarEspNow(p);   // la Tira C viaja por ESP-NOW
  notificarEstado();
}

// ── Callbacks BLE ───────────────────────────────────────────
class CmdCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    if (v.size() < sizeof(Pkt)) return;
    Pkt p; memcpy(&p, v.data(), sizeof(Pkt));
    if (p.magic != MAGIC) return;
    if (p.op == OP_SET) procesarSet(p);
  }
};

class CfgCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    if (v.size() < 6) return;                       // 3 × uint16 LE
    const uint8_t* d = v.data();
    uint16_t n0 = d[0] | (d[1] << 8), n1 = d[2] | (d[3] << 8), n2 = d[4] | (d[5] << 8);
    prefs.putUShort("n0", n0);
    prefs.putUShort("n1", n1);
    prefs.putUShort("n2", n2);
    Pkt p = {}; p.magic = MAGIC; p.op = OP_CONFIG; p.mask = 0b100; p.count = n2;
    enviarEspNow(p);                                // el secundario guarda y se reinicia
    delay(150);
    ESP.restart();                                  // reinicio limpio con la nueva longitud
  }
};

// ── Interruptores (2 estados: cualquier cambio = toggle) ────
int swEstado[2];
uint32_t swUltimo[2] = {0, 0};

void vigilarInterruptores() {
  const int pines[2] = {PIN_SW_A, PIN_SW_B};
  for (int i = 0; i < 2; i++) {
    int lect = digitalRead(pines[i]);
    if (lect != swEstado[i] && millis() - swUltimo[i] > 60) {
      swEstado[i] = lect; swUltimo[i] = millis();
      zonas[i].on = !zonas[i].on;
      aplicarLocal(i);
      notificarEstado();
    }
  }
}

// ── Setup ───────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  prefs.begin("lucesac");
  numLeds[0] = prefs.getUShort("n0", LEDS_DEF);
  numLeds[1] = prefs.getUShort("n1", LEDS_DEF);
  numLeds[2] = prefs.getUShort("n2", LEDS_DEF);

  // Estado inicial: blanco cálido suave (los interruptores mandan)
  for (int i = 0; i < 3; i++) zonas[i] = {true, 255, 170, 80, 150, 0, 1000};

  tiraA = new WS2812FX(numLeds[0], PIN_TIRA_A, NEO_GRB + NEO_KHZ800);
  tiraB = new WS2812FX(numLeds[1], PIN_TIRA_B, NEO_GRB + NEO_KHZ800);
  for (WS2812FX* t : {tiraA, tiraB}) { t->init(); t->start(); }
  aplicarLocal(0); aplicarLocal(1);

  pinMode(PIN_SW_A, INPUT_PULLUP);
  pinMode(PIN_SW_B, INPUT_PULLUP);
  swEstado[0] = digitalRead(PIN_SW_A);
  swEstado[1] = digitalRead(PIN_SW_B);

  // ESP-NOW (coexiste con BLE; ambos ESP en el canal 1)
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESPNOW_CANAL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() == ESP_OK) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, bcast, 6);
    peer.channel = ESPNOW_CANAL;
    esp_now_add_peer(&peer);
  } else {
    Serial.println("[ESP-NOW] error al iniciar");
  }

  // BLE
  NimBLEDevice::init("AC-Luces");
  NimBLEServer* srv = NimBLEDevice::createServer();
  srv->advertiseOnDisconnect(true);
  NimBLEService* svc = srv->createService(UUID_SVC);

  NimBLECharacteristic* chCmd = svc->createCharacteristic(
      UUID_CMD, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chCmd->setCallbacks(new CmdCB());

  chEst = svc->createCharacteristic(
      UUID_EST, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  NimBLECharacteristic* chCfg = svc->createCharacteristic(
      UUID_CFG, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  chCfg->setCallbacks(new CfgCB());
  uint8_t cfg[6] = {(uint8_t)(numLeds[0] & 0xFF), (uint8_t)(numLeds[0] >> 8),
                    (uint8_t)(numLeds[1] & 0xFF), (uint8_t)(numLeds[1] >> 8),
                    (uint8_t)(numLeds[2] & 0xFF), (uint8_t)(numLeds[2] >> 8)};
  chCfg->setValue(cfg, 6);

  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(UUID_SVC);
  adv->start();

  notificarEstado();
  Serial.println("[OK] AC-Luces listo: BLE anunciando, ESP-NOW activo");
}

void loop() {
  tiraA->service();
  tiraB->service();
  vigilarInterruptores();
}
