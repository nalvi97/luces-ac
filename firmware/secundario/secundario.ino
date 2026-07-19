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
#include <WebServer.h>
#include <Update.h>
#include <NimBLEDevice.h>
#include "mbedtls/aes.h"

#define PIN_TIRA   16
#define LEDS_DEF   60
#define ESPNOW_CANAL 1

#define MAGIC 0xAC1E
#define OP_SET    1
#define OP_CONFIG 2
#define OP_HB     4   // latido: "estoy vivo", lo escucha el principal
#define OP_OTA    5   // entra en modo actualización por WiFi
#define OP_VIC    6   // datos del Victron hacia el principal
#define FW_VER    9   // viaja en el campo count del latido

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
  uint8_t  orden;      // 0=GRB 1=RGB 2=BGR 3=BRG 4=GBR 5=RBG
  uint8_t  clave[16];  // OP_CONFIG: clave AES del Victron (todo ceros = desactivado)
};

// Datos del Victron rumbo al principal (Instant Readout descifrado)
struct __attribute__((packed)) PktVic {
  uint16_t magic;
  uint8_t  op;       // OP_VIC
  uint8_t  estado;   // device state Victron; 0xFE = clave incorrecta
  uint8_t  error;
  int16_t  vbat;     // 0.01 V
  int16_t  ibat;     // 0.1 A
  uint16_t wsolar;   // W
  uint16_t whdia;    // unidades de 10 Wh
  uint16_t iload;    // 0.1 A; 0x1FF = no disponible
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
uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool encendida = true;   // estado lógico de la tira (para el barrido antifantasma)
volatile bool otaPedido = false;

// ── Victron Instant Readout ─────────────────────────────────
uint8_t claveVic[16] = {0};
bool vicScanOn = false;
PktVic vicDatos = {MAGIC, OP_VIC, 0xFF, 0xFF, 0, 0, 0, 0, 0x1FF};
volatile bool vicNuevo = false, vicClaveMal = false;

// Anuncio Victron (datos de fabricante): [0-1] empresa 0x02E1 · [2-3] prefijo
// [4-5] modelo · [6] tipo de registro (0x01 = cargador solar) · [7-8] IV LE
// [9] byte de comprobación (= clave[0], sin cifrar) · [10..] payload AES-CTR
class VicScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    std::string md = dev->getManufacturerData();
    if (md.size() < 21) return;
    const uint8_t* d = (const uint8_t*)md.data();
    if (d[0] != 0xE1 || d[1] != 0x02) return;    // Victron Energy BV
    if (d[6] != 0x01) return;                    // solo registros de cargador solar
    if (d[9] != claveVic[0]) { vicClaveMal = true; return; }

    uint8_t ctr[16] = {d[7], d[8]};              // IV little-endian, resto 0
    uint8_t sblk[16]; size_t off = 0;
    uint8_t plano[16] = {0};
    size_t nc = md.size() - 10; if (nc > 16) nc = 16;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, claveVic, 128);
    mbedtls_aes_crypt_ctr(&aes, nc, &off, ctr, sblk, d + 10, plano);
    mbedtls_aes_free(&aes);

    // Orden de campos verificado contra victron-ble (solar_charger.py)
    vicDatos.estado = plano[0];
    vicDatos.error  = plano[1];
    vicDatos.vbat   = (int16_t)(plano[2] | (plano[3] << 8));
    vicDatos.ibat   = (int16_t)(plano[4] | (plano[5] << 8));
    vicDatos.whdia  = plano[6] | (plano[7] << 8);
    vicDatos.wsolar = plano[8] | (plano[9] << 8);
    vicDatos.iload  = plano[10] | ((plano[11] & 0x01) << 8);
    vicNuevo = true;
  }
};
VicScanCB vicCB;

// El callback de ESP-NOW corre en el hilo WiFi: guardamos y aplicamos en loop()
volatile bool pendiente = false;
Pkt ultimo;

void alRecibir(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < (int)sizeof(Pkt)) return;
  Pkt p; memcpy(&p, data, sizeof(Pkt));
  if (p.magic != MAGIC || !(p.mask & 0b100)) return;
  if (p.op == OP_HB) return;   // ignora latidos de otros
  ultimo = p;
  pendiente = true;
}

void aplicar(const Pkt& p) {
  if (p.op == OP_OTA) { otaPedido = true; return; }
  if (p.op == OP_CONFIG) {
    Serial.printf("[RX] CONFIG: %u LEDs, orden %u -> guardo y reinicio\n", p.count, p.orden);
    prefs.putUShort("n2", p.count);
    prefs.putUChar("o2", p.orden);
    prefs.putBytes("vk", p.clave, 16);
    delay(100);
    ESP.restart();
    return;
  }
  Serial.printf("[RX] SET: on=%u rgb=(%u,%u,%u) bri=%u fx=%u speed=%u\n",
                p.on, p.r, p.g, p.b, p.bri, p.fx, p.speed);
  // Quirk WS2812FX: brillo 0 = "sin escalado" (=máximo). Apagar = stop().
  encendida = p.on;
  if (!p.on) { tira->stop(); return; }
  if (!tira->isRunning()) tira->start();
  tira->setBrightness(p.bri > 0 ? p.bri : 1);
  tira->setColor(((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | p.b);
  tira->setMode(p.fx);
  tira->setSpeed(p.speed);
}

// ── Modo actualización OTA (idéntico al del principal, WiFi propia) ──
WebServer* otaSrv = nullptr;
uint32_t otaUltimo = 0;

void entrarModoOTA() {
  Serial.println("[OTA] WiFi 'LucesAC-OTA-AMB' (clave luces-ac) -> http://192.168.4.1");
  if (vicScanOn) NimBLEDevice::deinit(true);   // radio limpia para el modo OTA
  esp_now_deinit();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("LucesAC-OTA-AMB", "luces-ac");

  if (!tira->isRunning()) tira->start();
  tira->setBrightness(40); tira->setColor(0x2040FF); tira->setMode(FX_MODE_STATIC);

  otaSrv = new WebServer(80);
  otaSrv->on("/", HTTP_GET, []() {
    otaUltimo = millis();
    otaSrv->send(200, "text/html",
      "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Luces AC OTA</title>"
      "<body style='font-family:sans-serif;background:#14110c;color:#f2e9dc;text-align:center;padding-top:12vh'>"
      "<h2>Luces AC &middot; ambiente</h2><p>Sube <b>ota-secundario.bin</b></p>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='fw' accept='.bin'><br><br>"
      "<input type='submit' value='Actualizar' style='padding:10px 24px'></form>");
  });
  otaSrv->on("/update", HTTP_POST, []() {
    bool ok = !Update.hasError();
    otaSrv->send(200, "text/html", ok ? "<h2>Actualizado. Reiniciando&hellip;</h2>" : "<h2>Error al actualizar</h2>");
    delay(800);
    ESP.restart();
  }, []() {
    otaUltimo = millis();
    HTTPUpload& up = otaSrv->upload();
    if (up.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
    else if (up.status == UPLOAD_FILE_WRITE) Update.write(up.buf, up.currentSize);
    else if (up.status == UPLOAD_FILE_END) Update.end(true);
  });
  otaSrv->begin();
  otaUltimo = millis();

  while (true) {
    otaSrv->handleClient();
    tira->service();
    if (millis() - otaUltimo > 300000UL) ESP.restart();
    delay(2);
  }
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
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, bcast, 6);
    peer.channel = ESPNOW_CANAL;
    esp_now_add_peer(&peer);
    Serial.printf("[OK] Secundario escuchando ESP-NOW · MAC %s · canal %d · %u LEDs · orden %u\n",
                  WiFi.macAddress().c_str(), ESPNOW_CANAL, n, o);
  } else {
    Serial.println("[ESP-NOW] error al iniciar");
  }

  // Escaneo BLE del Victron, solo si hay clave configurada
  prefs.getBytes("vk", claveVic, 16);
  for (int i = 0; i < 16; i++) if (claveVic[i]) { vicScanOn = true; break; }
  if (vicScanOn) {
    NimBLEDevice::init("");
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&vicCB, false);
    scan->setActiveScan(false);             // pasivo: el dato viaja en el propio anuncio
    scan->setMaxResults(0);                 // solo callbacks, sin almacenar
    scan->setDuplicateFilter(false);        // los anuncios cambian: queremos todos
    // Ciclo de trabajo reducido: la radio se comparte con ESP-NOW y un escaneo
    // continuo se come los comandos de la tira (60 ms de escucha cada 300 ms).
    scan->setInterval(300);
    scan->setWindow(60);
    scan->start(0, false);                  // escaneo indefinido
    Serial.println("[VIC] escaneando anuncios del Victron");
  }
}

void loop() {
  if (otaPedido) { otaPedido = false; entrarModoOTA(); }

  tira->service();
  if (pendiente) {
    pendiente = false;
    aplicar(ultimo);
  }

  // Latido cada 3 s: el principal lo vigila y la app muestra "Ambiente ✓ / sin señal"
  static uint32_t tHB = 0;
  if (millis() - tHB > 3000) {
    tHB = millis();
    Pkt p = {}; p.magic = MAGIC; p.op = OP_HB; p.mask = 0b100;
    p.count = FW_VER;   // la versión viaja en el latido
    esp_now_send(bcast, (const uint8_t*)&p, sizeof(p));
  }

  // Barrido antifantasma: apagada, reenvía el negro cada segundo para borrar
  // LEDs "encendidos" por ruido en la línea de datos.
  static uint32_t tNegro = 0;
  if (millis() - tNegro > 1000) {
    tNegro = millis();
    if (!encendida) tira->strip_off();
  }

  // Datos del Victron al principal, como mucho cada 2 s
  static uint32_t tVic = 0;
  if (millis() - tVic > 2000 && (vicNuevo || vicClaveMal)) {
    tVic = millis();
    PktVic v = vicDatos;
    v.magic = MAGIC; v.op = OP_VIC;
    if (vicClaveMal && !vicNuevo) { v.estado = 0xFE; v.error = 0xFE; }
    vicNuevo = false; vicClaveMal = false;
    esp_now_send(bcast, (const uint8_t*)&v, sizeof(v));
  }
}
