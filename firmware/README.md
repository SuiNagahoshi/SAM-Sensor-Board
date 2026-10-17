# ESP32 環境センサ・モニタ (firmware)

ESP32 DevKit-C に接続した **BME680**・**InvenSense 6 軸 (MPU-6500/9250)**・**ST7789 液晶**・
**USR-ES1 (W5500)** をまとめて扱う Arduino ファームウェアです。PlatformIO / Espressif
ESP32 Dev Module 用。

- 画面 **左**: BME680 の温湿度・気圧・ガス (バー付き) と、USR-ES1 で受信した一言メッセージ
- 画面 **右**: IMU の加速度 / ジャイロ / 磁気を各軸グラフ表示
  (MPU 側 `WHO_AM_I` は 0x70。磁気 AK8963 は I2C バイパス → I2C マスタの順で自動検出し、
  見つからない場合のみ MAG 欄に "no magnetometer" と表示)
- PC とは USR-ES1 経由の **TCP** (ESP32 = サーバ) でやり取り

```
platformio.ini   … ビルド設定 (TFT_eSPI のピンは build_flags で指定)
include/config.h … ピン割り当て・アドレス・周期などの設定 (最初にここを見る)
src/main.cpp     … 本体 (センサ読出し / 描画 / TCP をノンブロッキングで周期実行)
pc/send_message.py … PC 側の送信スクリプト (Python3 標準ライブラリのみ)
docs/PINOUT.txt    … ピン割り当て・配線メモ (要点だけのテキスト)
docs/PROTOCOL.txt  … TCP 仕様と PC スクリプトの使い方
```

## ピン割り当て (要点)

詳細・注意点は [`docs/PINOUT.txt`](docs/PINOUT.txt) を参照。実体定義は `include/config.h`。

| 信号 | GPIO | 接続先 |
|------|------|--------|
| I2C SDA | 21 | BME680 / MPU-9250 (共有) |
| I2C SCL | 22 | BME680 / MPU-9250 (共有) |
| ST7789 SCK | 18 | ST7789 |
| ST7789 MOSI | 23 | ST7789 |
| ST7789 CS | 15 | ST7789 |
| ST7789 DC | 19 | ST7789 |
| ST7789 RST | 17 | ST7789 |
| ST7789 BLK | 3V3 | バックライト (直結) |
| ST7789 MISO | 未使用 | (書き込み専用) |
| USR-ES1 SCLK | 18 | ST7789 SCK と共通バス |
| USR-ES1 MOSI | 23 | ST7789 MOSI と共通バス |
| USR-ES1 MISO | 13 | W5500 → ESP32 (**下記の注意参照**) |
| USR-ES1 CS | 16 | W5500 |
| USR-ES1 RST | 4 | W5500 (アクティブ Low) |
| USR-ES1 INT | 未接続 | — |

> **W5500 の MISO 線は GPIO13 へ移してください。**
> 指示では MISO=GPIO19 ですが、GPIO19 は液晶の DC/RS が使っています。DC は液晶
> 専用の制御出力で SPI のデータ線ではないため、入力である MISO と同じ GPIO には
> 同居できません。`include/config.h` の `PIN_SPI_MISO` は 13 にしてあります
> (別の空き GPIO に変える場合は `config.h` を合わせてください)。
> MISO 線を移すまでの間も、液晶 (ENV) とセンサ (IMU) はこのままで動作します。

## ビルドと書き込み

PlatformIO をインストール済みの環境で、`firmware/` を開いて:

```sh
pio run                 # ビルド
pio run -t upload       # 書き込み (USB 接続)
pio device monitor      # シリアルモニタ (115200bps)
```

`platformio.ini` の `lib_deps` は以下を自動取得します。

| ライブラリ | 用途 |
|-----------|------|
| `bodmer/TFT_eSPI` | ST7789 表示 (高速・transactions 対応) |
| `adafruit/Adafruit BME680 Library` | 温湿度/気圧/ガス (ノンブロッキング API 使用) |
| (MPU/MAG はライブラリ不使用) | `src/main.cpp` 内の自前 I2C ドライバ (6 軸 + AK8963) |
| `khoih-prog/Ethernet_Generic` | W5500 TCP (ESP32 対応) |

## 使い方

1. LAN ケーブルで PC と USR-ES1 を直結 (または同一セグメントのハブ経由)。
2. PC の IP を `192.168.1.10/24` など ESP32 と同一セグメントに固定。
3. 送信:

```sh
python pc/send_message.py "hello"
python pc/send_message.py            # 対話モード (空行で終了)
```

ESP32 側は受信すると画面左下の MSG 欄に折返し表示し、`OK` を返します。
仕様は [`docs/PROTOCOL.txt`](docs/PROTOCOL.txt)。

## 処理落ち対策 (設計方針)

- `loop()` 内で `delay()` せず、すべて `millis()` の周期判定で非ブロッキング実行。
  - MPU-9250: 5ms 毎 (約 200Hz) に `readSensor()`
  - BME680 : 3 秒毎に `beginReading()` を開始し、`remainingReadingMillis()` が
    0 になったら `endReading()`。ガスヒータ待ちでループを止めない。
  - 画面   : 80ms 毎 (約 12.5fps)。`startWrite()/endWrite()` で 1 フレームを
    まとめて転送し、数値は `setTextPadding()` で前回文字を消してゴーストを防ぐ。
  - TCP    : 毎ループ `server.available()` と受信バッファをチェック。
- ST7789 と W5500 は **SPI バスを共有**します。TFT_eSPI も W5500 も
  `SPI.beginTransaction()/endTransaction()` を使うため、CS の競合は起きません。
  描画と TCP 送受信を同時に行わないよう、`loop()` で順に処理しています。

## トラブルシューティング

| 症状 | 確認 |
|------|------|
| 画面が真っ暗/白 | ST7789 の CS(15)/DC(19)/RST(17)、バックライト(BLK→3V3)、SCK(18)/MOSI(23) を確認 |
| シリアルに `BME680: not found` | I2C 21/22、3V3。アドレスは 0x77→0x76 を自動試行 |
| シリアルに `MPU: not found` | I2C 21/22。`WHO_AM_I` が 0x70/0x71/0x73 以外だと未対応 |
| `Ethernet: W5500 not found` | USR-ES1 の CS(16)/RST(4)/MISO(13)、3V3。MISO が GPIO19 のままだと検出できない |
| PC から繋がらない | PC の IP を 192.168.1.x/24 に固定、ポート 5000、FW を確認 |
| 画面更新がカクつく | `DRAW_PERIOD_MS` を大きく、`SPI_FREQUENCY` を下げる (platformio.ini) |

## 変更履歴 / ログ (log & doc)

- 2026-10-17 初版。BME680/MPU-9250/ST7789/USR-ES1 を統合。TCP サーバ (5000) と
  PC 送信スクリプトを追加。ピンは ST7789 CS=15, USR-ES1 CS=18, SPI=12/13/14 に確定。
- 2026-10-17 液晶の実配線に合わせて修正: ST7789 SCK=18, MOSI=23, CS=15, DC=19,
  RST=17 (MISO 未使用)。`tft_test` 環境を追加。USR-ES1 側の配線は未確定
  (`docs/PINOUT.txt` 参照)。
- 2026-10-18 液晶の表示を実機で確認 (`tft_test` で色が切り替わる)。ブレッドボード
  配線では 40MHz が不安定だったため SPI クロックを 10MHz に低下。安定確認後に
  20〜40MHz へ上げてよい (platformio.ini の SPI_FREQUENCY)。
- 2026-10-18 USR-ES1(W5500) の配線を反映: CS=16, RST=4 (リセットパルスを追加)。
  MISO は GPIO19 が液晶 DC と重複するため GPIO13 とした。起動時に I2C スキャンと
  MPU9250 の `begin()` 戻り値を表示・ログするようにした。
- 2026-10-18 IMU は実機の MPU 側 `WHO_AM_I` が 0x70 と判明 (0x71/0x73 のみ許容する
  Bolder Flight ライブラリは使えない)。加速度 [g] + ジャイロ [dps] の自前 I2C ドライバに
  変更。磁気 (AK8963) は I2C バイパス → I2C マスタの順で自動検出して 16bit/100Hz で読む。
- 既知の対応: `Ethernet_Generic 2.8.1` は Arduino-ESP32 core 3.x の
  `Server::begin(uint16_t)` を実装していないため、`src/main.cpp` 内の薄い
  ラッパ `TcpServer` で回避している (core へのパッチ不要)。
