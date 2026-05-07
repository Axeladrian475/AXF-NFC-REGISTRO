// ============================================================================
//  AXF GymNet — ESP32 Firmware v4.0  (Solo NFC)
//  PN532 NFC via SPI
//
//  Pines:
//    PN532 NFC → SPI: SCK=18, MISO=19, MOSI=23, SS=5
//
//  Flujo REGISTRO (automático):
//    1. El frontend presiona "Leer NFC"
//    2. El backend genera un token y lo guarda en hardware_sesiones
//    3. El ESP32 hace polling automático a GET /api/hardware/siguiente/nfc
//    4. Cuando encuentra tarea pendiente, la recoge y activa el lector
//    5. Reporta cada paso al backend vía POST /api/hardware/estado
//    6. Si error  → POST /api/hardware/cancelar
//    7. Si éxito  → POST /api/hardware/evento
//
//  Flujo ACCESO (modo torniquete/puerta):
//    - Mientras no hay registro pendiente, verifica acceso por NFC
//
//  Librerías necesarias (instalar en Arduino IDE):
//    - Adafruit PN532    (by Adafruit)
//    - ArduinoJson       (by Benoit Blanchon)
//    - WiFi              (incluida en ESP32 board)
//    - HTTPClient        (incluida en ESP32 board)
// ============================================================================

#include <SPI.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ─────────────────────────────────────────────────────────────────────────────
// CONFIGURACIÓN — edita solo esta sección
// ─────────────────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "Mega_2.4G_6F7B";
const char* WIFI_PASSWORD = "7Qk93cRx";
const char* SERVER_URL    = "http://192.168.1.59:3001";
const char* API_KEY       = "axf_esp32_2025";

// Intervalo de polling para buscar tareas pendientes (ms)
const unsigned long POLL_INTERVALO_MS = 2000;

// ─────────────────────────────────────────────────────────────────────────────
// PINES NFC
// ─────────────────────────────────────────────────────────────────────────────
#define PN532_SCK   18
#define PN532_MISO  19
#define PN532_MOSI  23
#define PN532_SS     5

// ─────────────────────────────────────────────────────────────────────────────
// OBJETO NFC
// ─────────────────────────────────────────────────────────────────────────────
Adafruit_PN532 nfc(PN532_SS);

// ─────────────────────────────────────────────────────────────────────────────
// ESTADO GLOBAL
// ─────────────────────────────────────────────────────────────────────────────
bool tareaActiva = false;  // true mientras hay un registro en curso

// ─────────────────────────────────────────────────────────────────────────────
// HELPERS
// ─────────────────────────────────────────────────────────────────────────────

// Convierte el UID bytes a string con formato "AA:BB:CC:DD"
String uidToString(uint8_t* uid, uint8_t len) {
  String s = "";
  for (uint8_t i = 0; i < len; i++) {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
    if (i < len - 1) s += ":";
  }
  s.toUpperCase();
  return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP HELPERS
// ─────────────────────────────────────────────────────────────────────────────

// Reportar paso intermedio al backend → el frontend actualiza el modal
bool reportarEstado(const String& tokenSesion, const String& paso) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(String(SERVER_URL) + "/api/hardware/estado");
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<200> doc;
  doc["api_key"]      = API_KEY;
  doc["token_sesion"] = tokenSesion;
  doc["paso"]         = paso;

  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  http.end();

  Serial.printf("[HW] Estado: %s (%d)\n", paso.c_str(), code);
  return (code == 200);
}

// Reportar error → backend marca sesión como error
bool reportarError(const String& tokenSesion, const String& motivo) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(String(SERVER_URL) + "/api/hardware/cancelar");
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<200> doc;
  doc["api_key"]      = API_KEY;
  doc["token_sesion"] = tokenSesion;
  doc["motivo"]       = motivo;

  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  http.end();

  Serial.printf("[HW] Error: %s (%d)\n", motivo.c_str(), code);
  return (code == 200);
}

// Enviar lectura NFC exitosa → backend marca sesión como done
bool enviarEventoNFC(const String& uidStr, const String& token) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Sin conexión al enviar evento");
    return false;
  }

  HTTPClient http;
  http.begin(String(SERVER_URL) + "/api/hardware/evento");
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["api_key"]      = API_KEY;
  doc["tipo"]         = "nfc";
  doc["valor"]        = uidStr;
  doc["token_sesion"] = token;

  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  Serial.printf("[HTTP] Evento NFC: %d — %s\n", code, resp.c_str());
  return (code == 200);
}

// Verificar acceso NFC en modo torniquete/puerta
void verificarAccesoNFC(const String& uidStr) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(SERVER_URL) + "/api/hardware/acceso");
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["api_key"] = API_KEY;
  doc["tipo"]    = "nfc";
  doc["valor"]   = uidStr;

  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  StaticJsonDocument<256> respDoc;
  if (!deserializeJson(respDoc, resp) && code == 200) {
    Serial.printf("[ACCESO] %s — %s\n",
      (const char*)(respDoc["resultado"] | "?"),
      (const char*)(respDoc["nombre"]    | "?")
    );
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Polling: buscar si hay tarea NFC pendiente
// Devuelve true si encontró tarea; llena tokenOut
// ─────────────────────────────────────────────────────────────────────────────
bool buscarTareaNFC(String& tokenOut) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = String(SERVER_URL) + "/api/hardware/siguiente/nfc?api_key=" + String(API_KEY);
  http.begin(url);

  int code = http.GET();
  String resp = http.getString();
  http.end();

  if (code != 200) return false;

  StaticJsonDocument<200> respDoc;
  if (deserializeJson(respDoc, resp)) return false;

  bool hay = respDoc["hay"] | false;
  if (!hay) return false;

  tokenOut = String(respDoc["token"] | "");
  return tokenOut.length() > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// REGISTRO NFC
// ─────────────────────────────────────────────────────────────────────────────
void procesarRegistroNFC(const String& token) {
  Serial.println("[NFC] Iniciando registro...");
  reportarEstado(token, "acerca_tarjeta");

  uint8_t uid[7];
  uint8_t uidLen = 0;
  bool detectado = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 12000);

  if (!detectado) {
    Serial.println("[NFC] Timeout — no se detectó tarjeta");
    reportarError(token, "timeout_nfc");
    return;
  }

  reportarEstado(token, "tarjeta_detectada");
  String uidStr = uidToString(uid, uidLen);
  Serial.println("[NFC] UID: " + uidStr);

  reportarEstado(token, "enviando");
  bool ok = enviarEventoNFC(uidStr, token);

  if (ok) {
    Serial.println("[NFC] Registrado correctamente ✓");
  } else {
    Serial.println("[NFC] Error al enviar al backend");
    reportarError(token, "error_red");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// MODO ACCESO — leer NFC y verificar contra backend
// ─────────────────────────────────────────────────────────────────────────────
void modoAcceso() {
  uint8_t uid[7];
  uint8_t uidLen = 0;
  bool detectado = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 3000);

  if (detectado) {
    String uidStr = uidToString(uid, uidLen);
    Serial.println("[ACCESO] NFC: " + uidStr);
    verificarAccesoNFC(uidStr);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(600);

  Serial.println("\n================================");
  Serial.println("   AXF GymNet — ESP32 v4.0");
  Serial.println("   (Solo NFC — sin huella)");
  Serial.println("================================\n");

  // ── WiFi ──────────────────────────────────────────────────────────────────
  Serial.print("[WIFI] Conectando a " + String(WIFI_SSID));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 24) {
    delay(500);
    Serial.print(".");
    intentos++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Conectado ✓  IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] No conectado — reintentando en loop");
  }

  // ── NFC ───────────────────────────────────────────────────────────────────
  SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
  nfc.begin();
  uint32_t ver = nfc.getFirmwareVersion();
  if (!ver) {
    Serial.println("[NFC]  ERROR — PN532 no encontrado. Verifica SCK=18, MISO=19, MOSI=23, SS=5");
  } else {
    nfc.SAMConfig();
    Serial.printf("[NFC]  OK ✓  (PN5%02x firmware v%d.%d)\n",
      (ver >> 24) & 0xFF, (ver >> 16) & 0xFF, (ver >> 8) & 0xFF);
  }

  Serial.println("\n[INFO] ESP32 operando en modo automático (solo NFC).");
  Serial.println("[INFO] Los botones en la web activan el lector automáticamente.\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// LOOP — polling automático de tareas
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // ── Reconectar WiFi si se perdió ──────────────────────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Reconectando...");
    WiFi.reconnect();
    delay(3000);
    return;
  }

  // ── Si hay tarea en proceso, esperar ──────────────────────────────────────
  if (tareaActiva) {
    delay(200);
    return;
  }

  // ── Buscar tarea de registro NFC pendiente ────────────────────────────────
  String tokenNFC = "";
  if (buscarTareaNFC(tokenNFC)) {
    tareaActiva = true;
    Serial.println("[POLL] Tarea NFC encontrada: " + tokenNFC);
    procesarRegistroNFC(tokenNFC);
    tareaActiva = false;
    delay(500);
    return;
  }

  // ── Sin tareas pendientes → modo acceso pasivo ────────────────────────────
  modoAcceso();

  delay(POLL_INTERVALO_MS);
}
