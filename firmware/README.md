# ESP32 環境センサ・モニタ (firmware)

ESP32 DevKit-C に接続した **BME680**・**InvenSense 9軸/6軸 (MPU-9250/6500)**・**ST7789 液晶**・
**USR-ES1 (W5500)** をまとめて扱う Arduino ファームウェアです。PlatformIO / Espressif
ESP32 Dev Module 用。

- 画面 **左**: BME680 の温湿度・気圧・ガス (バー付き) と、USR-ES1 で受信した一言メッセージ
- 画面 **右**: IMU の加速度 / ジャイロを各軸グラフ表示
  (IMU は外部ライブラリ `hideakitai/MPU9250` を使用。実機の `WHO_AM_I` は 0x70 で
   `AK8963 WHO AM I` も応答せず、**9軸 (MPU-9250) と表示されて販売されていた
   6軸 (MPU-6500 相当) の偽装品**と確定。ファームは 6 軸で動作し、MAG 欄には
   "no magnetometer" と表示される。9軸の本物に交換した場合はライブラリが
   磁気を自動検出して MAG 表示が有効になる (コードの変更は不要))
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
| (MPU/MAG は上記ライブラリ) | `hideakitai/MPU9250` — 6軸 + AK8963 磁気 (WHO_AM_I 0x70/0x71/0x73 対応) |
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
  - MPU-9250: 5ms 毎 (約 200Hz) にライブラリの `update_accel_gyro()` (+ 磁気があれば `update_mag()`)
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
| シリアルに `IMU: MPU=--` | I2C 21/22。`WHO_AM_I` が 0x70/0x71/0x73 以外だと未対応 |
| シリアルに `AK8963 WHO AM I = 0x..` / `Could not connect to AK8963` | 磁気が読み出せない個体。6 軸はそのまま動作 (MAG 欄は "no magnetometer") |
| `Ethernet: W5500 not found` | まず `W5500 probe: VERSIONR=0x04` を確認。0x04 以外なら MISO=13 / CS=16 / RST=4 / 3V3 の配線。VERSIONR=0x04 なのに not found なら `TFT_MISO` 設定を確認 |
| PC から繋がらない | PC の IP を 192.168.1.x/24 に固定、ポート 5000、FW を確認 |
| 画面更新がカクつく | `DRAW_PERIOD_MS` を大きく、`SPI_FREQUENCY` を下げる (platformio.ini) |
| MAG 欄に `no magnetometer` | 現在の MPU モジュールは 6軸偽装品のため正常な表示。本物の MPU-9250 に交換すると自動で MAG 表示が有効になる |

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
- 2026-10-18 実機の MPU 側 `WHO_AM_I` が 0x70 (MPU-6500 相当の値) と判明。
  自前ドライバをやめ、**外部ライブラリ `hideakitai/MPU9250` に統一**した。この
  ライブラリは 0x70/0x71/0x73 をすべて認識し、内蔵磁気 AK8963 は I2C バイパスを
  有効化して 0x0C を直接読む。磁気が無い個体では `setup()` が false を返して
  `update()` が止まるため、公開 API の `update_accel_gyro()` / `update_mag()` を
  個別に呼び、6 軸だけでも必ず動くようにしている。
- 2026-10-18 ライブラリによる判定でも AK8963 が見つからず、**購入した MPU-9250
  モジュールは実態が 6軸 (MPU-6500 相当) の偽装品と確定** (販売者には連絡済み)。
  ファームは 6 軸 (加速度 [g] / ジャイロ [dps]) で継続動作。9軸の本物に差し替え
  た場合は MAG 欄が自動で有効になる。
- 2026-10-18 **W5500 が検出できない原因を修正**: TFT_eSPI は自前の
  `SPIClass(VSPI)` で `spi.begin(SCLK, TFT_MISO, MOSI, -1)` を呼ぶため、
  `TFT_MISO=-1` だと ESP32 HAL が VSPI 既定の **GPIO19 (= 液晶 DC)** に MISO を
  接続し直し、共有バスの MISO=GPIO13 を上書きしていた。`platformio.ini` で
  `TFT_MISO=13` を明示し、`config.h` に整合の `static_assert` を追加。
  さらに起動時に W5500 の `VERSIONR` (0x0039) を生 SPI で読み、
  配線不良かどうかを切り分けられるようにした。
- 既知の対応: `Ethernet_Generic 2.8.1` は Arduino-ESP32 core 3.x の
  `Server::begin(uint16_t)` を実装していないため、`src/main.cpp` 内の薄い
  ラッパ `TcpServer` で回避している (core へのパッチ不要)。
