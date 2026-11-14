// KORAIL 연결기 높이 측정기 — 이중 레이저 삼각측량
// 밑변(궤간) + 두 레이저 거리(SSS)로 회전축(꼭짓점) 높이를 계산해 OLED에 표시한다.
// 센서: JRT C5B/RX 계열 ×2 (UART), OLED: SSD1309 128x64 (I2C), MCU: ESP32 DevKitC
// 원리: docs/measurement-principle.md, 배선: docs/hardware.md, 보정: docs/calibration.md

#include <Wire.h>
#include <U8g2lib.h>

// ================= 하드웨어 =================
#define S1_RX 16
#define S1_TX 17
#define S2_RX 25
#define S2_TX 26
#define SENSOR_BAUD 9600

#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C

// ================= 측정 / 표시 =================
#define EMA_ALPHA    0.8f     // 1.0 = 필터 없음
#define JUMP_SNAP_MM 50.0f    // 이만큼 튀면 필터 무시하고 즉시 반영
#define STALE_MS     400      // 이 시간 넘게 새 값 없으면 '과거 데이터'
#define DISPLAY_HZ   10
#define FRAME_LEN    12
#define DIST_SCALE   10.0f    // 거리 보정 배율
#define RAW_DEBUG    0        // 1 = 원시 바이트 덤프

// ================= 센서 시작 =================
#define BOOT_DELAY_MS 1000    // 전원 켜고 센서 부팅 대기
#define START_GAP_MS  300     // 두 센서 명령 사이 간격
#define RESTART_MS    2000    // 이 시간 응답 없으면 재시작

// ================= 삼각측량 =================
#define BASE_MM      1435.0f  // 밑변: 두 레이저 점 사이 거리
#define L_OFFSET_MM  0.0f     // 좌 레이저 0점 보정 (측정값에 더함)
#define R_OFFSET_MM  0.0f     // 우 레이저 0점 보정 (측정값에 더함)
#define H_GAIN       1.0f     // 최종 높이 보정: 기울기  (tools/calibrate.py 결과)
#define H_OFFSET_MM  0.0f     // 최종 높이 보정: 절편 (mm)

const uint8_t CMD_CONT[] = {0xAA,0x00,0x00,0x21,0x00,0x01,0x00,0x00,0x22};

U8G2_SSD1309_128X64_NONAME0_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

struct Sensor {
  HardwareSerial *port;
  const char *tag;
  uint8_t  buf[FRAME_LEN];
  int      idx;
  float    mm, filt;
  bool     valid, err;
  uint32_t samples, rateCount, lastRx, lastGood;
  float    rateHz;
};

// S2(Serial1, 25/26) = 좌(L), S1(Serial2, 16/17) = 우(R)
Sensor sL = {&Serial1, "L"};
Sensor sR = {&Serial2, "R"};

struct Tri {
  bool  ok;   // 삼각형 성립
  float h;    // 꼭짓점 높이 (mm)
  float y;    // 꼭짓점이 밑변 중앙에서 좌우로 벗어난 양 (+ = R쪽)
};

// ================= 센서 수신 =================
static inline uint8_t bcd(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

void decode(Sensor &s) {
  s.lastRx = millis();

  if (s.buf[6] == 0xEE) { s.err = true; return; }
  s.err = false;

  float raw = bcd(s.buf[6]) * 1000.0f + bcd(s.buf[7]) * 100.0f +
              bcd(s.buf[8]) * 10.0f   + bcd(s.buf[9]) * 0.1f;
  s.mm = raw * DIST_SCALE;

  if (!s.valid || fabsf(s.mm - s.filt) > JUMP_SNAP_MM) s.filt = s.mm;
  else s.filt += EMA_ALPHA * (s.mm - s.filt);

  s.valid = true;
  s.lastGood = millis();
  s.samples++;
  s.rateCount++;
}

void feed(Sensor &s, uint8_t b) {
  if (s.idx == 0 && b != 0xAA) return;
  s.buf[s.idx++] = b;
#if RAW_DEBUG
  Serial.printf("%02X ", b);
#endif
  if (s.idx < FRAME_LEN) return;
  s.idx = 0;
  decode(s);
#if RAW_DEBUG
  if (s.err) Serial.printf("| %s  EE (반사 없음)\n", s.tag);
  else       Serial.printf("| %s  %.1f mm\n", s.tag, s.mm);
#endif
}

void service(Sensor &s) {
  while (s.port->available()) feed(s, s.port->read());
}

void startSensor(Sensor &s) {
  s.port->write(CMD_CONT, sizeof(CMD_CONT));
  s.lastRx = millis();
}

// ================= 삼각측량 =================
// 밑변 D, 좌 변 a, 우 변 b (서로 달라도 됨) → 꼭짓점 높이
Tri solveTri(float a, float b) {
  Tri t = {false, 0.0f, 0.0f};
  const float D = BASE_MM;
  if (a <= 0.0f || b <= 0.0f) return t;

  const float x  = (a * a - b * b + D * D) / (2.0f * D);  // 좌 착점 → 꼭짓점 수선의 발
  const float h2 = a * a - x * x;
  if (h2 <= 0.0f) return t;                                // 삼각형 안 됨

  t.h  = H_GAIN * sqrtf(h2) + H_OFFSET_MM;
  t.y  = x - 0.5f * D;
  t.ok = true;
  return t;
}

// ================= 디스플레이 =================
void fmtAge(char *b, size_t n, uint32_t ms) {
  if (ms < 9950)        snprintf(b, n, "%.1fs", ms / 1000.0f);
  else if (ms < 999500) snprintf(b, n, "%lus", (unsigned long)((ms + 500) / 1000));
  else                  snprintf(b, n, "OLD");
}

// inv = 반전(주의 표시), alignRight = x를 오른쪽 끝으로
void label(int x, int yb, const char *txt, bool inv, bool alignRight) {
  const int w = u8g2.getStrWidth(txt);
  if (alignRight) x -= w;
  if (inv) {
    const int a = u8g2.getAscent();
    u8g2.drawBox(x - 1, yb - a - 1, w + 2, a + 2);
    u8g2.setDrawColor(0);
  }
  u8g2.drawStr(x, yb, txt);
  u8g2.setDrawColor(1);
}

// 상단 한 칸: "L 1155" + 상태(● 측정중 / 반전 "1.2s" 과거값 / 반전 "EE" 반사없음)
void drawSensor(const Sensor &s, float shown, int x, int xr, uint32_t now) {
  char b[16];
  if (s.valid) snprintf(b, sizeof(b), "%s %.0f", s.tag, shown);
  else         snprintf(b, sizeof(b), "%s ----", s.tag);
  u8g2.drawStr(x, 8, b);

  if (s.err) {
    label(xr, 8, "EE", true, true);
  } else if (s.valid) {
    const uint32_t age = now - s.lastGood;
    if (age <= STALE_MS) {
      u8g2.drawDisc(xr - 3, 4, 3);
    } else {
      fmtAge(b, sizeof(b), age);
      label(xr, 8, b, true, true);
    }
  }
}

void drawScreen(float dL, float dR, const Tri &t, bool both, bool live,
                uint32_t hAge, uint32_t now) {
  char b[24];
  u8g2.clearBuffer();

  // 1) 상단: 레이저 거리 (작게) — 왼쪽 L, 오른쪽 R
  u8g2.setFont(u8g2_font_6x10_tf);
  drawSensor(sL, dL, 0, 61, now);
  drawSensor(sR, dR, 66, 127, now);
  u8g2.drawVLine(63, 0, 10);
  u8g2.drawHLine(0, 11, 128);

  // 2) 가운데: 높이 (크게)
  if (t.ok) snprintf(b, sizeof(b), "%.1f", t.h);
  else      strcpy(b, "----");
  u8g2.setFont(u8g2_font_logisoso32_tn);
  int w = u8g2.getStrWidth(b);
  if (w > 108) { u8g2.setFont(u8g2_font_logisoso24_tn); w = u8g2.getStrWidth(b); }
  int x = 111 - w;
  if (x < 0) x = 0;
  u8g2.drawStr(x, 48, b);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(114, 48, "mm");

  // 3) 하단 왼쪽: 높이 데이터 상태
  if (!both) {
    label(1, 63, "WAIT", true, false);
  } else if (live) {
    label(1, 63, "LIVE", false, false);
  } else {
    char a[8];
    fmtAge(a, sizeof(a), hAge);
    snprintf(b, sizeof(b), "HOLD %s", a);
    label(1, 63, b, true, false);
  }

  // 하단 오른쪽: 삼각형 성립 여부 / 꼭짓점 좌우 위치
  if (both) {
    if (!t.ok) {
      label(127, 63, "TRI NG", true, true);
    } else {
      snprintf(b, sizeof(b), "y%+.0f", t.y);
      label(127, 63, b, false, true);
    }
  }

  u8g2.sendBuffer();
}

// ================= 메인 =================
void setup() {
  Serial.begin(115200);
  Serial2.begin(SENSOR_BAUD, SERIAL_8N1, S1_RX, S1_TX);
  Serial1.begin(SENSOR_BAUD, SERIAL_8N1, S2_RX, S2_TX);

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  u8g2.setI2CAddress(OLED_ADDR * 2);
  u8g2.begin();
  u8g2.setFontMode(1);

  // 센서 부팅 대기 후 시간차를 두고 하나씩 시작
  delay(BOOT_DELAY_MS);
  startSensor(sL);
  delay(START_GAP_MS);
  startSensor(sR);

#if !RAW_DEBUG
  Serial.println("ms,L_mm,R_mm,h_mm,y_mm,live");
#endif
}

void loop() {
  service(sL);
  service(sR);

  const uint32_t now = millis();

  // 재시작은 한 번에 한 센서만, 간격 두고
  static uint32_t lastCmd = 0;
  if (now - lastCmd >= START_GAP_MS) {
    Sensor *t = nullptr;
    if (now - sL.lastRx > RESTART_MS)      t = &sL;
    else if (now - sR.lastRx > RESTART_MS) t = &sR;
    if (t) {
      startSensor(*t);
      lastCmd = now;
      Serial.printf("# %s 재시작\n", t->tag);
    }
  }

  static uint32_t rateT0 = 0;
  if (now - rateT0 >= 1000) {
    const float dt = (now - rateT0) / 1000.0f;
    sL.rateHz = sL.rateCount / dt; sL.rateCount = 0;
    sR.rateHz = sR.rateCount / dt; sR.rateCount = 0;
    rateT0 = now;
  }

  static uint32_t lastDraw = 0;
  if (now - lastDraw >= 1000 / DISPLAY_HZ) {
    lastDraw = now;

    const float dL = sL.filt + L_OFFSET_MM;
    const float dR = sR.filt + R_OFFSET_MM;
    const bool  both = sL.valid && sR.valid;
    const Tri   t = both ? solveTri(dL, dR) : Tri{false, 0.0f, 0.0f};

    const uint32_t ageL = now - sL.lastGood, ageR = now - sR.lastGood;
    const uint32_t hAge = ageL > ageR ? ageL : ageR;
    const bool live = both && !sL.err && !sR.err && hAge <= STALE_MS;

    drawScreen(dL, dR, t, both, live, hAge, now);

#if !RAW_DEBUG
    Serial.printf("%lu,%.1f,%.1f,%.1f,%.1f,%d\n",
                  (unsigned long)now, dL, dR, t.h, t.y, live);
#endif
  }
}
