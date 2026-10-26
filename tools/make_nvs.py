#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["esp-idf-nvs-partition-gen"]
# ///
"""STA 用の Wi-Fi 接続情報を NVS パーティションのイメージ(nvs.bin)にする。

使い方（--net は SSID PASSWORD PRIORITY。優先度は大きいほど先に試す。オープンな AP は PASSWORD を "" に）:
  tools/make_nvs.py nvs.bin --net HomeAP 'pass word' 10 --net Phone 'secret' 5
書き込み（NVS は 0x9000, 0x6000 バイト。以前の NVS の内容は消える）:
  esptool -p /dev/ttyUSB0 write-flash 0x9000 nvs.bin

最大 8 件。実行中のデバイスには HTTP でも登録できる（README 参照）。
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

NVS_SIZE = 0x6000
MAX_ENTRIES = 8


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", help="出力する nvs.bin")
    ap.add_argument("--net", nargs=3, action="append", metavar=("SSID", "PASSWORD", "PRIORITY"), required=True)
    args = ap.parse_args()
    if len(args.net) > MAX_ENTRIES:
        sys.exit(f"max {MAX_ENTRIES} networks")

    rows = ["key,type,encoding,value", "wifi,namespace,,"]
    for i, (ssid, password, prio) in enumerate(args.net):
        if not ssid or len(ssid.encode()) > 32 or len(password.encode()) > 64:
            sys.exit(f"invalid ssid/password length: {ssid!r}")
        for field in (ssid, password):
            if '"' in field or "\n" in field:
                sys.exit("ssid/password must not contain double quotes or newlines")
        rows.append(f'ssid{i},data,string,"{ssid}"')
        if password:   # オープンな AP はパスワードのキーを書かない
            rows.append(f'pass{i},data,string,"{password}"')
        rows.append(f"prio{i},data,i32,{int(prio)}")

    with tempfile.TemporaryDirectory() as d:
        csv = Path(d) / "wifi.csv"
        csv.write_text("\n".join(rows) + "\n")
        subprocess.run(
            [sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate", str(csv), args.out, hex(NVS_SIZE)],
            check=True,
        )
    print(f"wrote {args.out} ({len(args.net)} networks)")


if __name__ == "__main__":
    main()
