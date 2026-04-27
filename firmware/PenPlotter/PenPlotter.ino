/**
 * PenPlotter Firmware  v3.0
 * Jacob Majors & Troy Pappas
 *
 * Board  : Creality Ender-3 v4.2.2  (STM32F103RCT6)
 * Display: CR10 stock — 128×64 ST7920 + rotary encoder
 * Motors : X  (GT2 belt, PC2/PB9)
 *          Y  (GT2 belt, PB6/PB5 — old Z axis, axis-flipped)
 * Drivers: TMC2208 @ 16× microstepping, shared enable PC3
 * Pen    : Servo on PB0 (repurposed BLTouch pin)
 * SD card: onboard SDIO (D0=PC8 D1=PC9 D2=PC10 D3=PC11 CK=PC12 CMD=PD2)
 *          Detect: PC7
 *
 * Two modes:
 *   USB   — browser sends G-code over Web Serial; LCD shows live position
 *   Local — browse + run .gcode files stored on SD card; no laptop needed
 *
 * Steps/mm (both axes, GT2 20T pulley, TMC2208 16× step):
 *   (200 × 16) / (20 × 2) = 80 steps/mm
 *
 * Critical: G-code XY moves use MultiStepper so both axes arrive together
 *           → perfectly straight diagonal lines on paper.
 */

#include <SPI.h>
#include <U8g2lib.h>
#include <AccelStepper.h>
#include <MultiStepper.h>
#include <Servo.h>
#include <EEPROM.h>
// SD card — uses SdFat in SPI-compat mode via SDIO pins on v4.2.2:
//   CS   = PC11 (SDIO_D3)   SCK  = PC12 (SDIO_CK)
//   MOSI = PD2  (SDIO_CMD)  MISO = PC8  (SDIO_D0)
// SdFat handles 1-bit SPI negotiation; no HAL/FatFs dependency needed.
#define SD_ENABLED
#include <SdFat.h>

// SPI configuration for SD card on the onboard slot
#define SD_CS_PIN   PC11
#define SD_SCK_PIN  PC12
#define SD_MOSI_PIN PD2
#define SD_MISO_PIN PC8
SPIClass SdSpi(SD_MOSI_PIN, SD_MISO_PIN, SD_SCK_PIN);
SdFat32  SD_FS;

// ── Pins ─────────────────────────────────────────────────────────────────────
#define XY_EN    PC3
#define X_STEP   PC2
#define X_DIR    PB9
#define Y_STEP   PB6   // old Z_STEP
#define Y_DIR    PB5   // old Z_DIR
#define PEN_PIN  PB0   // servo (repurposed BLTouch)
#define SD_DETECT PC7  // SD card detect (LOW = inserted)
#define ENC_A    PB10
#define ENC_B    PB14
#define ENC_BTN  PB2
#define LCD_CLK  PB13
#define LCD_DATA PB15
#define LCD_CS   PB12
#define BUZZ     PC6

// ── Motion ───────────────────────────────────────────────────────────────────
#define X_SPM         80.0f   // steps / mm — both axes (GT2 belt, 20T, 16× step)
#define Y_SPM         80.0f
#define SPEED_DRAW  3200.0f   // 40 mm/s — pen on paper (conservative for ink)
#define SPEED_TRAVEL 9600.0f  // 120 mm/s — pen lifted
#define SPEED_JOG    3200.0f  // 40 mm/s — manual jog
#define ACCEL_JOG    9600.0f  // steps/s² — for individual jog moves
#define PEN_Z_UP     4.0f     // Z > this → pen up (web app: penUpZ=8, penDownZ=2)

// ── Pen servo ─────────────────────────────────────────────────────────────────
#define PEN_UP_DEF  90
#define PEN_DN_DEF  45
#define PEN_MIN      0
#define PEN_MAX    180

// ── EEPROM ────────────────────────────────────────────────────────────────────
#define EE_MAGIC   0
#define EE_UP      1
#define EE_DN      2
#define MAGIC    0xC2   // bump this to reset saved values

// ── Hardware ──────────────────────────────────────────────────────────────────
U8G2_ST7920_128X64_F_SW_SPI lcd(U8G2_R0, LCD_CLK, LCD_DATA, LCD_CS);
AccelStepper sx(AccelStepper::DRIVER, X_STEP, X_DIR);
AccelStepper sy(AccelStepper::DRIVER, Y_STEP, Y_DIR);
MultiStepper  xy;     // coordinates both axes simultaneously
Servo         pen;

// ── Screens ───────────────────────────────────────────────────────────────────
enum Screen {
  SCR_MAIN, SCR_MANUAL, SCR_JOG_X, SCR_JOG_Y,
  SCR_PEN, SCR_EDIT_ANGLE, SCR_FILES, SCR_PRINTING, SCR_ABOUT, SCR_USB
};
Screen  scr    = SCR_MAIN;
int8_t  cur    = 0;
bool    redraw = true;

// ── State ─────────────────────────────────────────────────────────────────────
uint8_t penUp  = PEN_UP_DEF;
uint8_t penDn  = PEN_DN_DEF;
bool    penRaised = true;
float   posX = 0, posY = 0;
uint8_t editIdx = 0;
int16_t editVal = 0;

// ── SD / file playback ────────────────────────────────────────────────────────
#define  MAX_FILES  32
#define  FNAME_LEN  20
bool     sdOk      = false;
bool     sdChecked = false;
char     fileList[MAX_FILES][FNAME_LEN];
uint8_t  fileCount = 0;
bool     printing    = false;
bool     printPaused = false;
File32   printFile;
uint32_t printTotal  = 0;
uint32_t printDone   = 0;
char     printName[FNAME_LEN] = "";

// ── SD write mode (M28/M29 — receive G-code from browser, save to SD) ────────
bool     sdWriteMode = false;
File32   sdWriteFile;
char     sdWriteName[FNAME_LEN] = "";

// ── Encoder ───────────────────────────────────────────────────────────────────
bool     encA_last = HIGH, btn_last = HIGH;
uint32_t btn_time  = 0;
int      enc_acc   = 0;
bool     clicked   = false;

// ── G-code serial (USB / browser) ────────────────────────────────────────────
#define  GBUF 96
char     gcBuf[GBUF];
uint8_t  gcLen     = 0;
bool     absMode   = true;
float    gcX = 0, gcY = 0;
float    gcFeed    = SPEED_DRAW;
bool     usbActive = false;
uint32_t lastSerial= 0;

// ═════════════════════════════════════════════════════════════════════════════
// EEPROM + settings
// ═════════════════════════════════════════════════════════════════════════════
void loadSettings() {
  if (EEPROM.read(EE_MAGIC) == MAGIC) {
    penUp = EEPROM.read(EE_UP);
    penDn = EEPROM.read(EE_DN);
  } else {
    penUp = PEN_UP_DEF; penDn = PEN_DN_DEF;
    EEPROM.write(EE_MAGIC, MAGIC);
    EEPROM.write(EE_UP,    penUp);
    EEPROM.write(EE_DN,    penDn);
  }
}
void saveSettings() {
  EEPROM.write(EE_UP, penUp);
  EEPROM.write(EE_DN, penDn);
}

// ═════════════════════════════════════════════════════════════════════════════
// Hardware helpers
// ═════════════════════════════════════════════════════════════════════════════
void motorsOn()  { digitalWrite(XY_EN, LOW);  }
void motorsOff() { digitalWrite(XY_EN, HIGH); }
void penUp_()    { pen.write(penUp); penRaised = true;  }
void penDown_()  { pen.write(penDn); penRaised = false; }
void beep(uint16_t f=2000, uint16_t ms=20) { tone(BUZZ, f, ms); }

// ── Encoder ───────────────────────────────────────────────────────────────────
void pollEnc() {
  bool a = digitalRead(ENC_A);
  if (a != encA_last) {
    if (a == LOW) enc_acc += (digitalRead(ENC_B) == HIGH) ? 1 : -1;
    encA_last = a;
  }
  bool b = digitalRead(ENC_BTN);
  uint32_t now = millis();
  if (b != btn_last && (now - btn_time) > 50) {
    btn_time = now; btn_last = b;
    if (b == LOW) { clicked = true; beep(); }
  }
}
int getDelta() {
  static int r = 0;
  r += enc_acc; enc_acc = 0;
  int d = r / 2; r -= d * 2;
  return d;
}

// ═════════════════════════════════════════════════════════════════════════════
// SD card
// ═════════════════════════════════════════════════════════════════════════════
bool initSD() {
  SdSpi.begin();
  SdSpiConfig cfg(SD_CS_PIN, DEDICATED_SPI, SD_SCK_MHZ(4), &SdSpi);
  return SD_FS.begin(cfg);
}

void scanFiles() {
  fileCount = 0;
  if (!sdOk) return;
  File32 root;
  if (!root.open("/")) return;
  File32 f;
  while (f.openNext(&root, O_RDONLY) && fileCount < MAX_FILES) {
    if (!f.isDir()) {
      char n[FNAME_LEN]; f.getName(n, FNAME_LEN);
      size_t len = strlen(n);
      if ((len > 6 && strcasecmp(n + len - 6, ".gcode") == 0) ||
          (len > 3 && strcasecmp(n + len - 3, ".gc")    == 0)) {
        strncpy(fileList[fileCount], n, FNAME_LEN - 1);
        fileList[fileCount][FNAME_LEN - 1] = '\0';
        fileCount++;
      }
    }
    f.close();
  }
  root.close();
}

// ═════════════════════════════════════════════════════════════════════════════
// Motion — the key function for straight-line plotting
// ═════════════════════════════════════════════════════════════════════════════
// MultiStepper ensures X and Y arrive simultaneously → straight diagonal lines.
// Without this, AccelStepper runs each axis at full speed independently,
// making all diagonal moves into curves.
void moveMM(float tx, float ty) {
  long pos[2] = {
    (long)roundf(tx * X_SPM),
    (long)roundf(ty * Y_SPM)
  };
  sx.setMaxSpeed(gcFeed);
  sy.setMaxSpeed(gcFeed);
  motorsOn();
  xy.moveTo(pos);
  while (xy.run()) { /* both axes step in lockstep */ }
  gcX = tx; gcY = ty;
  posX = tx; posY = ty;
}

// ═════════════════════════════════════════════════════════════════════════════
// G-code parser — used for both USB stream and SD file playback
// ═════════════════════════════════════════════════════════════════════════════
float param(char* line, char p) {
  char* ptr = strchr(line, p);
  if (!ptr) ptr = strchr(line, p + 32); // lowercase fallback
  return ptr ? atof(ptr + 1) : NAN;
}

// Draw "SAVING..." on LCD during M28 upload
void drawSavingScreen() {
  lcd.clearBuffer(); hdr("SAVING TO SD");
  lcd.setFont(u8g2_font_5x8_tr);
  lcd.drawStr(4, 28, sdWriteName);
  lcd.drawStr(4, 42, "Receiving from USB...");
  lcd.drawStr(4, 56, "Do not disconnect.");
  lcd.sendBuffer();
}

void execGcode(char* raw) {
  char* star = strchr(raw, '*'); if (star) *star = '\0';
  char* p = raw;
  if (*p == 'N' || *p == 'n') { while (*p && *p != ' ') p++; while (*p == ' ') p++; }
  if (!*p) { Serial.println(F("ok")); return; }

  // ── SD write mode: funnel all lines to file until M29 ─────────────────────
  if (sdWriteMode) {
    // Check if this is M29 (stop writing)
    bool isM29 = ((*p == 'M' || *p == 'm') && atoi(p + 1) == 29);
    if (!isM29) {
      if (sdOk && sdWriteFile) sdWriteFile.println(raw);
      Serial.println(F("ok"));
      return;
    }
    // M29 falls through below to close the file
  }

  if (*p == 'G' || *p == 'g') {
    int   cmd = atoi(p + 1);
    float x   = param(p,'X'), y = param(p,'Y');
    float z   = param(p,'Z'), f = param(p,'F');

    if (!isnan(f)) gcFeed = constrain((f / 60.0f) * X_SPM, 200.0f, SPEED_TRAVEL);

    if (cmd == 0) {
      float sf = gcFeed; gcFeed = SPEED_TRAVEL;
      float tx = isnan(x) ? gcX : (absMode ? x : gcX+x);
      float ty = isnan(y) ? gcY : (absMode ? y : gcY+y);
      if (!isnan(x)||!isnan(y)) moveMM(tx, ty);
      if (!isnan(z)) { if (z > PEN_Z_UP) penUp_(); else penDown_(); }
      gcFeed = sf;
    } else if (cmd == 1) {
      float tx = isnan(x) ? gcX : (absMode ? x : gcX+x);
      float ty = isnan(y) ? gcY : (absMode ? y : gcY+y);
      if (!isnan(x)||!isnan(y)) moveMM(tx, ty);
      if (!isnan(z)) { if (z > PEN_Z_UP) penUp_(); else penDown_(); }
    } else if (cmd == 4) {
      // G4 Pms — dwell
      float ms = param(p,'P');
      if (!isnan(ms)) delay((uint32_t)ms);
    } else if (cmd == 28) {
      gcX=0; gcY=0; posX=0; posY=0;
      sx.setCurrentPosition(0); sy.setCurrentPosition(0);
      redraw = true;
    } else if (cmd == 90) { absMode = true;  }
      else if (cmd == 91) { absMode = false; }

  } else if (*p == 'M' || *p == 'm') {
    int cmd = atoi(p + 1);
    if (cmd == 28) {
      // M28 <filename> — open SD file for writing (browser upload)
      char* fname = p + 3; while (*fname == ' ') fname++;
      // Strip trailing whitespace
      int fl = strlen(fname);
      while (fl > 0 && (fname[fl-1] == ' ' || fname[fl-1] == '\r')) fname[--fl] = '\0';
      if (!sdOk) { Serial.println(F("Error:No SD card")); return; }
      if (sdWriteFile) sdWriteFile.close();
      sdWriteFile.open(fname, O_WRONLY | O_CREAT | O_TRUNC);
      if (sdWriteFile) {
        strncpy(sdWriteName, fname, FNAME_LEN - 1);
        sdWriteMode = true;
        drawSavingScreen();
        Serial.print(F("Writing:")); Serial.println(fname);
      } else {
        Serial.println(F("Error:SD open failed"));
        return;
      }
    } else if (cmd == 29) {
      // M29 — close file, refresh list
      if (sdWriteMode && sdWriteFile) {
        sdWriteFile.close();
        sdWriteMode = false;
        scanFiles();
        beep(2000, 100); delay(80); beep(2500, 100);
        scr = SCR_FILES; cur = 0; redraw = true;
        Serial.print(F("Saved:")); Serial.println(sdWriteName);
      }
    } else if (cmd == 84)  { motorsOff();
    } else if (cmd == 115) {
      Serial.println(F("FIRMWARE_NAME:PenPlotter FIRMWARE_VERSION:3.0 "
                       "MACHINE_TYPE:PenPlotter EXTRUDER_COUNT:0"));
    } else if (cmd == 203) { float v=param(p,'X'); if(!isnan(v)) gcFeed=constrain(v*X_SPM,200.f,SPEED_TRAVEL);
    } else if (cmd == 204) { float a=param(p,'P'); if(!isnan(a)){sx.setAcceleration(a*X_SPM);sy.setAcceleration(a*Y_SPM);}
    }
  }
  Serial.println(F("ok"));
}

// ═════════════════════════════════════════════════════════════════════════════
// USB serial
// ═════════════════════════════════════════════════════════════════════════════
void processSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    lastSerial = millis();
    if (!usbActive) { usbActive = true; scr = SCR_USB; redraw = true; }
    if (c == '\n' || c == '\r') {
      if (gcLen) { gcBuf[gcLen] = '\0'; execGcode(gcBuf); gcLen = 0; }
    } else if (c != ';' && gcLen < GBUF - 1) {
      gcBuf[gcLen++] = c;
    }
  }
  if (usbActive && millis() - lastSerial > 10000) {
    usbActive = false; scr = SCR_MAIN; redraw = true;
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// SD file playback — one G-code line per loop call (non-blocking)
#define LINE_BUF 96
bool tickPrint() {
  if (!printing || printPaused) return printing;
  if (!printFile) { printing = false; return false; }

  char line[LINE_BUF]; uint8_t len = 0;
  while (printFile.available()) {
    char c = (char)printFile.read();
    printDone++;
    if (c == '\n' || c == '\r') { if (len) break; }
    else if (c != ';' && len < LINE_BUF - 1) line[len++] = c;
  }

  if (!printFile.available() && len == 0) {
    printFile.close(); printing = false;
    penUp_(); motorsOff();
    beep(2000,200); delay(100); beep(2500,200);
    scr = SCR_FILES; cur = 0; redraw = true;
    return false;
  }
  if (len) { line[len] = '\0'; execGcode(line); redraw = true; }
  return true;
}

void startPrint(uint8_t idx) {
  if (idx >= fileCount || !sdOk) return;
  printFile.open(fileList[idx], O_RDONLY);
  if (!printFile) return;
  printTotal = printFile.size(); printDone = 0;
  strncpy(printName, fileList[idx], FNAME_LEN - 1);
  printing = true; printPaused = false;
  gcX = 0; gcY = 0; absMode = true; gcFeed = SPEED_DRAW;
  scr = SCR_PRINTING; cur = 0; redraw = true;
}

void stopPrint() {
  if (printFile) printFile.close();
  printing = false; printPaused = false;
  penUp_(); motorsOff();
  scr = SCR_FILES; cur = 0; redraw = true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Display
// ═════════════════════════════════════════════════════════════════════════════
void hdr(const char* t) {
  lcd.setFont(u8g2_font_6x12_tr);
  lcd.drawStr(2, 10, t);
  lcd.drawHLine(0, 13, 128);
}
void bar(int x, int y, int w, int h, int v, int mx) {
  lcd.drawFrame(x,y,w,h);
  int f = constrain((int)((float)v/mx*(w-2)),0,w-2);
  if (f) lcd.drawBox(x+1,y+1,f,h-2);
}
void list(const char** items, int n, int sel) {
  int start = constrain(sel-1, 0, max(0,n-4));
  lcd.setFont(u8g2_font_5x8_tr);
  for (int i=0; i<4&&(start+i)<n; i++) {
    int idx=start+i, y=16+i*12;
    if (idx==sel){lcd.drawBox(0,y,128,12);lcd.setDrawColor(0);}
    lcd.drawStr(4,y+9,items[idx]);
    lcd.setDrawColor(1);
  }
}

void scrMain() {
  static const char* items[] = {"Manual Move","Pen Heights","Files","About"};
  lcd.clearBuffer(); hdr("PEN PLOTTER");
  list(items, 4, cur);
  lcd.setFont(u8g2_font_5x8_tr);
  char buf[24];
  snprintf(buf,sizeof(buf),"X%+5.1f Y%+5.1f %s",posX,posY,penRaised?"UP":"DN");
  lcd.drawStr(0,63,buf);
  lcd.sendBuffer();
}

void scrManual() {
  static const char* items[] = {"Jog X","Jog Y","Pen Up","Pen Down","< Back"};
  lcd.clearBuffer(); hdr("MANUAL MOVE");
  lcd.setFont(u8g2_font_5x8_tr);
  char buf[28];
  snprintf(buf,sizeof(buf),"X%+6.1fmm  Y%+6.1fmm",posX,posY);
  lcd.drawStr(2,24,buf); lcd.drawHLine(0,26,128);
  int start=constrain(cur-1,0,max(0,5-3));
  for (int i=0;i<3&&(start+i)<5;i++) {
    int idx=start+i,y=28+i*12;
    if(idx==cur){lcd.drawBox(0,y,128,12);lcd.setDrawColor(0);}
    lcd.drawStr(4,y+9,items[idx]);
    if(idx==2||idx==3) lcd.drawStr(100,y+9,penRaised?"[UP]":"[DN]");
    lcd.setDrawColor(1);
  }
  lcd.sendBuffer();
}

void scrJog(bool isX) {
  lcd.clearBuffer(); hdr(isX?"JOG  X  AXIS":"JOG  Y  AXIS");
  char buf[16]; snprintf(buf,sizeof(buf),"%+.1f mm",isX?posX:posY);
  lcd.setFont(u8g2_font_10x20_tr);
  lcd.drawStr((128-lcd.getStrWidth(buf))/2,40,buf);
  lcd.setFont(u8g2_font_5x8_tr);
  lcd.drawStr(90,11,isX?"< X >":"< Y >");
  lcd.drawHLine(0,50,128);
  lcd.drawStr(2,62,"Turn:move   Click:done");
  lcd.sendBuffer();
}

void scrPen() {
  lcd.clearBuffer(); hdr("PEN HEIGHTS");
  lcd.setFont(u8g2_font_5x8_tr);
  auto row=[&](int y,const char* l,int v,bool sel){
    if(sel){lcd.drawBox(0,y,128,12);lcd.setDrawColor(0);}
    lcd.drawStr(4,y+9,l);
    char b[5]; snprintf(b,sizeof(b),"%3d",v);
    lcd.drawStr(72,y+9,b); bar(84,y+1,40,8,v,PEN_MAX);
    lcd.setDrawColor(1);
  };
  row(15,"Up Height",  penUp,cur==0);
  row(28,"Down Height",penDn,cur==1);
  {bool s=cur==2;if(s){lcd.drawBox(0,41,128,12);lcd.setDrawColor(0);}
   lcd.drawStr(4,50,"Test Pen");lcd.drawStr(90,50,penRaised?"UP":"DN");lcd.setDrawColor(1);}
  {bool s=cur==3;if(s){lcd.drawBox(0,54,128,10);lcd.setDrawColor(0);}
   lcd.drawStr(4,62,"< Back");lcd.setDrawColor(1);}
  lcd.sendBuffer();
}

void scrEditAngle() {
  lcd.clearBuffer();
  hdr(editIdx==0?"PEN UP HEIGHT":"PEN DOWN HEIGHT");
  char buf[8]; snprintf(buf,sizeof(buf),"%d",editVal);
  lcd.setFont(u8g2_font_10x20_tr);
  int tw=lcd.getStrWidth(buf);
  lcd.drawStr((128-tw)/2,40,buf);
  lcd.setFont(u8g2_font_6x12_tr);
  lcd.drawStr((128+tw)/2+2,40,"deg");
  bar(4,44,120,8,editVal,PEN_MAX);
  lcd.setFont(u8g2_font_5x8_tr);
  lcd.drawHLine(0,54,128); lcd.drawStr(2,63,"Turn:adjust  Click:save");
  lcd.sendBuffer();
}

void scrFiles() {
  lcd.clearBuffer(); hdr("FILES  (SD CARD)");
  lcd.setFont(u8g2_font_5x8_tr);
  if (!sdOk) {
    lcd.drawStr(4,24,"No SD card detected.");
    lcd.drawStr(4,36,"Insert card, go back,");
    lcd.drawStr(4,48,"then re-open Files.");
  } else if (fileCount == 0) {
    lcd.drawStr(4,32,"No .gcode files.");
    lcd.drawStr(4,44,"Copy files to SD root.");
  } else {
    // Show up to 4 files, cur selects which
    int start = constrain(cur, 0, max(0,(int)fileCount-4));
    for (int i=0;i<4&&(start+i)<fileCount;i++) {
      int y=16+i*12, idx=start+i;
      if(idx==cur){lcd.drawBox(0,y,128,12);lcd.setDrawColor(0);}
      // Truncate filename to fit
      char trunc[19]; strncpy(trunc,fileList[idx],18); trunc[18]='\0';
      lcd.drawStr(4,y+9,trunc);
      lcd.setDrawColor(1);
    }
    char buf[16];
    snprintf(buf,sizeof(buf),"%d files  Click:run",fileCount);
    lcd.drawStr(0,63,buf);
  }
  lcd.sendBuffer();
}

void scrPrinting() {
  lcd.clearBuffer(); hdr(printPaused?"PAUSED":"PRINTING");
  lcd.setFont(u8g2_font_5x8_tr);
  // Filename (truncated)
  char trunc[19]; strncpy(trunc,printName,18); trunc[18]='\0';
  lcd.drawStr(2,24,trunc);
  // Progress bar
  int pct = printTotal>0 ? (int)(printDone*100/printTotal) : 0;
  bar(2,27,124,8,pct,100);
  char pbuf[8]; snprintf(pbuf,sizeof(pbuf),"%d%%",pct);
  lcd.drawStr(56,40,pbuf);
  // Position
  char pos[24];
  snprintf(pos,sizeof(pos),"X%+5.1f Y%+5.1f",posX,posY);
  lcd.drawStr(2,52,pos);
  // Controls
  lcd.drawStr(2,63,printPaused?"Clk:resume  Hold:stop":"Clk:pause   Hold:stop");
  lcd.sendBuffer();
}

void scrAbout() {
  lcd.clearBuffer();
  lcd.setFont(u8g2_font_6x12_tr);
  const char* t="PEN PLOTTER";
  lcd.drawStr((128-lcd.getStrWidth(t))/2,12,t);
  lcd.drawHLine(0,14,128);
  lcd.setFont(u8g2_font_5x8_tr); lcd.drawStr(4,26,"Made by:");
  lcd.setFont(u8g2_font_6x12_tr);
  lcd.drawStr(4,40,"Jacob Majors");
  lcd.drawStr(4,54,"Troy Pappas");
  lcd.setFont(u8g2_font_5x8_tr);
  lcd.drawStr(92,63,"v3.0"); lcd.drawStr(2,63,"Click:back");
  lcd.sendBuffer();
}

void scrUSB() {
  lcd.clearBuffer(); hdr("USB  CONNECTED");
  lcd.setFont(u8g2_font_5x8_tr);
  char buf[24];
  snprintf(buf,sizeof(buf),"X %+.2f mm",posX); lcd.drawStr(4,28,buf);
  snprintf(buf,sizeof(buf),"Y %+.2f mm",posY); lcd.drawStr(4,40,buf);
  lcd.drawStr(4,52,penRaised?"Pen: UP":"Pen: DOWN");
  lcd.drawStr(2,63,"Click:toggle pen");
  lcd.sendBuffer();
}

void drawScreen() {
  switch(scr) {
    case SCR_MAIN:       scrMain();       break;
    case SCR_MANUAL:     scrManual();     break;
    case SCR_JOG_X:      scrJog(true);   break;
    case SCR_JOG_Y:      scrJog(false);  break;
    case SCR_PEN:        scrPen();       break;
    case SCR_EDIT_ANGLE: scrEditAngle(); break;
    case SCR_FILES:      scrFiles();     break;
    case SCR_PRINTING:   scrPrinting();  break;
    case SCR_ABOUT:      scrAbout();     break;
    case SCR_USB:        scrUSB();       break;
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Input handlers
// ═════════════════════════════════════════════════════════════════════════════
void handleMain(int d, bool clk) {
  if(d){cur=constrain(cur+d,0,3);redraw=true;}
  if(clk){
    switch(cur){
      case 0: scr=SCR_MANUAL;cur=0;break;
      case 1: scr=SCR_PEN;   cur=0;break;
      case 2:
        // Re-check SD card each time Files is opened
        if(!sdChecked||!sdOk){sdOk=initSD();sdChecked=true;}
        if(sdOk) scanFiles();
        scr=SCR_FILES; cur=0; break;
      case 3: scr=SCR_ABOUT; break;
    }
    redraw=true;
  }
}

void handleManual(int d, bool clk) {
  if(d){cur=constrain(cur+d,0,4);redraw=true;}
  if(clk){
    switch(cur){
      case 0: scr=SCR_JOG_X; motorsOn(); break;
      case 1: scr=SCR_JOG_Y; motorsOn(); break;
      case 2: penUp_();       break;
      case 3: penDown_();     break;
      case 4: scr=SCR_MAIN;cur=0;motorsOff();break;
    }
    redraw=true;
  }
}

void handleJog(bool isX, int d, bool clk) {
  if(d){
    float mm=d*0.5f;
    motorsOn();
    if(isX){ sx.setMaxSpeed(SPEED_JOG); sx.move((long)(mm*X_SPM)); posX+=mm; }
    else    { sy.setMaxSpeed(SPEED_JOG); sy.move((long)(mm*Y_SPM)); posY+=mm; }
    redraw=true;
  }
  if(clk){scr=SCR_MANUAL;cur=0;redraw=true;}
}

void handlePen(int d, bool clk) {
  if(d){cur=constrain(cur+d,0,3);redraw=true;}
  if(clk){
    switch(cur){
      case 0: scr=SCR_EDIT_ANGLE;editIdx=0;editVal=penUp;break;
      case 1: scr=SCR_EDIT_ANGLE;editIdx=1;editVal=penDn;break;
      case 2: if(penRaised)penDown_();else penUp_();break;
      case 3: scr=SCR_MAIN;cur=0;break;
    }
    redraw=true;
  }
}

void handleEditAngle(int d, bool clk) {
  if(d){editVal=constrain(editVal+d,PEN_MIN,PEN_MAX);pen.write(editVal);redraw=true;}
  if(clk){
    if(editIdx==0)penUp=(uint8_t)editVal;else penDn=(uint8_t)editVal;
    saveSettings(); beep(3000,60);
    scr=SCR_PEN;cur=editIdx;redraw=true;
  }
}

void handleFiles(int d, bool clk) {
  if(!sdOk||fileCount==0){if(clk){scr=SCR_MAIN;cur=0;redraw=true;}return;}
  if(d){cur=constrain(cur+d,0,(int)fileCount-1);redraw=true;}
  if(clk){startPrint(cur);}
}

void handleAbout(int d, bool clk) {
  (void)d;
  if (clk) { scr = SCR_MAIN; cur = 0; redraw = true; }
}

// Hold-to-stop during printing: track long press
static uint32_t holdStart = 0;
static bool     holdActive = false;

void handlePrinting(int d, bool clk) {
  (void)d;
  if(clk){
    printPaused=!printPaused;
    redraw=true;
  }
}

void checkHoldStop() {
  // If button held > 1s during printing → stop
  bool btnDown = (digitalRead(ENC_BTN) == LOW);
  if(btnDown && !holdActive){ holdStart=millis(); holdActive=true; }
  if(!btnDown){ holdActive=false; }
  if(holdActive && millis()-holdStart>1000 && printing){
    stopPrint(); holdActive=false;
    beep(1000,300);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  pinMode(XY_EN,  OUTPUT); motorsOff();
  pinMode(SD_DETECT, INPUT_PULLUP);
  pinMode(ENC_A,  INPUT_PULLUP);
  pinMode(ENC_B,  INPUT_PULLUP);
  pinMode(ENC_BTN,INPUT_PULLUP);
  pinMode(BUZZ,   OUTPUT);

  pen.attach(PEN_PIN);
  loadSettings();
  penUp_();

  // Steppers (jog only — G-code moves use MultiStepper xy)
  sx.setMaxSpeed(SPEED_JOG); sx.setAcceleration(ACCEL_JOG);
  sy.setMaxSpeed(SPEED_JOG); sy.setAcceleration(ACCEL_JOG);
  xy.addStepper(sx);
  xy.addStepper(sy);

  // Try SD card at boot (don't block if absent)
  sdOk = initSD();
  sdChecked = true;
  if(sdOk) scanFiles();

  lcd.begin();
  beep(1500,80); delay(100); beep(2200,80);
  redraw = true;
}

// ═════════════════════════════════════════════════════════════════════════════
// LOOP
// ═════════════════════════════════════════════════════════════════════════════
void loop() {
  processSerial();

  // USB mode takes priority
  if(usbActive){
    pollEnc();
    bool clk=clicked; clicked=false;
    if(clk){if(penRaised)penDown_();else penUp_();redraw=true;}
    if(redraw){redraw=false;scrUSB();}
    return;
  }

  // SD file playback (non-blocking: one G-code line per loop iteration)
  if(printing && !printPaused){
    checkHoldStop();
    tickPrint();
    if(redraw){redraw=false;scrPrinting();}
    return;
  }
  if(printing && printPaused){ checkHoldStop(); }

  pollEnc();
  int  d   = getDelta();
  bool clk = clicked; clicked=false;

  // Run individual-axis jog moves
  bool moving = sx.distanceToGo() || sy.distanceToGo();
  if(moving){ sx.run(); sy.run(); } else { motorsOff(); }

  if(d || clk){
    switch(scr){
      case SCR_MAIN:       handleMain(d,clk);           break;
      case SCR_MANUAL:     handleManual(d,clk);         break;
      case SCR_JOG_X:      handleJog(true,d,clk);       break;
      case SCR_JOG_Y:      handleJog(false,d,clk);      break;
      case SCR_PEN:        handlePen(d,clk);             break;
      case SCR_EDIT_ANGLE: handleEditAngle(d,clk);      break;
      case SCR_FILES:      handleFiles(d,clk);           break;
      case SCR_PRINTING:   handlePrinting(d,clk);       break;
      case SCR_ABOUT:      handleAbout(d,clk);           break;
      default: break;
    }
  }

  if(redraw && !moving){redraw=false;drawScreen();}
}
