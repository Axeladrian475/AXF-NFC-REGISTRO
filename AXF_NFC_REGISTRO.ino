// ============================================================================
//  AXF GymNet — ESP32 Firmware v3.0
//  Sensor Huella R307/R503  +  PN532 NFC
//
//  Pines:
//    PN532 NFC  → SPI: SCK=18, MISO=19, MOSI=23, SS=5
//    Huella     → UART2: RX=16, TX=17
//
//  Flujo REGISTRO (AUTOMÁTICO — sin escribir nada en Serial Monitor):
//    1. El frontend presiona "Leer NFC" o "Escanear Huella"
//    2. El backend genera un token y lo guarda en hardware_sesiones
//    3. El ESP32 hace polling automático a GET /api/hardware/siguiente/:tipo
//    4. Cuando encuentra una tarea pendiente, la recoge y activa el sensor
//    5. El ESP32 reporta cada paso al backend vía POST /api/hardware/estado
//    6. Si hay error → POST /api/hardware/cancelar (no ocupa espacio en sensor)
//    7. Si éxito  → POST /api/hardware/evento  (guarda el valor leído)
//
//  Flujo ACCESO (modo torniquete/puerta):
//    - El ESP32 hace polling de acceso en segundo plano mientras no hay registro
//
//  Librerías necesarias (instalar en Arduino IDE):
//    - Adafruit PN532          (by Adafruit)
//    - ArduinoJson             (by Benoit Blanchon)
//    - WiFi                    (incluida en ESP32 board)
//    - HTTPClient              (incluida en ESP32 board)
// ============================================================================

#include <SPI.h>
#include <Adafruit_PN532.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ─────────────────────────────────────────────────────────────────────────────
// CONFIGURACIÓN — edita solo esta sección
// ─────────────────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "Mega_2.4G_6F7B";
const char* WIFI_PASSWORD = "7Qk93cRx";
const char* SERVER_URL    = "http://192.168.1.37:3001";
const char* API_KEY       = "axf_esp32_2025";

// Intervalo de polling para buscar tareas pendientes (ms)
const unsigned long POLL_INTERVALO_MS = 2000;

// ─────────────────────────────────────────────────────────────────────────────
// PINES
// ─────────────────────────────────────────────────────────────────────────────
#define PN532_SCK   18
#define PN532_MISO  19
#define PN532_MOSI  23
#define PN532_SS     5
#define HUELLA_RX   16
#define HUELLA_TX   17

// ─────────────────────────────────────────────────────────────────────────────
// OBJETOS
// ─────────────────────────────────────────────────────────────────────────────
Adafruit_PN532  nfc(PN532_SS);
HardwareSerial  huellaSerial(2);

// ─────────────────────────────────────────────────────────────────────────────
// ESTADO GLOBAL
// ─────────────────────────────────────────────────────────────────────────────
uint8_t  posicionHuella = 1;      // Se incrementa en cada registro exitoso
bool     tareaActiva    = false;  // true mientras hay un registro en curso

// ─────────────────────────────────────────────────────────────────────────────
// COMANDOS SENSOR HUELLA (protocolo R307/R503)
// ─────────────────────────────────────────────────────────────────────────────
const uint8_t CMD_GET_IMAGE[] = { 0xEF,0x01,0xFF,0xFF,0xFF,0xFF,0x01,0x00,0x03,0x01,0x00,0x05 };
const uint8_t CMD_IMG2TZ_1[]  = { 0xEF,0x01,0xFF,0xFF,0xFF,0xFF,0x01,0x00,0x04,0x02,0x01,0x00,0x08 };
const uint8_t CMD_IMG2TZ_2[]  = { 0xEF,0x01,0xFF,0xFF,0xFF,0xFF,0x01,0x00,0x04,0x02,0x02,0x00,0x09 };
const uint8_t CMD_CREATE_MODEL[] = { 0xEF,0x01,0xFF,0xFF,0xFF,0xFF,0x01,0x00,0x03,0x05,0x00,0x09 };
const uint8_t CMD_SEARCH[]    = { 0xEF,0x01,0xFF,0xFF,0xFF,0xFF,0x01,0x00,0x08,0x04,0x01,0x00,0x00,0x00,0x0A,0x00,0x18 };

// ─────────────────────────────────────────────────────────────────────────────
// HELPERS — sensor huella
// ─────────────────────────────────────────────────────────────────────────────
uint8_t enviarComando(const uint8_t* cmd, uint8_t len, uint8_t* resp, int espera = 2000) {
  while (huellaSerial.available()) huellaSerial.read();
  huellaSerial.write(cmd, len);
  unsigned long t = millis();
  int i = 0;
  while (millis() - t < espera) {
    if (huellaSerial.available()) {
      resp[i++] = huellaSerial.read();
      if (i >= 16) break;
    }
  }
  return (i >= 10) ? resp[9] : 0xFF;
}

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
bool reportarEstado(String tokenSesion, String paso) {
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
  Serial.println("[HW] Estado reportado: " + paso + " (" + String(code) + ")");
  return (code == 200);
}

// Reportar error → backend marca la sesión como error, no ocupa espacio de sensor
bool reportarError(String tokenSesion, String motivo) {
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
  Serial.println("[HW] Error reportado: " + motivo + " (" + String(code) + ")");
  return (code == 200);
}

// Enviar lectura exitosa → backend marca la sesión como done
bool enviarEventoRegistro(String tipo, String valor, String token) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Sin conexión");
    return false;
  }

  HTTPClient http;
  http.begin(String(SERVER_URL) + "/api/hardware/evento");
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["api_key"]      = API_KEY;
  doc["tipo"]         = tipo;
  doc["valor"]        = valor;
  doc["token_sesion"] = token;

  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  Serial.println("[HTTP] Evento: " + String(code) + " — " + resp);
  return (code == 200);
}

// Verificar acceso (modo puerta/torniquete)
void verificarAcceso(String tipo, String valor) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(SERVER_URL) + "/api/hardware/acceso");
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["api_key"] = API_KEY;
  doc["tipo"]    = tipo;
  doc["valor"]   = valor;

  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  StaticJsonDocument<256> respDoc;
  if (!deserializeJson(respDoc, resp) && code == 200) {
    Serial.println("[ACCESO] " + String(respDoc["resultado"] | "?") + " — " + String(respDoc["nombre"] | "?"));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Polling: buscar si hay tarea pendiente de tipo nfc o huella
// Devuelve true si encontró tarea; llena tokenOut y tipoOut
// ─────────────────────────────────────────────────────────────────────────────
bool buscarTareaPendiente(String tipo, String& tokenOut) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  // La API key va como header X-Api-Key para GET (no tiene body)
  String url = String(SERVER_URL) + "/api/hardware/siguiente/" + tipo;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  // Para GET usamos un header con la api_key
  http.addHeader("X-Api-Key", API_KEY);

  // NOTA: el endpoint /siguiente/:tipo usa verificarApiKey en body,
  // así que hacemos un pequeño workaround con POST vacío conteniendo api_key
  http.end();

  // Re-hacer como POST con body mínimo para pasar el middleware
  HTTPClient http2;
  http2.begin(url);
  http2.addHeader("Content-Type", "application/json");

  StaticJsonDocument<64> doc;
  doc["api_key"] = API_KEY;
  String body;
  serializeJson(doc, body);

  int code = http2.GET();
  // El GET de /siguiente no tiene body; el middleware usa req.body.api_key
  // Cambiamos a POST para poder enviar api_key
  http2.end();

  // Solución definitiva: hacer la llamada como GET con query param
  HTTPClient http3;
  String urlConKey = String(SERVER_URL) + "/api/hardware/siguiente/" + tipo + "?api_key=" + String(API_KEY);
  http3.begin(urlConKey);
  code = http3.GET();
  String resp = http3.getString();
  http3.end();

  if (code != 200) return false;

  StaticJsonDocument<200> respDoc;
  if (deserializeJson(respDoc, resp)) return false;

  bool hay = respDoc["hay"] | false;
  if (!hay) return false;

  tokenOut = String(respDoc["token"] | "");
  return tokenOut.length() > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// REGISTRO NFC (automático con reporte de pasos)
// ─────────────────────────────────────────────────────────────────────────────
void procesarRegistroNFC(String token) {
  Serial.println("[NFC] Iniciando registro automático...");
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
  bool ok = enviarEventoRegistro("nfc", uidStr, token);

  if (ok) {
    Serial.println("[NFC] Registrado correctamente ✓");
  } else {
    Serial.println("[NFC] Error al enviar al backend");
    reportarError(token, "error_red");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// REGISTRO HUELLA (automático con reporte de pasos, manejo de error sin ocupar slot)
// ─────────────────────────────────────────────────────────────────────────────
void procesarRegistroHuella(String token) {
  Serial.println("[HUELLA] Iniciando registro automático...");
  uint8_t resp[16];

  // ── Toma 1 ──────────────────────────────────────────────────────
  reportarEstado(token, "acerca_dedo_1");
  Serial.println("[HUELLA] Toma 1 — espera dedo...");

  uint8_t code = 0xFF;
  unsigned long t = millis();
  while (code != 0x00 && millis() - t < 15000) {
    code = enviarComando(CMD_GET_IMAGE, sizeof(CMD_GET_IMAGE), resp, 400);
    delay(150);
  }
  if (code != 0x00) {
    Serial.println("[HUELLA] Timeout toma 1");
    reportarError(token, "timeout_dedo_1");
    return;
  }
  Serial.println("[HUELLA] Imagen 1 capturada ✓");

  code = enviarComando(CMD_IMG2TZ_1, sizeof(CMD_IMG2TZ_1), resp);
  if (code != 0x00) {
    Serial.println("[HUELLA] Error imagen 1: 0x" + String(code, HEX));
    reportarError(token, "error_imagen_1");
    return;
  }

  reportarEstado(token, "dedo_1_ok");
  delay(600);

  // ── Esperar a que retire el dedo ─────────────────────────────────
  reportarEstado(token, "retira_dedo");
  Serial.println("[HUELLA] Retira el dedo...");
  delay(2500);

  // ── Toma 2 ──────────────────────────────────────────────────────
  reportarEstado(token, "acerca_dedo_2");
  Serial.println("[HUELLA] Toma 2 — espera dedo...");

  code = 0xFF;
  t = millis();
  while (code != 0x00 && millis() - t < 15000) {
    code = enviarComando(CMD_GET_IMAGE, sizeof(CMD_GET_IMAGE), resp, 400);
    delay(150);
  }
  if (code != 0x00) {
    Serial.println("[HUELLA] Timeout toma 2");
    reportarError(token, "timeout_dedo_2");
    return;
  }
  Serial.println("[HUELLA] Imagen 2 capturada ✓");

  code = enviarComando(CMD_IMG2TZ_2, sizeof(CMD_IMG2TZ_2), resp);
  if (code != 0x00) {
    Serial.println("[HUELLA] Error imagen 2: 0x" + String(code, HEX));
    reportarError(token, "error_imagen_2");
    return;
  }

  reportarEstado(token, "dedo_2_ok");
  delay(400);

  // ── Crear modelo ─────────────────────────────────────────────────
  reportarEstado(token, "creando_modelo");
  code = enviarComando(CMD_CREATE_MODEL, sizeof(CMD_CREATE_MODEL), resp);
  if (code != 0x00) {
    if (code == 0x0A) {
      Serial.println("[HUELLA] Las dos tomas no coinciden");
      reportarError(token, "huellas_no_coinciden");
    } else {
      Serial.println("[HUELLA] Error creando modelo: 0x" + String(code, HEX));
      reportarError(token, "error_modelo");
    }
    return;
    // IMPORTANTE: al llegar aquí NO se ejecutó STORE, entonces
    // el slot posicionHuella NO fue ocupado. El frontend puede reintentar.
  }

  // ── Guardar en sensor ─────────────────────────────────────────────
  reportarEstado(token, "guardando");

  uint8_t storeCmd[15] = {
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x00, 0x06, 0x06, 0x01,
    (uint8_t)(posicionHuella >> 8),
    (uint8_t)(posicionHuella & 0xFF),
    0x00, 0x00
  };
  uint16_t ck = 0;
  for (int i = 6; i < 13; i++) ck += storeCmd[i];
  storeCmd[13] = (uint8_t)(ck >> 8);
  storeCmd[14] = (uint8_t)(ck & 0xFF);

  code = enviarComando(storeCmd, 15, resp);
  if (code != 0x00) {
    Serial.println("[HUELLA] Error guardando en sensor: 0x" + String(code, HEX));
    reportarError(token, "error_guardado");
    // IMPORTANTE: si STORE falló, el slot puede estar corrupto.
    // Incrementamos de todas formas para no reusar un slot potencialmente dañado.
    posicionHuella++;
    return;
  }

  String posicion = String(posicionHuella);
  Serial.println("[HUELLA] Guardada en posición " + posicion + " ✓");
  posicionHuella++;

  // ── Enviar al backend ─────────────────────────────────────────────
  reportarEstado(token, "enviando");
  bool ok = enviarEventoRegistro("huella", posicion, token);

  if (ok) {
    Serial.println("[HUELLA] Registrada correctamente ✓");
  } else {
    Serial.println("[HUELLA] Guardada en sensor pero error al enviar al backend");
    // La huella está en el sensor pero no en la BD.
    // Reportamos error de red para que el frontend muestre el mensaje correcto.
    reportarError(token, "error_red");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// ACCESO: leer NFC o huella y verificar contra el backend
// ─────────────────────────────────────────────────────────────────────────────
void modoAcceso() {
  // Intentar NFC primero (3 segundos)
  uint8_t uid[7];
  uint8_t uidLen = 0;
  bool detectadoNFC = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 3000);

  if (detectadoNFC) {
    String uidStr = uidToString(uid, uidLen);
    Serial.println("[ACCESO] NFC: " + uidStr);
    verificarAcceso("nfc", uidStr);
    return;
  }

  // Intentar huella (3 segundos)
  uint8_t resp[16];
  uint8_t code = 0xFF;
  unsigned long t = millis();
  while (code != 0x00 && millis() - t < 3000) {
    code = enviarComando(CMD_GET_IMAGE, sizeof(CMD_GET_IMAGE), resp, 400);
    delay(150);
  }
  if (code != 0x00) return; // Nadie tocó nada

  code = enviarComando(CMD_IMG2TZ_1, sizeof(CMD_IMG2TZ_1), resp);
  if (code != 0x00) return;

  // Buscar en librería
  while (huellaSerial.available()) huellaSerial.read();
  huellaSerial.write(CMD_SEARCH, sizeof(CMD_SEARCH));

  uint8_t respB[16];
  int i = 0;
  t = millis();
  while (millis() - t < 3000) {
    if (huellaSerial.available()) respB[i++] = huellaSerial.read();
    if (i >= 14) break;
  }

  if (i >= 12 && respB[9] == 0x00) {
    int16_t pageId = (int16_t)(((uint16_t)respB[10] << 8) | respB[11]);
    Serial.println("[ACCESO] Huella en posición " + String(pageId));
    verificarAcceso("huella", String(pageId));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(600);

  Serial.println("\n================================");
  Serial.println("   AXF GymNet — ESP32 v3.0");
  Serial.println("   (Modo Automático — sin Serial Monitor)");
  Serial.println("================================\n");

  // WiFi
  Serial.print("[WIFI] Conectando a " + String(WIFI_SSID));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 24) {
    delay(500); Serial.print("."); intentos++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Conectado ✓  IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] No conectado — revisando de nuevo en loop");
  }

  // NFC
  SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
  nfc.begin();
  uint32_t ver = nfc.getFirmwareVersion();
  if (!ver) {
    Serial.println("[NFC]  ERROR — PN532 no encontrado");
  } else {
    nfc.SAMConfig();
    Serial.printf("[NFC]  OK ✓  (PN5%02x firmware v%d.%d)\n",
      (ver >> 24) & 0xFF, (ver >> 16) & 0xFF, (ver >> 8) & 0xFF);
  }

  // Sensor huella
  huellaSerial.begin(57600, SERIAL_8N1, HUELLA_RX, HUELLA_TX);
  delay(500);
  uint8_t testResp[16];
  uint8_t testCode = enviarComando(CMD_GET_IMAGE, sizeof(CMD_GET_IMAGE), testResp, 1000);
  if (testCode == 0x02 || testCode == 0x00) {
    Serial.println("[HUELLA] OK ✓  Sensor respondiendo");
  } else {
    Serial.println("[HUELLA] ERROR — verifica RX=16, TX=17");
  }

  Serial.println("\n[INFO] El ESP32 ahora opera en modo automático.");
  Serial.println("[INFO] Al presionar los botones en la web, el dispositivo");
  Serial.println("[INFO] se activa solo sin necesidad de comandos manuales.\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// LOOP — polling automático de tareas
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // Reconectar WiFi si se perdió
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Reconectando...");
    WiFi.reconnect();
    delay(3000);
    return;
  }

  // Si hay una tarea en proceso, no hacer polling de acceso
  if (tareaActiva) {
    delay(200);
    return;
  }

  // ── Buscar tarea de registro NFC pendiente ─────────────────────
  String tokenNFC = "";
  if (buscarTareaPendiente("nfc", tokenNFC)) {
    tareaActiva = true;
    Serial.println("[POLL] Tarea NFC encontrada: " + tokenNFC);
    procesarRegistroNFC(tokenNFC);
    tareaActiva = false;
    delay(500);
    return;
  }

  // ── Buscar tarea de registro HUELLA pendiente ──────────────────
  String tokenHuella = "";
  if (buscarTareaPendiente("huella", tokenHuella)) {
    tareaActiva = true;
    Serial.println("[POLL] Tarea Huella encontrada: " + tokenHuella);
    procesarRegistroHuella(tokenHuella);
    tareaActiva = false;
    delay(500);
    return;
  }

  // ── Si no hay tareas de registro, modo acceso pasivo ──────────
  // (Verifica si alguien intenta entrar con NFC o huella)
  modoAcceso();

  delay(POLL_INTERVALO_MS);
}
