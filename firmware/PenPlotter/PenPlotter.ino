/**
 * Pen Plotter Firmware  v1.2
 * Made by Jacob Majors and Troy Pappas
 *
 * Hardware : Melzi Creality (ATmega1284P)
 * Display  : CR10 stock — 128×64 ST7920 + rotary encoder + click
 * Motors   : X axis (belt)  |  Y axis (belt, was Z after printer flip)
 * Pen      : Servo on pin 13 (former hotend heater pin)
 *
 * Navigation
 *   Turn knob  — scroll menu / move axis / adjust value
 *   Click      — select / confirm / enter jog mode
 *   Click (in jog / edit) — exit back to menu
 */

#include <SPI.h>
#include <U8g2lib.h>
#include <AccelStepper.h>
#include <Servo.h>
#include <EEPROM.h>

// ── Pin Assignments — Creality v4.2.2 (STM32F103RCT6 / RET6) ─────────────────
// All steppers share one enable pin (active LOW)
#define XY_EN_PIN     PC3

#define X_STEP_PIN    PC2
#define X_DIR_PIN     PB9

// Y uses the old Z stepper after the axis flip + belt conversion
#define Y_STEP_PIN    PB6   // was Z_STEP
#define Y_DIR_PIN     PB5   // was Z_DIR

#define PEN_SERVO_PIN PB0   // BLTouch signal port — repurposed for pen servo

// CR10 stock display — RET6 connector (standard Ender-3 display)
#define ENC_A_PIN     PB10  // BTN_EN1
#define ENC_B_PIN     PB14  // BTN_EN2
#define ENC_BTN_PIN   PB2   // BTN_ENC (click)
#define LCD_CLK_PIN   PB13  // ST7920 CLK  (LCD_PINS_D4)
#define LCD_DATA_PIN  PB15  // ST7920 DATA (LCD_PINS_EN)
#define LCD_CS_PIN    PB12  // ST7920 CS   (LCD_PINS_RS)
#define BEEPER_PIN    PC6

// ── Motion ────────────────────────────────────────────────────────────────────
#define X_STEPS_MM    80.0f
#define Y_STEPS_MM    80.0f   // recalibrate after fitting belt to Y
#define JOG_SPEED     3000    // steps/s
#define JOG_ACCEL     8000    // steps/s²
#define JOG_STEP_MM   0.5f   // mm per encoder tick in live-jog mode

// ── Pen Defaults ──────────────────────────────────────────────────────────────
#define PEN_UP_DEF   90
#define PEN_DN_DEF   45
#define PEN_MIN       0
#define PEN_MAX     180

// ── EEPROM ────────────────────────────────────────────────────────────────────
#define EE_MAGIC   0
#define EE_PEN_UP  1
#define EE_PEN_DN  2
#define MAGIC    0xA5

// ── Hardware Objects ──────────────────────────────────────────────────────────
U8G2_ST7920_128X64_F_SW_SPI u8g2(U8G2_R0, LCD_CLK_PIN, LCD_DATA_PIN, LCD_CS_PIN);
AccelStepper sx(AccelStepper::DRIVER, X_STEP_PIN, X_DIR_PIN);
AccelStepper sy(AccelStepper::DRIVER, Y_STEP_PIN, Y_DIR_PIN);
Servo penServo;

// ── App State ─────────────────────────────────────────────────────────────────
enum Screen { MAIN, MANUAL, JOG_X, JOG_Y, PEN_HEIGHTS, EDIT_ANGLE, ABOUT };

Screen  scr      = MAIN;
int8_t  cursor   = 0;
bool    redraw   = true;

uint8_t penUpAng = PEN_UP_DEF;
uint8_t penDnAng = PEN_DN_DEF;
bool    penIsUp  = true;

float   posX = 0.0f;
float   posY = 0.0f;

uint8_t editIdx = 0;   // 0 = up, 1 = down
int16_t editVal = 0;

// ── Encoder ───────────────────────────────────────────────────────────────────
bool     encA_last = HIGH;
bool     btn_last  = HIGH;
uint32_t btn_time  = 0;
int      enc_acc   = 0;
bool     clicked   = false;

// ── Menu Strings ──────────────────────────────────────────────────────────────
const char* MAIN_ITEMS[] = { "Manual Move", "Pen Heights", "About" };
const char* MAN_ITEMS[]  = { "Jog X Axis", "Jog Y Axis",
                              "Pen Up", "Pen Down", "< Back" };
const char* PEN_ITEMS[]  = { "Set Up Height", "Set Down Height",
                              "Test Pen", "< Back" };
#define MAIN_N 3
#define MAN_N  5
#define PEN_N  4

// ─────────────────────────────────────────────────────────────────────────────
// EEPROM
// ─────────────────────────────────────────────────────────────────────────────
void loadSettings() {
  if (EEPROM.read(EE_MAGIC) == MAGIC) {
    penUpAng = EEPROM.read(EE_PEN_UP);
    penDnAng = EEPROM.read(EE_PEN_DN);
  } else {
    penUpAng = PEN_UP_DEF;
    penDnAng = PEN_DN_DEF;
    EEPROM.write(EE_MAGIC,  MAGIC);
    EEPROM.write(EE_PEN_UP, penUpAng);
    EEPROM.write(EE_PEN_DN, penDnAng);
  }
}

void saveSettings() {
  EEPROM.write(EE_PEN_UP, penUpAng);
  EEPROM.write(EE_PEN_DN, penDnAng);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pen
// ─────────────────────────────────────────────────────────────────────────────
void doPenUp()   { penServo.write(penUpAng); penIsUp = true; }
void doPenDown() { penServo.write(penDnAng); penIsUp = false; }

// ─────────────────────────────────────────────────────────────────────────────
// Steppers
// ─────────────────────────────────────────────────────────────────────────────
void enX()  { digitalWrite(XY_EN_PIN, LOW); }
void enY()  { digitalWrite(XY_EN_PIN, LOW); }
void disX() { if (!sx.distanceToGo() && !sy.distanceToGo()) digitalWrite(XY_EN_PIN, HIGH); }
void disY() { if (!sx.distanceToGo() && !sy.distanceToGo()) digitalWrite(XY_EN_PIN, HIGH); }

// ─────────────────────────────────────────────────────────────────────────────
// Beeper
// ─────────────────────────────────────────────────────────────────────────────
void beep(uint16_t freq = 2000, uint16_t ms = 20) { tone(BEEPER_PIN, freq, ms); }

// ─────────────────────────────────────────────────────────────────────────────
// Encoder  (poll every loop iteration)
// ─────────────────────────────────────────────────────────────────────────────
void pollEnc() {
  bool a = digitalRead(ENC_A_PIN);
  if (a != encA_last) {
    if (a == LOW)
      enc_acc += (digitalRead(ENC_B_PIN) == HIGH) ? 1 : -1;
    encA_last = a;
  }
  bool btn = digitalRead(ENC_BTN_PIN);
  uint32_t now = millis();
  if (btn != btn_last && (now - btn_time) > 50) {
    btn_time = now;
    btn_last = btn;
    if (btn == LOW) { clicked = true; beep(); }
  }
}

// Returns detents (±1) since last call.
// If menu skips 2 items per tick → change /2 to /4.
// If it takes 2 ticks per item → change /2 to /1.
int getDelta() {
  static int rem = 0;
  rem += enc_acc; enc_acc = 0;
  int d = rem / 2; rem -= d * 2;
  return d;
}

// ─────────────────────────────────────────────────────────────────────────────
// DISPLAY HELPERS
// ─────────────────────────────────────────────────────────────────────────────
void drawHeader(const char* title) {
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(2, 10, title);
  u8g2.drawHLine(0, 13, 128);
}

// Scrollable list; up to 4 rows (leaves 1 row for status bar at bottom)
void drawList(const char** items, int n, int sel) {
  const int ROWS = 4;
  int start = constrain(sel - 1, 0, max(0, n - ROWS));
  u8g2.setFont(u8g2_font_5x8_tr);
  for (int i = 0; i < ROWS && (start + i) < n; i++) {
    int idx = start + i;
    int y   = 16 + i * 12;
    if (idx == sel) {
      u8g2.drawBox(0, y, 128, 12);
      u8g2.setDrawColor(0);
    }
    u8g2.drawStr(4, y + 9, items[idx]);
    u8g2.setDrawColor(1);
  }
}

// Horizontal bar: (value/max) filled across barW pixels
void drawBar(int x, int y, int barW, int barH, int value, int maxVal) {
  u8g2.drawFrame(x, y, barW, barH);
  int fill = (int)((float)value / maxVal * (barW - 2));
  fill = constrain(fill, 0, barW - 2);
  if (fill > 0) u8g2.drawBox(x + 1, y + 1, fill, barH - 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// SCREEN DRAW FUNCTIONS
// ─────────────────────────────────────────────────────────────────────────────

// ── Main ──────────────────────────────────────────────────────────────────────
void drawMain() {
  u8g2.clearBuffer();
  drawHeader("PEN PLOTTER");
  drawList(MAIN_ITEMS, MAIN_N, cursor);
  // Bottom status bar
  u8g2.setFont(u8g2_font_5x8_tr);
  char buf[24];
  snprintf(buf, sizeof(buf), "X%+5.1f  Y%+5.1f  %s",
           posX, posY, penIsUp ? "UP" : "DN");
  u8g2.drawStr(0, 63, buf);
  u8g2.sendBuffer();
}

// ── Manual Move ───────────────────────────────────────────────────────────────
void drawManual() {
  u8g2.clearBuffer();
  drawHeader("MANUAL MOVE");

  // Position readout under header
  char buf[28];
  u8g2.setFont(u8g2_font_5x8_tr);
  snprintf(buf, sizeof(buf), "X%+6.1fmm  Y%+6.1fmm", posX, posY);
  u8g2.drawStr(2, 24, buf);
  u8g2.drawHLine(0, 26, 128);

  // 3-item list (fits below position bar)
  const int ROWS = 3;
  int start = constrain(cursor - 1, 0, max(0, MAN_N - ROWS));
  u8g2.setFont(u8g2_font_5x8_tr);
  for (int i = 0; i < ROWS && (start + i) < MAN_N; i++) {
    int idx = start + i;
    int y   = 28 + i * 12;
    if (idx == cursor) {
      u8g2.drawBox(0, y, 128, 12);
      u8g2.setDrawColor(0);
    }
    // Show pen state next to Pen Up/Down
    const char* label = MAN_ITEMS[idx];
    u8g2.drawStr(4, y + 9, label);
    if (idx == 2 || idx == 3) {
      const char* tag = penIsUp ? "[UP]" : "[DN]";
      u8g2.drawStr(100, y + 9, tag);
    }
    u8g2.setDrawColor(1);
  }
  u8g2.sendBuffer();
}

// ── Jog X / Y ─────────────────────────────────────────────────────────────────
void drawJog(bool isX) {
  u8g2.clearBuffer();
  const char* title = isX ? "JOG  X  AXIS" : "JOG  Y  AXIS";
  drawHeader(title);

  // Big position value
  char buf[16];
  float pos = isX ? posX : posY;
  snprintf(buf, sizeof(buf), "%+.1f mm", pos);
  u8g2.setFont(u8g2_font_10x20_tr);
  int tw = u8g2.getStrWidth(buf);
  u8g2.drawStr((128 - tw) / 2, 40, buf);

  // Arrow hints at top right
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(90, 11, isX ? "< X >" : "< Y >");

  // Footer
  u8g2.drawHLine(0, 50, 128);
  u8g2.drawStr(2, 62, "Turn:move   Click:done");
  u8g2.sendBuffer();
}

// ── Pen Heights ───────────────────────────────────────────────────────────────
void drawPenHeights() {
  u8g2.clearBuffer();
  drawHeader("PEN HEIGHTS");

  u8g2.setFont(u8g2_font_5x8_tr);

  // Up height row
  {
    bool sel = (cursor == 0);
    if (sel) { u8g2.drawBox(0, 15, 128, 12); u8g2.setDrawColor(0); }
    u8g2.drawStr(4, 24, "Up Height");
    char b[8]; snprintf(b, sizeof(b), "%3d", penUpAng);
    u8g2.drawStr(72, 24, b);
    drawBar(84, 16, 40, 8, penUpAng, PEN_MAX);
    u8g2.setDrawColor(1);
  }

  // Down height row
  {
    bool sel = (cursor == 1);
    if (sel) { u8g2.drawBox(0, 28, 128, 12); u8g2.setDrawColor(0); }
    u8g2.drawStr(4, 37, "Down Height");
    char b[8]; snprintf(b, sizeof(b), "%3d", penDnAng);
    u8g2.drawStr(72, 37, b);
    drawBar(84, 29, 40, 8, penDnAng, PEN_MAX);
    u8g2.setDrawColor(1);
  }

  // Test Pen row
  {
    bool sel = (cursor == 2);
    if (sel) { u8g2.drawBox(0, 41, 128, 12); u8g2.setDrawColor(0); }
    u8g2.drawStr(4, 50, "Test Pen");
    u8g2.drawStr(90, 50, penIsUp ? "UP" : "DN");
    u8g2.setDrawColor(1);
  }

  // Back row
  {
    bool sel = (cursor == 3);
    if (sel) { u8g2.drawBox(0, 54, 128, 10); u8g2.setDrawColor(0); }
    u8g2.drawStr(4, 62, "< Back");
    u8g2.setDrawColor(1);
  }

  u8g2.sendBuffer();
}

// ── Edit Angle ────────────────────────────────────────────────────────────────
void drawEditAngle() {
  u8g2.clearBuffer();
  const char* title = (editIdx == 0) ? "PEN UP HEIGHT" : "PEN DOWN HEIGHT";
  drawHeader(title);

  // Big number
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", editVal);
  u8g2.setFont(u8g2_font_10x20_tr);
  int tw = u8g2.getStrWidth(buf);
  u8g2.drawStr((128 - tw) / 2, 40, buf);

  // "deg" unit
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr((128 + tw) / 2 + 2, 40, "deg");

  // Progress bar (0–180)
  drawBar(4, 44, 120, 8, editVal, PEN_MAX);

  // Footer hint
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawHLine(0, 54, 128);
  u8g2.drawStr(2, 63, "Turn:adjust   Click:save");

  u8g2.sendBuffer();
}

// ── About ─────────────────────────────────────────────────────────────────────
void drawAbout() {
  u8g2.clearBuffer();

  // Title block
  u8g2.setFont(u8g2_font_6x12_tr);
  const char* title = "PEN PLOTTER";
  int tw = u8g2.getStrWidth(title);
  u8g2.drawStr((128 - tw) / 2, 12, title);
  u8g2.drawHLine(0, 14, 128);

  // Credits
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(4, 26, "Made by:");

  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(4, 40, "Jacob Majors");
  u8g2.drawStr(4, 54, "Troy Pappas");

  // Version + hint
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(92, 63, "v1.2");
  u8g2.drawStr(2, 63, "Click to back");

  u8g2.sendBuffer();
}

// ─────────────────────────────────────────────────────────────────────────────
// SCREEN DISPATCH (draw the active screen)
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen() {
  switch (scr) {
    case MAIN:       drawMain();       break;
    case MANUAL:     drawManual();     break;
    case JOG_X:      drawJog(true);    break;
    case JOG_Y:      drawJog(false);   break;
    case PEN_HEIGHTS:drawPenHeights(); break;
    case EDIT_ANGLE: drawEditAngle();  break;
    case ABOUT:      drawAbout();      break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// INPUT HANDLERS
// ─────────────────────────────────────────────────────────────────────────────
void handleMain(int d, bool clk) {
  if (d) { cursor = constrain(cursor + d, 0, MAIN_N - 1); redraw = true; }
  if (clk) {
    if      (cursor == 0) { scr = MANUAL;      cursor = 0; }
    else if (cursor == 1) { scr = PEN_HEIGHTS; cursor = 0; }
    else if (cursor == 2) { scr = ABOUT; }
    redraw = true;
  }
}

void handleManual(int d, bool clk) {
  if (d) { cursor = constrain(cursor + d, 0, MAN_N - 1); redraw = true; }
  if (clk) {
    switch (cursor) {
      case 0: scr = JOG_X; enX(); break;
      case 1: scr = JOG_Y; enY(); break;
      case 2: doPenUp();   break;
      case 3: doPenDown(); break;
      case 4: scr = MAIN; cursor = 0; disX(); disY(); break;
    }
    redraw = true;
  }
}

void handleJogX(int d, bool clk) {
  if (d) {
    float mm = d * JOG_STEP_MM;
    enX();
    sx.move((long)(mm * X_STEPS_MM));
    posX += mm;
    redraw = true;
  }
  if (clk) {
    scr    = MANUAL;
    cursor = 0;
    redraw = true;
  }
}

void handleJogY(int d, bool clk) {
  if (d) {
    float mm = d * JOG_STEP_MM;
    enY();
    sy.move((long)(mm * Y_STEPS_MM));
    posY += mm;
    redraw = true;
  }
  if (clk) {
    scr    = MANUAL;
    cursor = 0;
    redraw = true;
  }
}

void handlePenHeights(int d, bool clk) {
  if (d) { cursor = constrain(cursor + d, 0, PEN_N - 1); redraw = true; }
  if (clk) {
    switch (cursor) {
      case 0:
        scr = EDIT_ANGLE; editIdx = 0; editVal = penUpAng;
        break;
      case 1:
        scr = EDIT_ANGLE; editIdx = 1; editVal = penDnAng;
        break;
      case 2:
        if (penIsUp) doPenDown(); else doPenUp();
        break;
      case 3:
        scr = MAIN; cursor = 0;
        break;
    }
    redraw = true;
  }
}

void handleEditAngle(int d, bool clk) {
  if (d) {
    editVal = constrain(editVal + d, PEN_MIN, PEN_MAX);
    penServo.write(editVal);   // live preview on servo
    redraw = true;
  }
  if (clk) {
    if (editIdx == 0) penUpAng = (uint8_t)editVal;
    else              penDnAng = (uint8_t)editVal;
    saveSettings();
    beep(3000, 60);
    scr    = PEN_HEIGHTS;
    cursor = editIdx;
    redraw = true;
  }
}

void handleAbout(int d, bool clk) {
  (void)d;
  if (clk) { scr = MAIN; cursor = 0; redraw = true; }
}

// ─────────────────────────────────────────────────────────────────────────────
// G-CODE SERIAL  (Web Serial from the browser, 115200 baud)
// ─────────────────────────────────────────────────────────────────────────────
#define GCODE_BUF  96
char   gcBuf[GCODE_BUF];
uint8_t gcLen    = 0;
bool   absMode   = true;
float  gcX = 0, gcY = 0;     // current absolute position in mm
bool   usbActive = false;
uint32_t lastSerial = 0;

float gcParam(char* line, char p) {
  char* ptr = strchr(line, p);
  if (!ptr) ptr = strchr(line, p + 32);   // try lowercase
  return ptr ? atof(ptr + 1) : NAN;
}

void moveToMM(float tx, float ty) {
  long stX = (long)(tx * X_STEPS_MM);
  long stY = (long)(ty * Y_STEPS_MM);
  digitalWrite(XY_EN_PIN, LOW);
  sx.moveTo(stX);
  sy.moveTo(stY);
  while (sx.distanceToGo() || sy.distanceToGo()) { sx.run(); sy.run(); }
  gcX = tx; gcY = ty;
  posX = tx; posY = ty;
  digitalWrite(XY_EN_PIN, HIGH);
}

void parseGcode(char* raw) {
  // Strip checksum (*xx)
  char* star = strchr(raw, '*');
  if (star) *star = '\0';
  // Skip line number  (Nxxx )
  char* p = raw;
  if (*p == 'N' || *p == 'n') { while (*p && *p != ' ') p++; while (*p == ' ') p++; }

  if (*p == 'G' || *p == 'g') {
    int cmd = atoi(p + 1);
    float x = gcParam(p, 'X'), y = gcParam(p, 'Y'),
          z = gcParam(p, 'Z'), f = gcParam(p, 'F');
    if (!isnan(f)) {
      float spdX = (f / 60.0f) * X_STEPS_MM;
      float spdY = (f / 60.0f) * Y_STEPS_MM;
      sx.setMaxSpeed(max(spdX, 200.0f));
      sy.setMaxSpeed(max(spdY, 200.0f));
    }
    if (cmd == 0 || cmd == 1) {
      float tx = isnan(x) ? gcX : (absMode ? x : gcX + x);
      float ty = isnan(y) ? gcY : (absMode ? y : gcY + y);
      if (!isnan(x) || !isnan(y)) moveToMM(tx, ty);
      if (!isnan(z)) { if (z > 4.0f) doPenUp(); else doPenDown(); redraw = true; }
    } else if (cmd == 28) {           // home: reset position counter, don't move
      gcX = 0; gcY = 0; posX = 0; posY = 0;
      sx.setCurrentPosition(0); sy.setCurrentPosition(0);
      redraw = true;
    } else if (cmd == 90) { absMode = true;  }
      else if (cmd == 91) { absMode = false; }
  } else if (*p == 'M' || *p == 'm') {
    int cmd = atoi(p + 1);
    if (cmd == 115) {
      Serial.println(F("FIRMWARE_NAME:PenPlotter FIRMWARE_VERSION:1.2 MACHINE_TYPE:PenPlotter EXTRUDER_COUNT:0"));
    } else if (cmd == 203) {
      float v = gcParam(p, 'X');
      if (!isnan(v)) { sx.setMaxSpeed((v/60.0f)*X_STEPS_MM); sy.setMaxSpeed((v/60.0f)*Y_STEPS_MM); }
    } else if (cmd == 204) {
      float a = gcParam(p, 'P');
      if (!isnan(a)) { sx.setAcceleration(a * X_STEPS_MM); sy.setAcceleration(a * Y_STEPS_MM); }
    }
    // M110 (line reset), M84 (motor hold), G21 (mm units) — silently accepted
  }
  Serial.println(F("ok"));
}

void processSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    lastSerial = millis();
    if (!usbActive) { usbActive = true; redraw = true; }
    if (c == '\n' || c == '\r') {
      if (gcLen > 0) { gcBuf[gcLen] = '\0'; parseGcode(gcBuf); gcLen = 0; }
    } else if (c != ';' && gcLen < GCODE_BUF - 1) {
      gcBuf[gcLen++] = c;
    }
  }
  if (usbActive && millis() - lastSerial > 8000) { usbActive = false; redraw = true; }
}

// When USB is active, show a simple status screen instead of the menus
void drawUSBScreen() {
  u8g2.clearBuffer();
  drawHeader("USB CONNECTED");
  u8g2.setFont(u8g2_font_5x8_tr);
  char buf[24];
  snprintf(buf, sizeof(buf), "X%+7.2f mm", posX);
  u8g2.drawStr(4, 28, buf);
  snprintf(buf, sizeof(buf), "Y%+7.2f mm", posY);
  u8g2.drawStr(4, 40, buf);
  u8g2.drawStr(4, 52, penIsUp ? "Pen: UP" : "Pen: DOWN");
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(2, 63, "Controlled from browser");
  u8g2.sendBuffer();
}

// ─────────────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  // Stepper enable — shared on v4.2.2, HIGH = disabled (A4988/TMC)
  pinMode(XY_EN_PIN, OUTPUT); digitalWrite(XY_EN_PIN, HIGH);

  // Encoder + button
  pinMode(ENC_A_PIN,   INPUT_PULLUP);
  pinMode(ENC_B_PIN,   INPUT_PULLUP);
  pinMode(ENC_BTN_PIN, INPUT_PULLUP);
  pinMode(BEEPER_PIN,  OUTPUT);

  // Servo
  penServo.attach(PEN_SERVO_PIN);

  // Steppers
  sx.setMaxSpeed(JOG_SPEED); sx.setAcceleration(JOG_ACCEL);
  sy.setMaxSpeed(JOG_SPEED); sy.setAcceleration(JOG_ACCEL);

  // Load saved pen heights
  loadSettings();
  doPenUp();

  // Display
  u8g2.begin();

  // Startup double-beep
  beep(1500, 80); delay(120); beep(2200, 80);

  redraw = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN LOOP
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // Serial G-code from browser (blocking moves happen inside parseGcode)
  processSerial();

  if (usbActive) {
    // USB mode: browser controls motion; encoder can still toggle pen
    pollEnc();
    bool clk = clicked; clicked = false;
    if (clk) { if (penIsUp) doPenDown(); else doPenUp(); redraw = true; }
    if (redraw) { redraw = false; drawUSBScreen(); }
    return;
  }

  // Standalone knob-menu mode
  pollEnc();

  int  d   = getDelta();
  bool clk = clicked; clicked = false;

  sx.run();
  sy.run();
  if (sx.distanceToGo() == 0) disX();
  if (sy.distanceToGo() == 0) disY();

  if (d || clk) {
    switch (scr) {
      case MAIN:        handleMain(d, clk);       break;
      case MANUAL:      handleManual(d, clk);     break;
      case JOG_X:       handleJogX(d, clk);       break;
      case JOG_Y:       handleJogY(d, clk);       break;
      case PEN_HEIGHTS: handlePenHeights(d, clk); break;
      case EDIT_ANGLE:  handleEditAngle(d, clk);  break;
      case ABOUT:       handleAbout(d, clk);      break;
    }
  }

  if (redraw) {
    redraw = false;
    drawScreen();
  }
}
