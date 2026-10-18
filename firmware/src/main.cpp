// ===========================================================================
//  main.cpp
//  ESP32 DevKit-C 環境センサ・モニタ
//
//    I2C : BME680 (温湿度/気圧/ガス), MPU-9250/6500 (加速度/ジャイロ/磁気)
//    SPI (共有) : ST7789 (SCK=18, MOSI=23, CS=15) と
//                 USR-ES1/W5500 (SCLK=18, MOSI=23, MISO=13, CS=16, RST=4)
//
//    画面左 : BME680 の値 + USR-ES1 で受信したテキストメッセージ
//    画面右 : MPU-6500/9250 の各軸 (加速度/ジャイロ) をグラフ表示
//
//  処理落ち回避のため、センサ読出し・通信・描画をすべてノンブロッキングで
//  周期実行する (loop 内で delay しない)。
//  ピンなどの詳細は include/config.h と docs/PINOUT.txt を参照。
// ===========================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Adafruit_BME680.h>
#include <MPU9250.h>
#include <Ethernet_Generic.h>
#include "config.h"

// ---------------------------------------------------------------------------
// ネットワーク設定 (固定 IP / 直結ケーブル想定)
// ---------------------------------------------------------------------------
static uint8_t   ETH_MAC[6]  = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01 };
static IPAddress ETH_IP(ETH_IP_0, ETH_IP_1, ETH_IP_2, ETH_IP_3);
static IPAddress ETH_MASK(ETH_MASK_0, ETH_MASK_1, ETH_MASK_2, ETH_MASK_3);
static IPAddress ETH_GW(ETH_IP_0, ETH_IP_1, ETH_IP_2, 1);
static IPAddress ETH_DNS(ETH_IP_0, ETH_IP_1, ETH_IP_2, 1);

// ---------------------------------------------------------------------------
// オブジェクト
// ---------------------------------------------------------------------------
// Ethernet_Generic 2.8.1 は Arduino-ESP32 core 3.x の Server::begin(uint16_t)
// を override していないため、そのままでは abstract クラスになる。
// ここで薄いラッパを作って core を書き換えずに対応する。
class TcpServer : public EthernetServer {
public:
  explicit TcpServer(uint16_t port) : EthernetServer(port) {}
  void begin(uint16_t port = 0) override {
    (void)port;
    EthernetServer::begin();
  }
};

static TFT_eSPI       tft = TFT_eSPI();
static Adafruit_BME680 bme(&Wire);
static MPU9250        imu;                  // hideakitai/MPU9250 ライブラリ
static TcpServer      server(TCP_PORT);
static EthernetClient client;

// ---------------------------------------------------------------------------
// 色 / レイアウト定数
// ---------------------------------------------------------------------------
static const uint16_t C_BG     = 0x0000;
static const uint16_t C_HDR    = 0x0210;   // 濃紺
static const uint16_t C_HDRTXT = 0xFFFF;
static const uint16_t C_LBL    = 0x8C71;   // 灰
static const uint16_t C_AXIS   = 0xBDF7;   // 明灰
static const uint16_t C_TRACK  = 0x39E7;   // バー下地
static const uint16_t C_TEMP   = 0xFD20;   // 橙
static const uint16_t C_HUM    = 0x07FF;   // 水
static const uint16_t C_PRES   = 0x07E0;   // 緑
static const uint16_t C_GAS    = 0xFFE0;   // 黄
static const uint16_t C_ACC    = 0xF800;   // 赤
static const uint16_t C_GYR    = 0x07E0;   // 緑
static const uint16_t C_MAG    = 0x07FF;   // 水
static const uint16_t C_MSG    = 0xFFFF;
static const uint16_t C_MSGHDR = 0x0410;
static const uint16_t C_ERR    = 0xF800;
static const uint16_t C_OK     = 0x07E0;

#define RIGHT_X     120
#define RIGHT_W     120
#define HDR_H       16

#define ENV_X       4
#define ENV_LBL_Y0  18
#define ENV_BLOCK_H 36
#define ENV_BAR_W   112
#define ENV_BAR_H   6

#define MSG_X       2
#define MSG_Y       162
#define MSG_W       116
#define MSG_H       76
#define MSG_LINES   5
#define MSG_COLS    18

#define IMU_LBL_X   124
#define IMU_VAL_X   136
#define IMU_VAL_W   46
#define IMU_BAR_X   184
#define IMU_BAR_W   52
#define IMU_BAR_H   7
#define IMU_ROW_H   14
#define IMU_ACC_Y0  32
#define IMU_GYR_Y0  90
#define IMU_MAG_Y0  148

// ---------------------------------------------------------------------------
// 状態
// ---------------------------------------------------------------------------
static float    envTemp = 0, envHum = 0, envPres = 0;
static uint32_t envGas  = 0;
static bool     envValid = false;

static bool     bmeOk = false, bmeMeasuring = false;
static uint32_t bmeNextStart = 0;

static float    ax, ay, az, gx, gy, gz, mx, my, mz;
static bool     imuValid = false, imuOk = false;
static bool     magOk = false;                // 磁気 (AK8963) の有無
static uint32_t imuNext = 0;

static bool     ethOk = false;
static char     curMsg[MSG_MAX] = "";
static bool     msgChanged = true;
static uint32_t msgCount = 0;
static char     rxBuf[MSG_MAX];
static size_t   rxLen = 0;

static uint32_t drawNext = 0;
static uint32_t dbgNext  = 0;

// ===========================================================================
//  描画ヘルパ
// ===========================================================================

// 左パネル用: 左端から伸びるバー (lo..hi を 0..w にマップ)
static void barFill(int x, int y, int w, int h, float v, float lo, float hi, uint16_t color) {
  tft.fillRect(x, y, w, h, C_TRACK);
  float f = (v - lo) / (hi - lo);
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  int len = (int)(w * f);
  if (len > 0) tft.fillRect(x, y, len, h, color);
}

// 右パネル用: 中心から伸びるバー (-scale..+scale をマップ)
static void barCentered(int x, int y, int w, int h, float v, float scale, uint16_t color) {
  tft.fillRect(x, y, w, h, C_TRACK);
  int cx = x + w / 2, half = w / 2;
  float f = v / scale;
  if (f > 1) f = 1;
  if (f < -1) f = -1;
  int len = (int)(half * (f < 0 ? -f : f));
  if (len > 0) {
    if (f >= 0) tft.fillRect(cx, y, len, h, color);
    else        tft.fillRect(cx - len, y, len, h, color);
  }
  tft.drawFastVLine(cx, y, h, C_AXIS);
}

// 数値 1 行 (軸ラベル + 値 + 中央バー)
static void drawImuRow(int y, const char* axis, float v, float scale, uint16_t color) {
  char s[16];
  tft.setTextFont(1);
  tft.setTextColor(C_AXIS, C_BG);
  tft.setTextPadding(0);
  tft.drawString(axis, IMU_LBL_X, y);

  tft.setTextColor(color, C_BG);
  tft.setTextPadding(IMU_VAL_W);
  snprintf(s, sizeof(s), "%+7.2f", v);
  tft.drawString(s, IMU_VAL_X, y);

  barCentered(IMU_BAR_X, y + 1, IMU_BAR_W, IMU_BAR_H, v, scale, color);
}

// 静止画 (setup で 1 回だけ描く)
static void drawStatic() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);

  // ヘッダ
  tft.fillRect(0, 0, RIGHT_X, HDR_H, C_HDR);
  tft.fillRect(RIGHT_X, 0, RIGHT_W, HDR_H, C_HDR);
  tft.setTextFont(2);
  tft.setTextColor(C_HDRTXT, C_HDR);
  tft.setTextPadding(0);
  tft.drawString("ENV", 6, 0);
  tft.drawString("IMU", RIGHT_X + 6, 0);

  // 左パネルの見出し
  static const char* lbl[4] = { "TEMP", "HUM", "PRES", "GAS" };
  tft.setTextFont(1);
  tft.setTextColor(C_LBL, C_BG);
  for (int i = 0; i < 4; i++)
    tft.drawString(lbl[i], ENV_X, ENV_LBL_Y0 + i * ENV_BLOCK_H);

  // 右パネルのセクション見出し
  tft.setTextColor(C_AXIS, C_BG);
  tft.drawString("ACC [g]",   IMU_LBL_X, IMU_ACC_Y0 - 12);
  tft.drawString("GYR [dps]", IMU_LBL_X, IMU_GYR_Y0 - 12);
  tft.drawString("MAG [uT]",  IMU_LBL_X, IMU_MAG_Y0 - 12);

  // メッセージ枠
  tft.drawRect(MSG_X, MSG_Y, MSG_W, MSG_H, C_MSGHDR);
  tft.fillRect(MSG_X + 1, MSG_Y + 1, MSG_W - 2, 12, C_MSGHDR);
  tft.setTextColor(C_HDRTXT, C_MSGHDR);
  tft.drawString("MSG", MSG_X + 4, MSG_Y + 3);
}

// 左パネルの値とバー
static void drawEnv() {
  char s[24];
  tft.setTextFont(2);
  tft.setTextPadding(ENV_BAR_W);

  tft.setTextColor(C_TEMP, C_BG);
  snprintf(s, sizeof(s), envValid ? "%.1f C" : "--.- C", envTemp);
  tft.drawString(s, ENV_X, ENV_LBL_Y0 + 9);
  barFill(ENV_X, ENV_LBL_Y0 + 27, ENV_BAR_W, ENV_BAR_H, envTemp, 0, 50, C_TEMP);

  tft.setTextColor(C_HUM, C_BG);
  snprintf(s, sizeof(s), envValid ? "%.1f %%" : "--.- %%", envHum);
  tft.drawString(s, ENV_X, ENV_LBL_Y0 + ENV_BLOCK_H + 9);
  barFill(ENV_X, ENV_LBL_Y0 + ENV_BLOCK_H + 27, ENV_BAR_W, ENV_BAR_H, envHum, 0, 100, C_HUM);

  tft.setTextColor(C_PRES, C_BG);
  snprintf(s, sizeof(s), envValid ? "%.1f hPa" : "--.- hPa", envPres / 100.0f);
  tft.drawString(s, ENV_X, ENV_LBL_Y0 + 2 * ENV_BLOCK_H + 9);
  barFill(ENV_X, ENV_LBL_Y0 + 2 * ENV_BLOCK_H + 27, ENV_BAR_W, ENV_BAR_H, envPres / 100.0f, 950, 1050, C_PRES);

  tft.setTextColor(C_GAS, C_BG);
  snprintf(s, sizeof(s), envValid ? "%.1f kOhm" : "--.- kOhm", envGas / 1000.0f);
  tft.drawString(s, ENV_X, ENV_LBL_Y0 + 3 * ENV_BLOCK_H + 9);
  barFill(ENV_X, ENV_LBL_Y0 + 3 * ENV_BLOCK_H + 27, ENV_BAR_W, ENV_BAR_H, envGas / 1000.0f, 0, 200, C_GAS);
}

// 右パネル
static void drawImu() {
  if (imuOk && imuValid) {
    drawImuRow(IMU_ACC_Y0 + 0 * IMU_ROW_H, "X", ax, 2.0f,   C_ACC);
    drawImuRow(IMU_ACC_Y0 + 1 * IMU_ROW_H, "Y", ay, 2.0f,   C_ACC);
    drawImuRow(IMU_ACC_Y0 + 2 * IMU_ROW_H, "Z", az, 2.0f,   C_ACC);
    drawImuRow(IMU_GYR_Y0 + 0 * IMU_ROW_H, "X", gx, 250.0f, C_GYR);
    drawImuRow(IMU_GYR_Y0 + 1 * IMU_ROW_H, "Y", gy, 250.0f, C_GYR);
    drawImuRow(IMU_GYR_Y0 + 2 * IMU_ROW_H, "Z", gz, 250.0f, C_GYR);
    if (magOk) {
      drawImuRow(IMU_MAG_Y0 + 0 * IMU_ROW_H, "X", mx, 200.0f, C_MAG);
      drawImuRow(IMU_MAG_Y0 + 1 * IMU_ROW_H, "Y", my, 200.0f, C_MAG);
      drawImuRow(IMU_MAG_Y0 + 2 * IMU_ROW_H, "Z", mz, 200.0f, C_MAG);
    } else {
      tft.setTextFont(1);
      tft.setTextColor(C_LBL, C_BG);
      tft.setTextPadding(RIGHT_W - 8);
      tft.drawString("no magnetometer", IMU_LBL_X, IMU_MAG_Y0);
    }
  } else {
    tft.setTextFont(1);
    tft.setTextColor(C_ERR, C_BG);
    tft.setTextPadding(RIGHT_W - 8);
    tft.drawString(imuOk ? "waiting..." : "IMU not found", IMU_LBL_X, IMU_ACC_Y0);
  }
}

// 受信メッセージ (変化時のみ)
static void drawMsg() {
  tft.fillRect(MSG_X + 1, MSG_Y + 13, MSG_W - 2, MSG_H - 14, C_BG);
  tft.setTextFont(1);
  tft.setTextColor(C_MSG, C_BG);
  tft.setTextPadding(MSG_W - 8);

  const char* p = curMsg;
  char line[MSG_COLS + 1];
  for (int li = 0; li < MSG_LINES && *p; li++) {
    size_t remain = strlen(p);
    size_t take = remain < MSG_COLS ? remain : MSG_COLS;
    if (remain > MSG_COLS) {                 // 空白で折り返せれば折る
      for (size_t i = take; i > 0; i--) {
        if (p[i - 1] == ' ') { take = i - 1; break; }
      }
      if (take == 0) take = MSG_COLS;        // 長い単語は強制分割
    }
    memcpy(line, p, take);
    line[take] = '\0';
    tft.drawString(line, MSG_X + 4, MSG_Y + 15 + li * 11);
    p += take;
    while (*p == ' ') p++;
  }
}

// 下部ステータス
static void drawStatus() {
  char s[28];
  tft.setTextFont(1);
  tft.setTextPadding(RIGHT_W - 8);

  tft.setTextColor(ethOk ? C_OK : C_ERR, C_BG);
  snprintf(s, sizeof(s), "IP %u.%u.%u.%u", ETH_IP_0, ETH_IP_1, ETH_IP_2, ETH_IP_3);
  tft.drawString(s, IMU_LBL_X, 196);

  tft.setTextColor(C_AXIS, C_BG);
  snprintf(s, sizeof(s), "BME:%s IMU:%s", bmeOk ? "ok" : "--", imuOk ? "ok" : "--");
  tft.drawString(s, IMU_LBL_X, 208);

  snprintf(s, sizeof(s), "RX msg:%lu", (unsigned long)msgCount);
  tft.drawString(s, IMU_LBL_X, 220);
}

static void drawDynamic() {
  tft.startWrite();
  drawEnv();
  drawImu();
  drawStatus();
  if (msgChanged) { drawMsg(); msgChanged = false; }
  tft.endWrite();
}

// ===========================================================================
//  センサ
// ===========================================================================

// --- MPU-9250 / MPU-6500 (hideakitai/MPU9250 外部ライブラリ) -----------------
//  WHO_AM_I = 0x70 (MPU-6500) / 0x71 (MPU-9250) / 0x73 (MPU-9255) をすべて
//  認識し、磁気 (AK8963) の有無もライブラリ自身が判定する。AK8963 は MPU に
//  内蔵 (補助 I2C バス) なので、ライブラリが I2C バイパスを有効化して 0x0C を
//  直接読み出す。
//
//  磁気が無い個体では setup() が false を返し update() が動かなくなるため、
//  ここでは公開 API の update_accel_gyro() / update_mag() を個別に呼び、
//  6 軸だけでも必ず読めるようにしている。
static void imuApplySettings(MPU9250Setting& st) {
  st.accel_fs_sel     = ACCEL_FS_SEL::A2G;         // ±2g     (16384 LSB/g)
  st.gyro_fs_sel      = GYRO_FS_SEL::G250DPS;      // ±250dps (131 LSB/dps)
  st.mag_output_bits  = MAG_OUTPUT_BITS::M16BITS;  // 16bit (0.15 uT/LSB)
  st.fifo_sample_rate = FIFO_SAMPLE_RATE::SMPL_200HZ;
  st.gyro_dlpf_cfg    = GYRO_DLPF_CFG::DLPF_41HZ;
  st.accel_dlpf_cfg   = ACCEL_DLPF_CFG::DLPF_45HZ;
}

// MPU 本体 (磁気があれば AK8963 も) を初期化する
static bool imuBegin() {
  MPU9250Setting st;
  imuApplySettings(st);

  imu.verbose(true);                                 // WHO_AM_I 診断を表示
  bool both = imu.setup(MPU9250_ADDR_0, st);         // 0x68
  imuOk = imu.isConnectedMPU9250();
  if (!imuOk) {                                      // 0x68 に居なければ 0x69 を試す
    both = imu.setup(MPU9250_ADDR_1, st);
    imuOk = imu.isConnectedMPU9250();
  }
  magOk = both;                                      // AK8963 まで見つかれば true

  Serial.printf("IMU: MPU=%s MAG(AK8963)=%s\n",
                imuOk ? "ok" : "--", magOk ? "ok" : "--");
  return imuOk;
}

// 加速度 [g] / ジャイロ [dps] / 磁気 [uT] を読み出す
static bool imuReadSensor() {
  imu.update_accel_gyro();                           // has_connected に依存しない
  ax = imu.getAccX();   ay = imu.getAccY();   az = imu.getAccZ();
  gx = imu.getGyroX();  gy = imu.getGyroY();  gz = imu.getGyroZ();
  if (magOk) {
    imu.update_mag();
    mx = imu.getMagX() / 10.0f;                      // mG -> uT
    my = imu.getMagY() / 10.0f;
    mz = imu.getMagZ() / 10.0f;
  }
  return true;
}

static void i2cScan() {
  Serial.println(F("I2C scan:"));
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  found 0x"));
      if (a < 16) Serial.print('0');
      Serial.println(a, HEX);
      found++;
    }
  }
  if (!found) Serial.println(F("  (none)"));
}

static void setupSensors() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  delay(50);

  // BME680
  if (bme.begin(BME680_ADDR_0) || bme.begin(BME680_ADDR_1)) {
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);        // 320C / 150ms
    bmeOk = true;
    Serial.println(F("BME680: OK"));
  } else {
    Serial.println(F("BME680: not found"));
  }

  // MPU-9250 / MPU-6500 (+ 磁気 AK8963) — hideakitai/MPU9250 ライブラリ
  imuBegin();

  // I2C スキャン (磁気がバイパスで見えていれば 0x0C も並ぶ)
  i2cScan();
}

static void serviceBme(uint32_t now) {
  if (!bmeOk) return;
  if (!bmeMeasuring) {
    if ((int32_t)(now - bmeNextStart) >= 0) {
      if (bme.beginReading() != 0) bmeMeasuring = true;
      else                        bmeNextStart = now + BME680_PERIOD_MS;
    }
  } else if (bme.remainingReadingMillis() == 0) {
    if (bme.endReading()) {
      envTemp = bme.temperature;
      envPres = (float)bme.pressure;
      envHum  = bme.humidity;
      envGas  = bme.gas_resistance;
      envValid = true;
    }
    bmeMeasuring = false;
    bmeNextStart = now + BME680_PERIOD_MS;
  }
}

static void serviceImu(uint32_t now) {
  if (!imuOk) return;
  if ((int32_t)(now - imuNext) < 0) return;
  imuNext = now + IMU_PERIOD_MS;
  imuReadSensor();
  imuValid = true;
}

// ===========================================================================
//  ネットワーク
// ===========================================================================
static void applyMessage(const char* m) {
  strncpy(curMsg, m, MSG_MAX - 1);
  curMsg[MSG_MAX - 1] = '\0';
  msgChanged = true;
  msgCount++;
  Serial.print(F("RX: "));
  Serial.println(curMsg);
  if (client && client.connected()) client.println(F("OK"));
}

static void serviceNet() {
  if (!ethOk) return;

  if (!client) {
    EthernetClient c = server.available();
    if (c) client = c;
  }
  if (!client) return;

  while (client.available() > 0) {
    int ci = client.read();
    if (ci < 0) break;
    char ch = (char)ci;
    if (ch == '\n' || ch == '\r') {
      if (rxLen > 0) { rxBuf[rxLen] = '\0'; applyMessage(rxBuf); rxLen = 0; }
    } else if (rxLen < MSG_MAX - 1) {
      rxBuf[rxLen++] = ch;
    } else {
      rxLen = 0;                          // 長すぎる行は捨てる
    }
  }

  if (!client.connected()) {
    if (rxLen > 0) { rxBuf[rxLen] = '\0'; applyMessage(rxBuf); rxLen = 0; }
    client.stop();
  }
}

// ---- W5500 生 SPI 診断 -----------------------------------------------------
//  VERSIONR (共通レジスタ 0x0039) を直接読む。0x04 なら SPI/CS/MISO 配線は OK。
//  Ethernet.begin() より前に呼び、"配線の問題" か "ライブラリ設定の問題" かを
//  切り分けられるようにする。
static uint8_t ethReadReg(uint16_t addr, uint8_t bsb) {
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_ETH_CS, LOW);
  delayMicroseconds(2);
  SPI.transfer((uint8_t)(addr >> 8));
  SPI.transfer((uint8_t)(addr & 0xFF));
  SPI.transfer((uint8_t)((bsb << 3) | 0x00));   // BSB[7:3] | RWB=0(read) | OM=00
  uint8_t v = (uint8_t)SPI.transfer(0x00);
  digitalWrite(PIN_ETH_CS, HIGH);
  SPI.endTransaction();
  return v;
}

static void ethProbe() {
  uint8_t ver = ethReadReg(0x0039, 0x00);        // VERSIONR
  Serial.printf("W5500 probe: VERSIONR=0x%02X (expect 0x04)  CS=%d MISO=%d\n",
                ver, PIN_ETH_CS, PIN_SPI_MISO);
  if (ver != 0x04)
    Serial.println(F("  -> W5500 が見えない: MISO=13 / CS=16 / RST=4 / 3V3 を確認"));
}

static void setupNetwork() {
  // USR-ES1(W5500) をリセットパルスで確実に初期化する
  pinMode(PIN_ETH_RST, OUTPUT);
  digitalWrite(PIN_ETH_RST, LOW);
  delay(20);
  digitalWrite(PIN_ETH_RST, HIGH);
  delay(150);

  ethProbe();

  Ethernet.init(PIN_ETH_CS);
  Ethernet.begin(ETH_MAC, ETH_IP, ETH_DNS, ETH_GW, ETH_MASK);
  ethOk = (Ethernet.hardwareStatus() != EthernetNoHardware);
  if (ethOk) {
    server.begin();
    Serial.print(F("Ethernet: OK  IP="));
    Serial.print(ETH_IP);
    Serial.print(F("  CS="));
    Serial.print(PIN_ETH_CS);
    Serial.print(F(" MISO="));
    Serial.println(PIN_SPI_MISO);
  } else {
    Serial.println(F("Ethernet: W5500 not found"));
  }
}

// ===========================================================================
//  setup / loop
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n== ESP32 sensor monitor =="));

  // CS をあらかじめ非活性 (High) にしてからバスを初期化する
  pinMode(PIN_TFT_CS, OUTPUT); digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(PIN_ETH_CS, OUTPUT); digitalWrite(PIN_ETH_CS, HIGH);

#if PIN_TFT_BL >= 0
  // バックライトを点灯 (GPIO 接続時のみ)
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);
#endif

  // ST7789 と W5500 で共有する SPI バス
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_ETH_CS);

  // 液晶
  tft.init();
  tft.setRotation(0);
  tft.setTextDatum(TL_DATUM);

  setupSensors();
  setupNetwork();

  drawStatic();
  drawDynamic();
  msgChanged = false;

  uint32_t now = millis();
  drawNext = now + DRAW_PERIOD_MS;
  imuNext  = now;
  bmeNextStart = now;
  dbgNext  = now + 2000;
}

void loop() {
  uint32_t now = millis();

  serviceNet();
  serviceBme(now);
  serviceImu(now);

  if ((int32_t)(now - drawNext) >= 0) {
    drawNext = now + DRAW_PERIOD_MS;
    drawDynamic();
  }

  // 1 行サマリのログ (2 秒毎)
  if ((int32_t)(now - dbgNext) >= 0) {
    dbgNext = now + 2000;
    Serial.printf("T=%.1fC H=%.1f%% P=%.1fhPa G=%.1fkOhm | ax=%.2f ay=%.2f az=%.2f gz=%.1f | msg=%lu\n",
                  envTemp, envHum, envPres / 100.0f, envGas / 1000.0f,
                  ax, ay, az, gz, (unsigned long)msgCount);
  }
}
