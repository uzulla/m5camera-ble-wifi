# M5Camera JPEG スナップ（ESP-IDF）

M5Stack の **M5Camera**（ESP32 / OV2640 / PSRAM 4MB / CP2104）で、Wi-Fi（AP + STA）と BLE から JPEG のスナップ・ストリームを取得するファームウェアです。

- ビルド環境: ESP-IDF v6.1（buildhost の `~/esp/esp-idf-v6.1`）。`esp32-camera` はコンポーネントマネージャが取得します。
- 作業場所: ローカルの `m5_jpg_snapshot/` が正で、buildhost の `~/m5_jpg_snapshot/` に同期してビルド・書き込みします。

```bash
tools/sync.sh          # buildhost に同期のみ
tools/sync.sh build    # 同期してビルド
tools/sync.sh flash    # 同期してビルド・書き込み（PORT=/dev/ttyUSB0）
```

`backup/` に工場出荷ファームの全フラッシュイメージ（4MB）があります。戻すときは `esptool write-flash 0 backup/m5camera_112233445566_factory_full.bin`。

## ネットワーク

| | |
|---|---|
| AP | SSID `M5Cam`（パスワードなし）、`192.168.4.1` |
| STA | NVS に登録した Wi-Fi に接続。**複数登録でき、Priority の大きい順に試し、失敗したら次へフォールバック** |

STA が切れると、Priority の先頭から試し直します。接続中に登録を変更したときも同様です。AP のチャンネルは STA の接続先に追従します（ESP32 の仕様）。
STA の IP は、接続せずに読める **状態の広告**（後述。BLE 広告と AP ビーコンに載せている）、`/status`、シリアルログ（`STA got IP`）のいずれかで分かります。

> AP はパスワードなしで、`/wifi` も認証なしです。信頼できる場所でのみ使ってください。

## HTTP API

| パス | 内容 |
|---|---|
| `GET /jpg` | スナップ（JPEG） |
| `GET /jpg_stream` | `multipart/x-mixed-replace` のストリーム。`fps=` で上限指定 |
| `GET /cpu` | CPU 使用率（JSON） |
| `GET /status` | 稼働時間、メモリ、AP/STA の状態。AP と STA の MAC、STA の IP/netmask/gw、AP に接続中のクライアントの MAC・DHCP で払い出した IP・RSSI |
| `GET /wifi` | 登録済みの STA 接続先（パスワードは返さない） |
| `POST /wifi?ssid=&pass=&prio=` | 接続先を追加 / 更新（同じ SSID は上書き。最大 8 件） |
| `DELETE /wifi?ssid=` | 接続先を削除 |

### 画像のパラメータ（`/jpg` `/jpg_stream` と BLE で共通）

| 名前 | 意味 | 既定 |
|---|---|---|
| `w`, `h` | **撮影サイズ**(px)。片方だけなら 4:3 で補完 | 640x480（BLE は 160x120） |
| `q` | 品質 0〜100。大きいほど高画質、**0 が最小サイズ** | 50（BLE は 20） |
| `rot` | 時計回りに 0 / 90 / 180 / 270 回転。90・270 は縦横が入れ替わって返る | 0 |
| `gray` | 1 でグレースケール（さらに小さくなる） | 0 |
| `fit` | `stretch`（既定。縦横比を無視して合わせる）/ `crop`（中央を切り出す） | stretch |

```bash
curl 'http://192.168.4.1/jpg?w=320&h=240&q=60' -o snap.jpg
curl 'http://192.168.4.1/jpg?w=1600&h=1200&q=100' -o best.jpg           # 最高画質（トップページ `/` にもリンクあり）
curl 'http://192.168.4.1/jpg?w=320&h=240&rot=90' -o rotated.jpg          # 240x320 が返る
curl 'http://192.168.4.1/jpg?w=96&h=96&q=0&gray=1&fit=crop' -o tiny.jpg  # とにかく小さく（600 バイト前後）
```

応答ヘッダの `X-Mode`（`native` / `soft`）と `X-Size`（出力サイズ）で、どちらの経路で作ったかが分かります。

- **native**: センサーが出力した JPEG をそのまま返す。速い（ストリームで実用的）。条件は、`w`×`h` がセンサーの解像度（160x120, 240x176, 320x240, 400x296, 640x480, 800x600, 1024x768, 1280x1024, 1600x1200）に一致し、`gray=0`、`rot` が 0 か 180（180 はセンサーの反転で行う）であること。
- **soft**: デコード → リサイズ・回転・グレースケール → 再エンコード。任意サイズが指定できるが遅い（QVGA で 0.3〜0.6 秒程度）。撮影サイズは 800x600 まで。

### CPU 使用率 `GET /cpu`

```json
{"total":23.4,"core0":31.0,"core1":15.8,"window_ms":1000,"free_heap":61000,"free_psram":2900000,
 "tasks":[{"name":"stream","cpu":12.3}, ...]}
```

直近 1 秒の使用率（%）です。`core0` / `core1` は各コアの使用率、`total` はその平均、`tasks` の `cpu` は 1 コア分を 100% としたタスクごとの使用率です。

### `GET /status`

```json
{"uptime_s":120,"camera":true,"free_heap":61000,"free_psram":2900000,
 "ap":{"ssid":"M5Cam","ip":"192.168.4.1","mac":"11:22:33:44:55:67",
       "clients":[{"mac":"aa:bb:cc:dd:ee:ff","ip":"192.168.4.2","rssi":-50}]},
 "sta":{"connected":true,"ssid":"HomeAP","mac":"11:22:33:44:55:66","ip":"192.168.1.20",
        "netmask":"255.255.255.0","gw":"192.168.1.1","rssi":-60}}
```

## STA の Wi-Fi 設定（NVS）

NVS（名前空間 `wifi`）のキー: `ssid<N>` / `pass<N>` / `prio<N>`（N = 0〜7、`prio` は i32、パスワードなしの AP は `pass<N>` を省略）。

**A. NVS イメージを作って書き込む**（ファームの書き込みとは独立。以前の NVS の内容は消える）

```bash
tools/make_nvs.py nvs.bin --net HomeAP 'password' 10 --net Phone 'secret' 5
scp nvs.bin buildhost:/tmp/
ssh buildhost '~/.espressif/python_env/idf6.1_py3.13_env/bin/esptool -p /dev/ttyUSB0 write-flash 0x9000 /tmp/nvs.bin'
```

**B. 動いているデバイスに HTTP で登録する**（AP の `192.168.4.1` から）

```bash
curl -X POST 'http://192.168.4.1/wifi?ssid=HomeAP&pass=password&prio=10'
```

SSID / パスワードの特殊文字は URL エンコードしてください。

## BLE

デバイス名 `M5Cam`。低速（実測 5〜15 KiB/s）ですが、小さい画像なら 1 秒以内で取れます。

| | UUID |
|---|---|
| Service | `5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0001` |
| REQ（Write） | `5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0002` — `w=160&h=120&q=20&rot=90&gray=1` のように HTTP と同じクエリを書く（空でも可） |
| DATA（Notify） | `5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0003` — `[seq u16 LE][payload]` |

DATA を購読してから REQ に書くと、撮影後に通知で届きます。`seq=0` の payload は `[JPEG 全長 u32 LE][JPEG の先頭]`、以降は続き。`seq=0xFFFF` はエラーメッセージ（ASCII）です。

```bash
tools/ble_snap.py out.jpg "w=96&h=96&q=0&gray=1"   # macOS / Linux。uv が bleak を入れる
```

## 状態の広告（BLE 広告 / Wi-Fi ビーコン）

画面がない機体の状態（STA の IP、接続先、カメラの可否など）を、**接続せずに読める場所**に 5 秒ごとに載せています。

- **BLE 広告の scan response**: Manufacturer Specific Data（会社 ID `0xFFFF`）。GATT 接続中も接続不可の広告（ADV_SCAN_IND、500〜1000ms 間隔）で見え続けます
- **AP のビーコン / probe response**: ベンダー固有 IE（OUI `5a:3c:00`、type `0x01`）。AP を再起動せずに差し替えています

```bash
tools/status_scan.py              # BLE をスキャン（macOS / Linux。uv が bleak を入れる）
tools/status_scan.py wifi wlp1s0  # Wi-Fi ビーコンを読む（Linux。iw か iwlist（wireless-tools）。root でなければ sudo -n）
# [BLE -54 dBm] M5Cam-445566  STA 192.168.1.23 (-58 dBm) "HomeAP"  |  camera ok  ap_clients 0  heap 51K  up 0h03m
```

バイト列（BLE では 27 バイトに切るので SSID は 13 バイトまで。Wi-Fi は SSID 全長）:

| オフセット | サイズ | 内容 |
|---|---|---|
| 0 | 1 | ver = 1 |
| 1 | 1 | flags: bit0 STA 接続中 / bit1 NVS に登録あり / bit2 カメラ OK / bit3 AP にクライアント有り / bit4 撮影中 |
| 2 | 4 | STA の IPv4（a.b.c.d の順。未接続なら 0） |
| 6 | 3 | STA MAC の下位 3 バイト（個体識別。macOS は BLE アドレスを隠すため） |
| 9 | 1 | STA の RSSI (int8) |
| 10 | 2 | 稼働時間（分, u16 LE） |
| 12 | 1 | 空き内部ヒープ (KiB) |
| 13 | 1 | AP に接続中のクライアント数 |
| 14 | 可変 | SSID（接続中はその SSID、未接続なら接続を試みている SSID） |

IP と SSID は平文で周囲に撒かれます。AP が無パスワードなのと同じく、信頼できる場所で使う前提です。

## I2C（他のマイコンから取得）

Grove ポート（**SDA=IO4, SCL=IO13**）の I2C スレーブ（7bit アドレス `0x42`）として動作します。カメラの SCCB（IO22/23）とは別のバスです。
ESP32 のスレーブはクロックストレッチができないため、**「コマンドを書く → 1ms 以上待つ → 応答を読む」の 2 段階**です。

| 手順 | マスターの操作 | 内容 |
|---|---|---|
| 1 | 書き込み `[0x01]` + クエリ文字列 | 撮影開始。クエリは HTTP の `/jpg` と同じ（空なら 160x120, q=20） |
| 2 | 書き込み `[0x02]` → 11 バイト読み出し | STATUS: `[0xA5][state][total u32][w u16][h u16][crc8]`。state は 0=なし 1=撮影中 2=完了 3=エラー。2 になるまで繰り返す |
| 3 | 書き込み `[0x03][offset u32][n u8]` → n+3 バイト読み出し | DATA: `[0xA5][n][data × n][crc8]` |
| 4 | offset を進めて total まで 3 を繰り返す | |

crc8 は多項式 0x07・初期値 0 で、`0xA5` の次のバイトから crc8 の直前までが対象です。先頭が `0xA5` でない / crc が合わないときは同じ要求をやり直します（`n` は 120 を推奨。1MHz・待ち 1ms で 1024x768 の JPEG（約 25KB）を約 0.5 秒で読める。詳細は docs/i2c.md の「転送時間」）。

### 電源（USB と Grove の同時接続）

I2C の信号線（IO4/13）は USB シリアル（CP2104）のピンと別なので、データ線は USB と同時に使えます。**5V ラインは USB と Grove が同じレールかもしれない**（回路図は未確認）ため、電源は片側だけにしてください。

- 開発中（USB 接続）: Grove ケーブルは GND / SDA / SCL の 3 本だけ繋ぐ（5V を外す）
- 運用時（ホストから給電）: USB を抜く。ホストの 5V 出力が、カメラ + Wi-Fi + BLE のピーク電流（数百 mA）を賄えるか確認する
- 基板に DTR/RTS の自動リセット回路がある。**ポートを開くだけ（DTR/RTS を触らない）ならリセットされない**が、DTR/RTS を両方オフにするなどの操作（書き込みや一部のモニタ）でカメラがリセットされ、I2C の転送も途切れる。ログを読むだけなら DTR/RTS を触らない（`idf.py monitor` は `--no-reset`）
- HTTP / BLE / I2C は同時に使えます（撮影は内部で直列化されます）

詳細（配線、バイト列の例、マスターの実装例、内部構造）は [docs/i2c.md](docs/i2c.md)、マスターのサンプルは [examples/powerhub_i2c_master/](examples/powerhub_i2c_master/) にあります。

## 構成

| ファイル | 役割 |
|---|---|
| `main/snap.c` | カメラ初期化、パラメータ解析、native / soft の撮影 |
| `main/wifi_mgr.c` | AP + STA、NVS の接続先管理、優先度フォールバック |
| `main/http_srv.c` | HTTP サーバー（ストリームは別タスクで配信し、他のリクエストを塞がない） |
| `main/ble_snap.c` | NimBLE の GATT サービス、広告（scan response に状態） |
| `main/status_adv.c` | 状態のバイト列を組み立て、BLE 広告と AP のベンダー IE に反映 |
| `main/i2c_snap.c` | I2C スレーブ（他のマイコンからの取得） |
| `main/cpu_mon.c` | FreeRTOS の実行時間統計から CPU 使用率を算出 |
| `docs/i2c.md` | I2C の開発者向け資料 |
| `examples/powerhub_i2c_master/` | I2C マスターのサンプル（PowerHub / ESP-IDF） |
| `tools/` | `sync.sh`（buildhost へ同期・ビルド）、`make_nvs.py`、`ble_snap.py`、`status_scan.py`（状態の広告を読む） |

カメラのピン: XCLK 27 / SIOD 22 / SIOC 23 / VSYNC 25 / HREF 26 / PCLK 21 / RESET 15 / D0〜D7 = 32, 35, 34, 5, 39, 18, 36, 19。
