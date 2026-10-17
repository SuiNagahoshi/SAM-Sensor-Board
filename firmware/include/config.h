// ===========================================================================
//  config.h  -  ピン割り当てと設定値 (ESP32 DevKit-C / Arduino)
//
//  ここを書き換えれば配線変更に追従できる。ST7789 用のピンはビルド時に
//  TFT_eSPI へ渡す必要があるため platformio.ini の build_flags にも同じ値を
//  書いてある (下の static_assert で不一致を検出する)。
//  詳細な配線は docs/PINOUT.txt を参照。
// ===========================================================================
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// I2C  (BME680 と MPU-9250 が共有)
// ---------------------------------------------------------------------------
#define PIN_I2C_SDA     21
#define PIN_I2C_SCL     22
#define I2C_FREQ_HZ     400000UL

// ---------------------------------------------------------------------------
// SPI  (ST7789 と USR-ES1[W5500] が SCLK/MOSI を共有)
//   液晶の実配線: SCK=18 / MOSI=23 / CS=15 / DC=19 / RST=17
//   USR-ES1      : SCLK=18 / MOSI=23 / MISO=13 / CS=16 / RST=4  (INT 未接続)
// ---------------------------------------------------------------------------
#define PIN_SPI_SCK     18
#define PIN_SPI_MISO    13   // USR-ES1(W5500) の MISO
                             //   ※ 液晶 DC とは別の GPIO にする必要がある (GPIO19 は液晶 DC)
#define PIN_SPI_MOSI    23

#define PIN_TFT_CS      15   // ST7789 chip select
#define PIN_TFT_DC      19   // ST7789 data/command
#define PIN_TFT_RST     17   // ST7789 reset
// バックライト (BLK/BL/LED)。GPIO に繋いだ場合のみ番号を入れる。
// 3.3V 直結や未使用なら -1 のまま (その場合はここで制御しない)。
#define PIN_TFT_BL      -1

#define PIN_ETH_CS      16   // USR-ES1 (W5500) chip select
#define PIN_ETH_RST     4    // USR-ES1 (W5500) reset (active low)

// ---- platformio.ini の TFT_eSPI ビルドフラグとの整合チェック ----
static_assert(PIN_TFT_CS   == TFT_CS,   "config.h と platformio.ini の TFT_CS が不一致");
static_assert(PIN_TFT_DC   == TFT_DC,   "config.h と platformio.ini の TFT_DC が不一致");
static_assert(PIN_TFT_RST  == TFT_RST,  "config.h と platformio.ini の TFT_RST が不一致");
static_assert(PIN_SPI_SCK  == TFT_SCLK, "config.h と platformio.ini の TFT_SCLK が不一致");
static_assert(PIN_SPI_MOSI == TFT_MOSI, "config.h と platformio.ini の TFT_MOSI が不一致");

// ---------------------------------------------------------------------------
// 表示 / 画面
// ---------------------------------------------------------------------------
#define SCREEN_W        240
#define SCREEN_H        240

// ---------------------------------------------------------------------------
// センサ I2C アドレス (実機に合わせて自動判別: 上から順に試す)
// ---------------------------------------------------------------------------
#define BME680_ADDR_0   0x77
#define BME680_ADDR_1   0x76
#define MPU9250_ADDR_0  0x68
#define MPU9250_ADDR_1  0x69

// ---------------------------------------------------------------------------
// タイミング (処理落ち回避のため各周期を分けてノンブロッキングで回す)
// ---------------------------------------------------------------------------
#define IMU_PERIOD_MS     5     // MPU-9250 読み出し周期 (~200Hz)
#define DRAW_PERIOD_MS    80    // 画面更新周期 (~12.5fps)
#define BME680_PERIOD_MS  3000  // BME680 測定開始周期 (ガスヒータ含め約3秒に1回)

// ---------------------------------------------------------------------------
// ネットワーク (PC と直結 LAN ケーブルを想定した固定 IP)
// ---------------------------------------------------------------------------
#define TCP_PORT        5000
#define ETH_IP_0        192
#define ETH_IP_1        168
#define ETH_IP_2        1
#define ETH_IP_3        50
#define ETH_MASK_0      255
#define ETH_MASK_1      255
#define ETH_MASK_2      255
#define ETH_MASK_3      0

// ---------------------------------------------------------------------------
// 受信メッセージ
// ---------------------------------------------------------------------------
#define MSG_MAX         128     // 受信 1 行の最大長 (バイト)
