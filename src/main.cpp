#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>

// WiFi credentials loaded at compile time
#include "wifi_secrets.h"


// Persistent storage
Preferences preferences;

// Pin assignments (recommended for ESP32)
const int ledPin      = 2;   // Built-in LED
const int PIN_U1_EN   = 13;
const int PIN_U1_STEP = 33;
const int PIN_U1_DIR  = 32;
int period = 150;
// Save/load period value
void savePeriod(int value) {
  preferences.begin("period", false);
  preferences.putInt("value", value);
  preferences.end();
}

int loadPeriod() {
  preferences.begin("period", true);
  int value = preferences.getInt("value", 150);
  preferences.end();
  return value;
}

// Motor state
enum MotorState { MOTOR_STOP, MOTOR_UP, MOTOR_DOWN };
MotorState motorState = MOTOR_STOP;

// Fixed-step move state
volatile bool isMovingSteps = false;
volatile int stepsToMove = 0;
volatile int stepDirection = 1; // 1 = UP, -1 = DOWN
// Default value for fixed-step count
int fixedStepCount = 100;

void saveMotorState(MotorState state) {
  preferences.begin("motor", false);
  preferences.putInt("state", (int)state);
  preferences.end();
}

MotorState loadMotorState() {
  preferences.begin("motor", true);
  int state = preferences.getInt("state", (int)MOTOR_STOP);
  preferences.end();
  return (MotorState)state;
}

// Web server
AsyncWebServer server(80);

// LED state
bool ledState = false;

void saveLedState(bool state) {
  preferences.begin("led", false);
  preferences.putBool("state", state);
  preferences.end();
}

bool loadLedState() {
  preferences.begin("led", true);
  bool state = preferences.getBool("state", false);
  preferences.end();
  return state;
}



String processor(const String& var) {
  if (var == "BUTTON_TEXT") {
    return ledState ? "Turn OFF" : "Turn ON";
  } else if (var == "LED_STATE") {
    return ledState ? "ON" : "OFF";
  }
  return String();
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());
}

// Web page HTML (global scope)
const char index_html[] PROGMEM = R"rawliteral(
  <!DOCTYPE HTML><html>
  <head>
    <meta name='viewport' content='width=device-width, initial-scale=1'>
    <title>ESP32 Control</title>
  </head>
  <body>
    <h2>ESP32 Web Server</h2>
    <p>LED State: <strong>%LED_STATE%</strong></p>
    <p>Motor State: <strong>%MOTOR_STATE%</strong></p>
    <p>Current PERIOD (us): <strong>%PERIOD%</strong></p>
    <form action="/setperiod" method="POST" style="margin-bottom:16px;">
      <input type="number" name="period" min="10" max="10000" value="%PERIOD%" required %DISABLED%>
      <button type="submit" %DISABLED%>Set PERIOD</button>
    </form>
    <form action="/setsteps" method="POST" style="margin-bottom:16px;">
      <input type="number" name="steps" min="1" max="10000" value="%STEPS%" required %DISABLED%>
      <button type="submit" %DISABLED%>Set Steps</button>
    </form>
    <form action="/moveup" method="POST" style="display:inline;">
      <button type="submit" %DISABLED%>Move Up</button>
    </form>
    <form action="/stop" method="POST" style="display:inline;">
      <button type="submit" %DISABLED%>Stop</button>
    </form>
    <form action="/movedown" method="POST" style="display:inline;">
      <button type="submit" %DISABLED%>Move Down</button>
    </form>
    <form action="/moveupsteps" method="POST" style="display:inline; margin-left:20px;">
      <button type="submit" %DISABLED%>Move Up %STEPS% Steps</button>
    </form>
    <form action="/movedownsteps" method="POST" style="display:inline;">
      <button type="submit" %DISABLED%>Move Down %STEPS% Steps</button>
    </form>
    <form action="/toggleled" method="POST" style="display:inline; margin-left:20px;">
      <button type="submit" %DISABLED%>Toggle LED</button>
    </form>
  </body>
  </html>
)rawliteral";

String getMotorStateString() {
  switch (motorState) {
    case MOTOR_UP: return "UP";
    case MOTOR_DOWN: return "DOWN";
    case MOTOR_STOP:
    default: return "STOP";
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  ledState = loadLedState();
  digitalWrite(ledPin, ledState ? HIGH : LOW);

  // Motor and switch pins

  pinMode(PIN_U1_EN, OUTPUT);
  pinMode(PIN_U1_STEP, OUTPUT);
  pinMode(PIN_U1_DIR, OUTPUT);
  digitalWrite(PIN_U1_EN, HIGH);
  digitalWrite(PIN_U1_STEP, LOW);
  digitalWrite(PIN_U1_DIR, HIGH);

  motorState = loadMotorState();
  period = loadPeriod();

  connectToWiFi();

  // Auto-reconnect WiFi
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.println("WiFi lost. Reconnecting...");
      connectToWiFi();
    }
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = index_html;
    html.replace("%LED_STATE%", ledState ? "ON" : "OFF");
    html.replace("%MOTOR_STATE%", getMotorStateString());
    html.replace("%PERIOD%", String(period));
    html.replace("%STEPS%", String(fixedStepCount));
    // Disable all controls if moving steps
    if (isMovingSteps) {
      html.replace("%DISABLED%", "disabled");
    } else {
      html.replace("%DISABLED%", "");
    }
    request->send(200, "text/html", html);
  });
  // Handler to set the number of steps for fixed-step move
  server.on("/setsteps", HTTP_POST, [](AsyncWebServerRequest *request){
    if (isMovingSteps) {
      request->redirect("/");
      return;
    }
    if (request->hasParam("steps", true)) {
      int newSteps = request->getParam("steps", true)->value().toInt();
      if (newSteps >= 1 && newSteps <= 10000) {
        fixedStepCount = newSteps;
      }
    }
    request->redirect("/");
  });

  server.on("/setperiod", HTTP_POST, [](AsyncWebServerRequest *request){
    if (isMovingSteps) {
      request->redirect("/");
      return;
    }
    if (request->hasParam("period", true)) {
      int newPeriod = request->getParam("period", true)->value().toInt();
      if (newPeriod >= 10 && newPeriod <= 10000) {
        period = newPeriod;
        savePeriod(period);
      }
    }
    request->redirect("/");
  });

  server.on("/toggleled", HTTP_POST, [](AsyncWebServerRequest *request){
    if (isMovingSteps) {
      request->redirect("/");
      return;
    }
    ledState = !ledState;
    digitalWrite(ledPin, ledState ? HIGH : LOW);
    saveLedState(ledState);
    request->redirect("/");
  });

  server.on("/moveup", HTTP_POST, [](AsyncWebServerRequest *request){
    if (isMovingSteps) {
      request->redirect("/");
      return;
    }
    if (motorState == MOTOR_UP) {
      // Already UP, do nothing
      request->redirect("/");
      return;
    }
    if (motorState == MOTOR_DOWN) {
      // If DOWN, stop first
      motorState = MOTOR_STOP;
      saveMotorState(motorState);
      delay(200); // Short delay to ensure stop
    }
    motorState = MOTOR_UP;
    saveMotorState(motorState);
    request->redirect("/");
  });
  server.on("/movedown", HTTP_POST, [](AsyncWebServerRequest *request){
    if (isMovingSteps) {
      request->redirect("/");
      return;
    }
    if (motorState == MOTOR_DOWN) {
      // Already DOWN, do nothing
      request->redirect("/");
      return;
    }
    if (motorState == MOTOR_UP) {
      // If UP, stop first
      motorState = MOTOR_STOP;
      saveMotorState(motorState);
      delay(200); // Short delay to ensure stop
    }
    motorState = MOTOR_DOWN;
    saveMotorState(motorState);
    request->redirect("/");
  });
  server.on("/stop", HTTP_POST, [](AsyncWebServerRequest *request){
    if (isMovingSteps) {
      request->redirect("/");
      return;
    }
    motorState = MOTOR_STOP;
    saveMotorState(motorState);
    request->redirect("/");
  });

  // Handler for Move Up 100 Steps (waits until done)
  server.on("/moveupsteps", HTTP_POST, [](AsyncWebServerRequest *request){
    if (isMovingSteps) {
      request->redirect("/");
      return;
    }
    isMovingSteps = true;
    stepsToMove = fixedStepCount;
    stepDirection = 1; // UP
    // Wait until move is done
    while (isMovingSteps) {
      delay(10);
    }
    request->redirect("/");
  });

  server.on("/movedownsteps", HTTP_POST, [](AsyncWebServerRequest *request){
    if (isMovingSteps) {
      request->redirect("/");
      return;
    }
    isMovingSteps = true;
    stepsToMove = fixedStepCount;
    stepDirection = -1; // DOWN
    // Wait until move is done
    while (isMovingSteps) {
      delay(10);
    }
    request->redirect("/");
  });

  server.begin();
}

void loop() {
  // Fixed-step move logic
  if (isMovingSteps) {
    // Set direction
    digitalWrite(PIN_U1_DIR, stepDirection == 1 ? HIGH : LOW);
    digitalWrite(PIN_U1_EN, LOW);
    for (int i = 0; i < stepsToMove; ++i) {
      digitalWrite(PIN_U1_STEP, HIGH);
      delayMicroseconds(period);
      digitalWrite(PIN_U1_STEP, LOW);
      delayMicroseconds(period);
    }
    digitalWrite(PIN_U1_EN, HIGH); // Disable after move
    isMovingSteps = false;
    motorState = MOTOR_STOP;
    saveMotorState(motorState);
    return;
  }
  // Stepper motor logic (U1 only)
  switch (motorState) {
    case MOTOR_UP:
      digitalWrite(PIN_U1_DIR, HIGH); // Unroll
      digitalWrite(PIN_U1_EN, LOW);
      digitalWrite(PIN_U1_STEP, HIGH);
      delayMicroseconds(period);
      digitalWrite(PIN_U1_STEP, LOW);
      delayMicroseconds(period);
      break;
    case MOTOR_DOWN:
      digitalWrite(PIN_U1_DIR, LOW); // Roll
      digitalWrite(PIN_U1_EN, LOW);
      digitalWrite(PIN_U1_STEP, HIGH);
      delayMicroseconds(period);
      digitalWrite(PIN_U1_STEP, LOW);
      delayMicroseconds(period);
      break;
    case MOTOR_STOP:
    default:
      digitalWrite(PIN_U1_EN, HIGH); // Disable
      break;
  }
}