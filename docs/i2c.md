# I2C インターフェース（開発者向け）

他のマイコン（マスター）から、M5Camera（スレーブ）に I2C で撮影を指示し、JPEG を受け取るための仕様です。
実装は `main/i2c_snap.c` / `main/i2c_snap.h` です。

## 概要

| 項目 | 値 |
|---|---|
| 役割 | M5Camera = I2C スレーブ / 相手 = マスター（マルチマスター非対応） |
| 7bit アドレス | `0x42`（`main/i2c_snap.c` の `I2C_ADDR`） |
| ピン | SDA = **IO4**、SCL = **IO13**（Grove / HY2.0-4P ポート） |
| 速度 | 100kHz / 400kHz |
| ロジックレベル | 3.3V |
| 取得できるもの | JPEG（解像度・品質・回転・グレースケールを指定可） |

カメラの SCCB（IO22/IO23）とは別のバスです。HTTP / BLE と同時に使えます（撮影は内部で順番に処理されます）。

## 配線

| Grove | 信号 | 備考 |
|---|---|---|
| 黒 | GND | 必須 |
| 赤 | 5V | **電源を片側だけにする**（下記） |
| 黄 | SCL (IO13) | |
| 白 | SDA (IO4) | |

- プルアップは、カメラ側に内蔵プルアップ（有効化済み）がある。400kHz で使う場合や配線が長い場合は、外付け（3.3V へ 2.2〜4.7kΩ）を推奨する。
- IO4 は、未実装のマイク（SPM1423）の CLK と同じピン。マイクを実装しないこと。

### 電源

- I2C の信号線は USB シリアル（CP2104）のピンと別なので、USB 接続中でも使える。
- 5V は USB と Grove が同じレールの可能性がある（回路図は未確認）。**電源は片側だけ**にする。
  - 開発中（USB 接続）: Grove は GND / SDA / SCL の 3 本だけ繋ぐ。
  - 運用時（ホストから給電）: USB を抜く。ホストの 5V 出力が、カメラ + Wi-Fi + BLE のピーク電流（数百 mA）を賄えること。
- 基板に DTR/RTS の自動リセット回路がある。USB シリアルを開くだけならリセットされないが、DTR/RTS を操作する（書き込み、`idf.py monitor` の既定動作など）と、カメラがリセットされて I2C の転送が途切れる。

## 通信モデル

ESP32 の I2C スレーブはクロックストレッチができません。そのため、1 回の要求を次の 2 段階にします。

```
マスター: [S][addr+W][コマンド…][P]      ← ① コマンドを書く
          （2ms 以上待つ）               ← ② スレーブが応答を用意する時間
マスター: [S][addr+R][応答…][P]          ← ③ 応答を読む
```

- スレーブは ① の STOP を受けてから応答を用意します。② の待ちが短いと、応答が用意されていない状態で読むことになり、先頭が `0xA5` にならずエラーになります。**待ちは 2ms 以上**（実装では 3ms を使う）。
- 応答は、書いた直後に 1 回だけ読む。読まなかった応答は、次のコマンドの応答を用意するときに破棄される。
- 応答の読み出し長は、後述のフレーム長ちょうどにする。

## コマンド

コマンドの先頭 1 バイトがコマンド番号です。未定義の番号は無視されます（応答なし）。

### 0x01 SNAP: 撮影開始

```
書き込み: [0x01][クエリ文字列（ASCII、省略可、最大 126 文字）]
```

- 応答はない。撮影は非同期に行われ、完了は STATUS で確認する。
- クエリは HTTP の `/jpg` と同じ書式。省略したキーは既定値になる。

| キー | 意味 | 既定（I2C） |
|---|---|---|
| `w`, `h` | 撮影サイズ(px)。片方だけなら 4:3 で補完 | 160, 120 |
| `q` | 品質 0〜100（大きいほど高画質、0 が最小サイズ） | 20 |
| `rot` | 時計回りに 0 / 90 / 180 / 270。90・270 は縦横が入れ替わる | 0 |
| `gray` | 1 でグレースケール | 0 |
| `fit` | `stretch` / `crop` | stretch |

- 撮影中（state=1）に SNAP を送ると無視される。
- 前回の画像は、新しい SNAP を受けた時点で破棄される。画像は次の SNAP まで保持され、何度でも読み出せる。
- 撮影に失敗した（パラメータ不正など）場合は state=3 になる。詳細な理由はシリアルログ（`i2c: snap failed: …`）にだけ出る。

### 0x02 STATUS: 状態の取得

```
書き込み: [0x02]
読み出し: 11 バイト
  [0]    0xA5                 同期バイト
  [1]    state                0=画像なし 1=撮影中 2=完了 3=エラー
  [2..5] total  (u32 LE)      JPEG の全長（state=2 のときだけ有効。それ以外は 0）
  [6..7] width  (u16 LE)      出力画像の幅（同上）
  [8..9] height (u16 LE)      出力画像の高さ（同上）
  [10]   crc8                 [1]〜[9] の crc8
```

- state=2 になるまで 10〜20ms 間隔で繰り返す。撮影時間の目安は、センサーの解像度に一致するサイズで 0.1〜0.3 秒、リサイズ・回転・グレースケールを伴うと 0.3〜0.7 秒。HTTP や BLE の撮影と重なると、その分待たされる。

### 0x03 READ: 画像データの読み出し

```
書き込み: [0x03][offset u32 LE][n u8]
読み出し: n + 3 バイト
  [0]       0xA5
  [1]       n_actual            実際に返したバイト数
  [2..]     data × n_actual     JPEG の offset 以降
  [n+2]     crc8                [1]〜[n+1] の crc8
```

- `n` は 1〜120。ESP32 の TX FIFO が 32 バイトなので、**29 以下を推奨**（1 回の読み出しが 32 バイトに収まる）。30 以上は、FIFO の補充が間に合うよう 100kHz で使う。
- スレーブは `n_actual = min(n, 120, total - offset)` を返す。最後のチャンクは、マスター側で `n = total - offset` にして、読み出し長を `n + 3` に合わせる。
- state が 2 でないとき、または `offset >= total` のときは `n_actual = 0` で、フレームは `[0xA5][0x00][0x00]`（3 バイト）になる。

## CRC

CRC-8、多項式 `0x07`、初期値 `0x00`、反転・XOR 出力なし。

```c
uint8_t crc8(const uint8_t *p, size_t n) {
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 0x80) ? (c << 1) ^ 0x07 : c << 1;
    }
    return c;
}
```

## 通信例

`w=96&h=96&q=0&gray=1` で撮影し、JPEG が 600 バイトだった場合。

```
① 書き込み  01 77 3D 39 36 26 68 3D 39 36 26 71 3D 30 26 67 72 61 79 3D 31
              ('w=96&h=96&q=0&gray=1' に先頭の 0x01 を付けたもの)
   ─ 撮影中。20ms 待つ ─
② 書き込み  02          → 3ms 待つ → 読み出し 11 バイト
   撮影中:  A5 01 00 00 00 00 00 00 00 00 79
   完了:    A5 02 58 02 00 00 60 00 60 00 98
            │  │  └ total=0x0258=600  └ 96x96   └ crc8
            │  └ state=2（完了）
            └ 同期バイト
③ 書き込み  03 00 00 00 00 1D   (offset=0, n=29) → 3ms 待つ → 読み出し 32 バイト
   応答:    A5 1D FF D8 FF ... (JPEG の先頭 29 バイト) crc8
   以降 offset=29, 58, ... と進め、最後は n = 600 - offset
```

## マスターの実装例（C、擬似コード）

`i2c_write()` / `i2c_read()` は、マスター側のプラットフォームの I2C 送受信関数に置き換える（STOP 条件で終わること）。

```c
#define ADDR  0x42
#define CHUNK 29

bool transact(const uint8_t *cmd, size_t cl, uint8_t *resp, size_t rl) {
    for (int retry = 0; retry < 5; retry++) {
        if (i2c_write(ADDR, cmd, cl)) {
            delay_ms(3);
            if (i2c_read(ADDR, resp, rl) && resp[0] == 0xA5 && crc8(resp + 1, rl - 2) == resp[rl - 1])
                return true;
        }
        delay_ms(3);
    }
    return false;
}

// 成功すると buf に JPEG を書き、長さを返す。失敗は -1
int snap(const char *query, uint8_t *buf, size_t buf_size) {
    uint8_t cmd[1 + 126] = {0x01};
    size_t ql = strlen(query);
    memcpy(cmd + 1, query, ql);
    if (!i2c_write(ADDR, cmd, 1 + ql)) return -1;

    uint8_t st[11];
    for (int t = 0;; t++) {                    // 完了待ち
        delay_ms(20);
        if (transact((uint8_t[]){0x02}, 1, st, 11)) {
            if (st[1] == 2) break;             // 完了
            if (st[1] == 3) return -1;         // エラー
        }
        if (t > 250) return -1;                // 5 秒でタイムアウト
    }
    uint32_t total = st[2] | st[3] << 8 | st[4] << 16 | (uint32_t)st[5] << 24;
    if (total > buf_size) return -1;

    for (uint32_t off = 0; off < total;) {
        uint8_t n = total - off < CHUNK ? total - off : CHUNK;
        uint8_t rq[6] = {0x03, off, off >> 8, off >> 16, off >> 24, n};
        uint8_t fr[CHUNK + 3];
        if (!transact(rq, 6, fr, n + 3) || fr[1] != n) return -1;
        memcpy(buf + off, fr + 2, n);
        off += n;
    }
    return total;
}
```

## 転送時間の目安

読み出し 1 回あたり、`(書き込み 7 バイト + 読み出し n+3 バイト) × 9 クロック / SCL 周波数 + 待ち 3ms` として計算した値です。

| SCL | n | 1 チャンクの時間 | 実効速度 |
|---|---|---|---|
| 100kHz | 29 | 約 6.5ms | 約 4.4 KB/s |
| 100kHz | 120 | 約 14ms | 約 8.5 KB/s |
| 400kHz | 29 | 約 4.1ms | 約 7 KB/s |

待ち時間（3ms）の割合が大きいので、チャンクを大きくするほど速くなります。600 バイトの画像（96x96 グレースケール）なら、100kHz・n=29 で約 0.14 秒です。

## ファームウェアの内部構造

```
I2C 割り込み(STOP) ─ on_receive (IRAM) ─ xQueueSendFromISR ─▶ i2c_cmd タスク
                                                              │ SNAP   → 撮影キュー ─▶ i2c_snap タスク → snap_capture()
                                                              │ STATUS → i2c_slave_write(11 バイト)
                                                              └ READ   → i2c_slave_write(n+3 バイト)
```

- `on_receive` は、1 回の書き込みトランザクション（STOP まで）ごとに呼ばれ、受信したコマンドをそのままキューに入れる（最大 128 バイト）。
- 応答は `i2c_slave_write()` で TX FIFO に積む。ESP32 は、FIFO に未送信データがあると `i2c_slave_write()` の呼び出しで FIFO をリセットするため、読まれなかった応答は次の応答で上書きされる。
- 撮影は `i2c_snap` タスクで行い、`snap_capture()` の内部ミューテックスで HTTP / BLE の撮影と直列化される。
- 保持する画像は 1 枚。メモリ上の JPEG（PSRAM またはヒープ）を、次の SNAP まで持つ。

## 変更するとき

| 変えたいもの | 場所 |
|---|---|
| アドレス | `main/i2c_snap.c` の `I2C_ADDR` |
| ピン | 同 `PIN_SDA` / `PIN_SCL`（Grove ポート以外にする場合は、カメラ・LED のピンと重ならないこと） |
| 既定の撮影パラメータ | 同 `I2C_DEF_W` / `I2C_DEF_H` / `I2C_DEF_Q` |
| 1 回の最大チャンク | 同 `CHUNK_MAX`（FIFO の制約があるので 29 を超える場合は低速で使う） |
