// ===========================================================================
//  tft_test.cpp  -  ST7789 表示だけの切り分けテスト
//
//  使い方:
//     pio run -e tft_test -t upload
//     pio device monitor            (115200bps)
//
//  画面が 赤 → 緑 → 青 → 白 → 黒 と 0.8 秒毎に全画面で切り替われば、
//  液晶・配線・SPI 初期化は正常。切り替わらない場合は以下を疑う。
//    * バックライト (BLK) が点いていない  -> config.h の PIN_TFT_BL
//    * CS/DC/RST のピン違い              -> config.h / platformio.ini
//    * SCK/MOSI/MISO のピン違い
//  画面中央に文字が出れば描画も OK。
// ===========================================================================

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include "config.h"

static TFT_eSPI tft = TFT_eSPI();

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println(F("===== TFT TEST ====="));
  Serial.printf("Pins: SCK=%d MISO=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d\n",
                PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI,
                PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST, PIN_TFT_BL);

  // CS を非活性に固定してから SPI を初期化
  pinMode(PIN_TFT_CS, OUTPUT);  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(PIN_ETH_CS, OUTPUT);  digitalWrite(PIN_ETH_CS, HIGH);

#if PIN_TFT_BL >= 0
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);
  Serial.println(F("Backlight: ON (GPIO drive)"));
#else
  Serial.println(F("Backlight: not controlled (tie BLK to 3.3V)"));
#endif

  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_ETH_CS);

  tft.init();
  tft.setRotation(0);
  tft.setTextDatum(TL_DATUM);
  Serial.printf("TFT init done: w=%d h=%d\n", tft.width(), tft.height());
}

void loop() {
  static const uint16_t colors[] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE, TFT_BLACK };
  static const char*    names[]  = { "RED", "GREEN", "BLUE", "WHITE", "BLACK" };
  static uint8_t idx = 0;

  Serial.printf("fillScreen: %s\n", names[idx]);
  tft.fillScreen(colors[idx]);

  // 背景と反転した色で文字を描く (描画確認用)
  uint16_t fg = (colors[idx] == TFT_WHITE) ? TFT_BLACK : TFT_WHITE;
  tft.setTextColor(fg, colors[idx]);
  tft.setTextFont(2);
  tft.drawString(names[idx], 10, 10);
  tft.drawString("TFT TEST", 10, 30);

  idx = (idx + 1) % 5;
  delay(800);
}
