// ============================================================
//  LED-Countdown-Lampe  –  ESP32 + FastLED + BLE UART (NUS) + WiFi
//  Kompatibel mit Adafruit Bluefruit Connect App
// ============================================================
#include <Arduino.h>
#include <FastLED.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// -----------------------------------------------------------
// Hardware
// -----------------------------------------------------------
static constexpr uint8_t  LED_PIN    = 16;
static constexpr uint16_t LEDS_TOTAL = 48;

CRGB leds[LEDS_TOTAL];

// -----------------------------------------------------------
// BLE Nordic UART Service (NUS)
// -----------------------------------------------------------
#define DEVICE_NAME         "DIY LED Lampe v2"
#define NUS_SERVICE_UUID    "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_CHAR_UUID    "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_CHAR_UUID    "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLECharacteristic* pTxChar  = nullptr;
bool               bleConnected = false;

static void bleSend(const char* msg) {
  if (bleConnected && pTxChar) {
    pTxChar->setValue((uint8_t*)msg, strlen(msg));
    pTxChar->notify();
  }
}

// -----------------------------------------------------------
// Web-Server & WiFiManager (global)
// -----------------------------------------------------------
WebServer   server(80);
WiFiManager wm;

// -----------------------------------------------------------
// Timer-Konstanten
// -----------------------------------------------------------
static constexpr uint32_t DEFAULT_MINUTES = 5;
static constexpr uint32_t LAST_MINUTE_MS  = 60UL  * 1000UL;
static constexpr uint32_t YELLOW_MS       = 50UL  * 1000UL;
static constexpr uint32_t RED_MS          = 10UL  * 1000UL;
static constexpr float    TIME_CORRECTION = 1.0f;

// -----------------------------------------------------------
// Programm-Zustand
// -----------------------------------------------------------
enum class Mode : uint8_t { Idle, Rainbow, Running, BlinkExpired, Fire };

Mode     mode              = Mode::Rainbow;
uint32_t configuredMinutes = DEFAULT_MINUTES;
uint32_t startMs           = 0;
uint32_t blinkLastToggleMs = 0;
bool     blinkOn           = false;
uint32_t lastFrameMs       = 0;
uint8_t currentBrightness = 200;

// -----------------------------------------------------------
// Helpers
// -----------------------------------------------------------
static uint32_t msFromMinutes(uint32_t minutes) {
  return (uint32_t)(minutes * 60UL * 1000UL * TIME_CORRECTION);
}

static void startCountdown() {
  mode    = Mode::Running;
  startMs = millis();
}

static void resetLamp() {
  mode = Mode::Idle;
  FastLED.clear(true);
}

static void setMinutes(uint32_t minutes) {
  configuredMinutes = (uint32_t)constrain((int)minutes, 1, 999);
  if (mode == Mode::Running) startCountdown();
}

// -----------------------------------------------------------
// Countdown-Rendering
// -----------------------------------------------------------
static void renderCountdown(uint32_t elapsedMs) {
  const uint32_t totalMs   = msFromMinutes(configuredMinutes);
  const uint32_t greenEnd  = (totalMs > LAST_MINUTE_MS) ? (totalMs - LAST_MINUTE_MS) : 0;
  const uint32_t yellowEnd = (totalMs > RED_MS)         ? (totalMs - RED_MS)         : 0;

  fill_solid(leds, LEDS_TOTAL, CRGB::Black);
  if (totalMs == 0) return;

  if (elapsedMs < greenEnd) {
    float    p  = (greenEnd == 0) ? 1.0f : (float)elapsedMs / (float)greenEnd;
    uint16_t on = max((uint16_t)1, (uint16_t)(p * LEDS_TOTAL));
    if (on > LEDS_TOTAL) on = LEDS_TOTAL;
    fill_solid(leds, on, CRGB::Green);
    return;
  }

  fill_solid(leds, LEDS_TOTAL, CRGB::Green);

  if (elapsedMs < yellowEnd) {
    float    p  = (YELLOW_MS == 0) ? 1.0f : (float)(elapsedMs - greenEnd) / (float)YELLOW_MS;
    uint16_t on = (uint16_t)(p * LEDS_TOTAL);
    if (on > LEDS_TOTAL) on = LEDS_TOTAL;
    fill_solid(leds, on, CRGB::Yellow);
    return;
  }

  fill_solid(leds, LEDS_TOTAL, CRGB::Yellow);

  float    p  = (RED_MS == 0) ? 1.0f : (float)(elapsedMs - yellowEnd) / (float)RED_MS;
  uint16_t on = (uint16_t)(p * LEDS_TOTAL);
  if (on > LEDS_TOTAL) on = LEDS_TOTAL;
  fill_solid(leds, on, CRGB::Red);
}

// -----------------------------------------------------------
// Fire2012
// -----------------------------------------------------------
static constexpr uint8_t COOLING  = 55;
static constexpr uint8_t SPARKING = 120;
static byte heat[LEDS_TOTAL];

static void renderFire() {
  for (uint16_t i = 0; i < LEDS_TOTAL; i++)
    heat[i] = qsub8(heat[i], random8(0, ((COOLING * 10) / LEDS_TOTAL) + 2));
  for (uint16_t k = LEDS_TOTAL - 1; k >= 2; k--)
    heat[k] = (heat[k-1] + heat[k-2] + heat[k-2]) / 3;
  if (random8() < SPARKING) {
    uint8_t y = random8(7);
    heat[y] = qadd8(heat[y], random8(160, 255));
  }
  for (uint16_t j = 0; j < LEDS_TOTAL; j++)
    leds[j] = HeatColor(heat[j]);
}

// -----------------------------------------------------------
// Rainbow
// -----------------------------------------------------------
static uint8_t rainbowHue = 0;

static void renderRainbow() {
  fill_rainbow(leds, LEDS_TOTAL, rainbowHue, 7);
  rainbowHue++;
}

// -----------------------------------------------------------
// Command-Verarbeitung (gemeinsam für BLE und HTTP)
// -----------------------------------------------------------
static String processCommand(const char* cmd) {
  char lower[32];
  uint8_t i = 0;
  while (cmd[i] && i < 31) { lower[i] = tolower((unsigned char)cmd[i]); i++; }
  lower[i] = '\0';

  if (strcmp(lower, "s") == 0 || strcmp(lower, "start") == 0) {
    startCountdown();
    return "Started";
  }
  if (strcmp(lower, "r") == 0 || strcmp(lower, "reset") == 0) {
    resetLamp();
    return "Stopped";
  }
  if (strcmp(lower, "f") == 0 || strcmp(lower, "fire") == 0) {
    mode = Mode::Fire;
    return "Fire";
  }
  if (strcmp(lower, "p") == 0 || strcmp(lower, "pride") == 0) {
    mode = Mode::Rainbow;
    return "Rainbow";
  }

  const char* numPart = nullptr;
  if (lower[0] == 't') {
    numPart = (strncmp(lower, "time", 4) == 0) ? lower + 4 : lower + 1;
    if (*numPart == '\0') {
      char buf[32];
      snprintf(buf, sizeof(buf), "Timer: %u minutes", (unsigned)configuredMinutes);
      return String(buf);
    }
    int minutes = atoi(numPart);
    if (minutes >= 1) {
      setMinutes((uint32_t)minutes);
      char buf[32];
      snprintf(buf, sizeof(buf), "Timer: %d minutes", minutes);
      return String(buf);
    }
  }
  if (strcmp(lower, "i") == 0 || strcmp(lower, "ip") == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      return "IP: " + WiFi.localIP().toString();
    } else {
      return "Kein WLAN";
    }
  }
  if (strncmp(lower, "b", 1) == 0 || strncmp(lower, "brightness", 10) == 0) {
    const char* numPart = (strncmp(lower, "brightness", 10) == 0) ? lower + 10 : lower + 1;
    if (*numPart == '\0') {
      char buf[32];
      snprintf(buf, sizeof(buf), "Brightness: %d%%", (int)(currentBrightness * 100 / 255));
      return String(buf);
    }
    int pct = atoi(numPart);
    pct = constrain(pct, 0, 100);
    currentBrightness = (uint8_t)(pct * 255 / 100);
    FastLED.setBrightness(currentBrightness);
    char buf[32];
    snprintf(buf, sizeof(buf), "Brightness: %d%%", pct);
    return String(buf);
  }
  return "Unknown command";
}

// -----------------------------------------------------------
// Web-Interface
// -----------------------------------------------------------
static const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel='icon' type='image/svg+xml' href='data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"><text y=".9em" font-size="90">🌈</text></svg>'>
<title>DIY LED Lampe v2</title>
<style>
  body { font-family: sans-serif; max-width: 400px; margin: 40px auto; padding: 0 16px; background: #111; color: #eee; }
  h1   { font-size: 1.4em; margin-bottom: 4px; }
  p    { color: #aaa; font-size: 0.9em; margin-top: 0; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 20px; }
  button {
    padding: 14px; border: none; border-radius: 8px;
    font-size: 1em; cursor: pointer; color: #fff; font-weight: bold;
  }
  .start  { background: #2e7d32; }
  .reset  { background: #555; }
  .fire   { background: #bf360c; }
  .pride  { background: #6a1b9a; }
  .time   { background: #1565c0; grid-column: span 2; display: flex; gap: 8px; }
  .time input {
    flex: 1; padding: 12px; border-radius: 8px; border: none;
    font-size: 1em; background: #222; color: #eee; text-align: center;
  }
  .time button { flex: 0 0 auto; padding: 12px 20px; }
  #status { margin-top: 20px; padding: 10px; background: #222; border-radius: 8px; font-size: 0.9em; min-height: 20px; }
</style>
</head>
<body>
<h1>DIY LED Lampe v2</h1>
<p>Steuerung per WLAN</p>
<div style='margin-top:12px;padding:10px;background:#1a1a2e;border-left:4px solid #5c6bc0;border-radius:4px;font-size:0.85em;color:#aaa'>
📖 Dokumentation: <a href='https://wiki.cogneon.de/DIY_LED-Lampe' target='_blank' style='color:#7986cb'>wiki.cogneon.de/DIY_LED-Lampe</a>
</div>
<div class="grid">
  <button class="start" onclick="cmd('start')">▶ Start</button>
  <button class="reset" onclick="cmd('reset')">■ Reset</button>
  <button class="fire"  onclick="cmd('fire')">🔥 Fire</button>
  <button class="pride" onclick="cmd('pride')">🌈 Rainbow</button>
  <div class="time">
    <input type="number" id="mins" value="5" min="1" max="999" placeholder="Minuten">
    <button style="background:#1565c0" onclick="setTime()">⏱ Zeit setzen</button>
  </div>
</div>
<div style='grid-column:span 2;margin-top:4px'>
  <label style='font-size:0.85em;color:#aaa'>Helligkeit</label>
  <input type='range' min='0' max='100' value='80' style='width:100%;margin-top:6px;accent-color:#5c6bc0'
    oninput="cmd('brightness'+this.value)">
</div>
<div id="status">Bereit.</div>
<script>
function cmd(c) {
  fetch('/cmd?c=' + c)
    .then(r => r.text())
    .then(t => document.getElementById('status').innerText = t)
    .catch(() => document.getElementById('status').innerText = 'Fehler');
}
function setTime() {
  const m = document.getElementById('mins').value;
  cmd('time' + m);
}
</script>
</body>
</html>
)rawliteral";

static void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", HTML_PAGE);
  });
  server.on("/cmd", HTTP_GET, []() {
    if (server.hasArg("c")) {
      String result = processCommand(server.arg("c").c_str());
      server.send(200, "text/plain", result);
    } else {
      server.send(400, "text/plain", "Missing parameter c");
    }
  });
  server.begin();
}

// -----------------------------------------------------------
// BLE-Callbacks
// -----------------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*)      override { bleConnected = true; }
  void onDisconnect(BLEServer* s) override {
    bleConnected = false;
    s->startAdvertising();
  }
};

static char    cmdBuf[32];
static uint8_t cmdLen = 0;

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String value = pChar->getValue();
    for (int i = 0; i < value.length(); i++) {
      char ch = value[i];
      if (ch == '\r' || ch == '\n') {
        if (cmdLen > 0) {
          cmdBuf[cmdLen] = '\0';
          String result = processCommand(cmdBuf);
          result += "\n";
          bleSend(result.c_str());
          cmdLen = 0;
        }
        continue;
      }
      if (cmdLen < sizeof(cmdBuf) - 1) cmdBuf[cmdLen++] = ch;
    }
  }
};

// -----------------------------------------------------------
// Setup
// -----------------------------------------------------------
void setup() {
  Serial.begin(115200);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LEDS_TOTAL);
  FastLED.setBrightness(255);
  FastLED.clear(true);

  // --- WiFiManager (non-blocking) ---
  wm.setConfigPortalBlocking(false);
  wm.autoConnect(DEVICE_NAME);

  // --- BLE ---
  BLEDevice::init(DEVICE_NAME);
  BLEServer*  pServer  = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(NUS_SERVICE_UUID);

  pTxChar = pService->createCharacteristic(NUS_TX_CHAR_UUID,
              BLECharacteristic::PROPERTY_NOTIFY);
  pTxChar->addDescriptor(new BLE2902());

  BLECharacteristic* pRxChar = pService->createCharacteristic(NUS_RX_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRxChar->setCallbacks(new RxCallbacks());

  pService->start();
  pServer->getAdvertising()->addServiceUUID(NUS_SERVICE_UUID);
  pServer->startAdvertising();

  // Startup-Effekt
  mode = Mode::Rainbow;
}

// -----------------------------------------------------------
// Loop
// -----------------------------------------------------------
void loop() {
  // WiFiManager im Hintergrund verarbeiten
  wm.process();

  // WebServer nur aktiv, wenn WLAN verbunden
  if (WiFi.status() == WL_CONNECTED) {
    static bool wifiSetupDone = false;
    if (!wifiSetupDone) {
      wifiSetupDone = true;
      Serial.print("WLAN verbunden. IP: ");
      Serial.println(WiFi.localIP());
      if (MDNS.begin("ledlampe")) {
        Serial.println("mDNS: http://ledlampe.local");
      }
      setupWebServer();
    }
    server.handleClient();
  }

  const uint32_t now = millis();

  switch (mode) {

    case Mode::Idle:
      break;

    case Mode::Running: {
      const uint32_t elapsed = now - startMs;
      if (elapsed >= msFromMinutes(configuredMinutes)) {
        mode              = Mode::BlinkExpired;
        blinkLastToggleMs = now;
        blinkOn           = true;
      } else if (now - lastFrameMs >= 80) {
        lastFrameMs = now;
        renderCountdown(elapsed);
        FastLED.show();
      }
      break;
    }

    case Mode::BlinkExpired:
      if (now - blinkLastToggleMs >= 500) {
        blinkLastToggleMs = now;
        blinkOn           = !blinkOn;
        fill_solid(leds, LEDS_TOTAL, blinkOn ? CRGB::Red : CRGB::Black);
        FastLED.show();
      }
      break;

    case Mode::Fire:
      if (now - lastFrameMs >= 50) {
        lastFrameMs = now;
        renderFire();
        FastLED.show();
      }
      break;

    case Mode::Rainbow:
      if (now - lastFrameMs >= 20) {
        lastFrameMs = now;
        renderRainbow();
        FastLED.show();
      }
      break;
  }
}