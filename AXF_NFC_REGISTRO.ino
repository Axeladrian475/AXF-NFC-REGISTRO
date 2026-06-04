// ============================================================================
//  AXF GymNet — ESP32 Firmware (Solo NFC)
//
//  Este firmware maneja exclusivamente el registro y lectura de tarjetas NFC.
//  Se eliminó toda la lógica de huella dactilar para simplificar el sistema.
//
//  Flujo:
//  ─────────────────────────────────────────────────────────────────────────
//  1. El ESP32 se conecta al WiFi y al lector NFC (PN532 vía SPI).
//  2. Hace polling unificado al backend con /siguiente/cualquiera cada 400ms.
//  3. Cuando encuentra una tarea tipo "nfc", lee la tarjeta y envía el UID.
//
//  Pines:
//    PN532 NFC  → SPI: SCK=18, MISO=19, MOSI=23, SS=5
// ============================================================================

#include <SPI.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ─────────────────────────────────────────────────────────────────────────────
// CONFIGURACIÓN
// ─────────────────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "Mega_2.4G_6F7B";
const char* WIFI_PASSWORD = "7Qk93cRx";
const char* SERVER_URL    = "https://axfgymnet.com";
const char* API_KEY       = "axf_esp32_2025";

const unsigned long POLL_INTERVALO_MS = 400;
const unsigned long TIMEOUT_NFC_MS   = 12000;

// ─────────────────────────────────────────────────────────────────────────────
// PINES NFC (PN532 SPI)
// ─────────────────────────────────────────────────────────────────────────────
#define PN532_SCK   18
#define PN532_MISO  19
#define PN532_MOSI  23
#define PN532_SS     5

// ─────────────────────────────────────────────────────────────────────────────
// OBJETOS
// ─────────────────────────────────────────────────────────────────────────────
Adafruit_PN532 nfc(PN532_SS);
WiFiClientSecure secureClient;

// ─────────────────────────────────────────────────────────────────────────────
// ESTADO GLOBAL
// ─────────────────────────────────────────────────────────────────────────────
bool     tareaActiva = false;
unsigned long ultimoPoll = 0;

// ─────────────────────────────────────────────────────────────────────────────
// HELPERS — NFC
// ─────────────────────────────────────────────────────────────────────────────
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
// HTTP HELPERS (HTTPS con WiFiClientSecure)
// ─────────────────────────────────────────────────────────────────────────────
bool httpPost(const String& path, const String& body, String* respOut = nullptr) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(secureClient, String(SERVER_URL) + path);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  http.setTimeout(8000);
  int code = http.POST(body);
  String resp = http.getString();
  Serial.printf("[HTTP] POST %s → %d | %s\n", path.c_str(), code, resp.c_str());
  if (respOut) *respOut = resp;
  http.end();
  return (code == 200);
}

bool httpGet(const String& path, String& respOut) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(secureClient, String(SERVER_URL) + path);
  http.addHeader("Connection", "close");
  http.setTimeout(8000);
  int code = http.GET();
  respOut = http.getString();
  if (code != 200) Serial.printf("[HTTP] GET %s → %d | %s\n", path.c_str(), code, respOut.c_str());
  http.end();
  return (code == 200);
}

// ─────────────────────────────────────────────────────────────────────────────
// REPORTES AL BACKEND
// ─────────────────────────────────────────────────────────────────────────────
bool reportarEstado(const String& token, const String& paso) {
  StaticJsonDocument<200> doc;
  doc["api_key"]      = API_KEY;
  doc["token_sesion"] = token;
  doc["paso"]         = paso;
  String body;
  serializeJson(doc, body);
  return httpPost("/api/hardware/estado", body);
}

bool reportarError(const String& token, const String& motivo) {
  StaticJsonDocument<200> doc;
  doc["api_key"]      = API_KEY;
  doc["token_sesion"] = token;
  doc["motivo"]       = motivo;
  String body;
  serializeJson(doc, body);
  Serial.printf("[ERROR] %s\n", motivo.c_str());
  return httpPost("/api/hardware/cancelar", body);
}

bool enviarEvento(const String& tipo, const String& valor, const String& token) {
  StaticJsonDocument<256> doc;
  doc["api_key"]      = API_KEY;
  doc["tipo"]         = tipo;
  doc["valor"]        = valor;
  doc["token_sesion"] = token;
  String body;
  serializeJson(doc, body);
  Serial.printf("[EVENTO] tipo=%s valor=%s token=%s\n", tipo.c_str(), valor.c_str(), token.c_str());
  return httpPost("/api/hardware/evento", body);
}

// ─────────────────────────────────────────────────────────────────────────────
// POLLING UNIFICADO — detecta tareas NFC pendientes en un solo request
// ─────────────────────────────────────────────────────────────────────────────
bool buscarTareaPendiente(String& tokenOut, String& tipoOut) {
  String resp;
  String path = "/api/hardware/siguiente/cualquiera?api_key=" + String(API_KEY);
  if (!httpGet(path, resp)) return false;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, resp)) return false;
  if (!(doc["hay"] | false)) return false;

  tokenOut = String(doc["token"] | "");
  tipoOut  = String(doc["tipo"]  | "");
  return (tokenOut.length() > 0 && tipoOut.length() > 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// REGISTRO NFC — lee la tarjeta y envía el UID al backend
// ─────────────────────────────────────────────────────────────────────────────
void procesarRegistroNFC(const String& token) {
  Serial.println("\n[NFC] Iniciando registro...");
  reportarEstado(token, "acerca_tarjeta");

  uint8_t uid[7];
  uint8_t uidLen = 0;
  bool detectado = nfc.readPassiveTargetID(
    PN532_MIFARE_ISO14443A, uid, &uidLen, (uint16_t)TIMEOUT_NFC_MS
  );

  if (!detectado) {
    Serial.println("[NFC] Timeout");
    reportarError(token, "timeout_nfc");
    return;
  }

  reportarEstado(token, "tarjeta_detectada");
  String uidStr = uidToString(uid, uidLen);
  Serial.println("[NFC] UID: " + uidStr);

  reportarEstado(token, "enviando");
  if (enviarEvento("nfc", uidStr, token)) {
    Serial.println("[NFC] ✓ Registrado");
  } else {
    reportarError(token, "error_red");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// WIFI
// ─────────────────────────────────────────────────────────────────────────────
void reconectarWifi() {
  Serial.println("[WIFI] Reconectando...");
  WiFi.reconnect();
  int n = 0;
  while (WiFi.status() != WL_CONNECTED && n < 20) { delay(500); Serial.print("."); n++; }
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("\n[WIFI] ✓ IP: " + WiFi.localIP().toString());
  else {
    Serial.println("\n[WIFI] Fallo, reintento en 3s");
    delay(3000);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n================================");
  Serial.println("   AXF GymNet — ESP32 Solo NFC");
  Serial.println("   (Registro de tarjetas NFC)");
  Serial.println("================================\n");

  // WiFi
  Serial.print("[WIFI] Conectando a " + String(WIFI_SSID));
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int n = 0;
  while (WiFi.status() != WL_CONNECTED && n < 24) { delay(500); Serial.print("."); n++; }
  Serial.println(WiFi.status() == WL_CONNECTED
    ? "\n[WIFI] ✓ IP: " + WiFi.localIP().toString()
    : "\n[WIFI] Sin conexión — reintento automático");

  // Configurar cliente HTTPS (sin verificación de certificado)
  secureClient.setInsecure();

  // NFC
  SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
  nfc.begin();
  uint32_t ver = nfc.getFirmwareVersion();
  if (!ver) {
    Serial.println("[NFC]  ERROR — PN532 no encontrado");
  } else {
    nfc.SAMConfig();
    Serial.printf("[NFC]  ✓ PN5%02x v%d.%d\n",
      (ver >> 24) & 0xFF, (ver >> 16) & 0xFF, (ver >> 8) & 0xFF);
  }

  Serial.println("\n[INFO] Listo — esperando tareas NFC...\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// LOOP — polling unificado, solo NFC
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  if (WiFi.status() != WL_CONNECTED) { reconectarWifi(); return; }
  if (tareaActiva) { delay(100); return; }

  unsigned long ahora = millis();
  if (ahora - ultimoPoll < POLL_INTERVALO_MS) { delay(10); return; }
  ultimoPoll = ahora;

  String tokenTarea = "", tipoTarea = "";
  if (!buscarTareaPendiente(tokenTarea, tipoTarea)) return;

  tareaActiva = true;
  Serial.printf("\n[POLL] Tarea: %s | Token: %s\n", tipoTarea.c_str(), tokenTarea.c_str());

  if (tipoTarea == "nfc") {
    procesarRegistroNFC(tokenTarea);
  } else {
    Serial.println("[POLL] Tipo no soportado: " + tipoTarea);
    reportarError(tokenTarea, "tipo_no_soportado");
  }

  tareaActiva = false;
  delay(200);
}
