# powerhub_i2c_master

M5Stack PowerHub (ESP32-S3) を I2C マスターにして、M5Camera から JPEG を取得する ESP-IDF のサンプル。
通信仕様は [`docs/i2c.md`](../../docs/i2c.md)。

- 接続: PowerHub の PORT.A（赤い Grove）と、M5Camera の Grove 端子を Grove ケーブルで接続する。
  PowerHub 側の PORT.A の出力を ON にしておく（BtnSEL で PORT.I2C を選び、BtnOK で出力 ON）。
  PowerHub の SDA は G15、SCL は G16。カメラ側に USB を挿すときは、5V が二重にならないようにする。
- 設定: `main/main.c` 先頭の `PIN_SDA` / `PIN_SCL` / `SCL_HZ` / `CHUNK` / `WAIT_US` / `QUERY`。
  既定は 1MHz・チャンク 120・待ち 1ms で、1024x768 の JPEG を約 0.5 秒で読み出せる。
- ビルドと書き込み: `idf.py set-target esp32s3`（`sdkconfig.defaults` で指定済み）→ `idf.py -p /dev/ttyACM0 flash`。
- 画像の取り出し: 10 秒ごとに撮影し、JPEG を base64 でシリアル（USB Serial/JTAG）に出力する。
  `./decode_jpeg.py /dev/ttyACM0 out.jpg` で `JPEG_BEGIN`〜`JPEG_END` を JPEG に復元する（`uv` が必要）。
