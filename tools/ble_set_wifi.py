#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["bleak"]
# ///
"""BLE 経由で M5Cam に STA の Wi-Fi 接続先を登録し、繋がるまで広告を見て待つ。

  tools/ble_set_wifi.py HomeAP 'passphrase' [prio]   # 登録 / 更新（同じ SSID は上書き。prio 既定 10）
  tools/ble_set_wifi.py HomeAP ''                    # パスワードなしの AP
  tools/ble_set_wifi.py --del HomeAP                 # 削除
  tools/ble_set_wifi.py --list                       # 登録一覧だけ見る
  tools/ble_set_wifi.py --radio 'ap=0&sta=1'         # AP / STA のオンオフを NVS に保存（次の起動から）。reboot=1 を足すと再起動
  tools/ble_set_wifi.py --radio reboot=1             # 再起動だけ

複数台が見えるときは --id 445566（status_scan.py が出す M5Cam-XXXXXX）で選ぶ。
"""
import asyncio
import struct
import sys
import time
from urllib.parse import quote

from bleak import BleakClient, BleakScanner

from status_scan import COMPANY_ID, F_STA_CONFIGURED, F_STA_CONNECTED, decode, is_m5cam

WIFI = "5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0004"
CTRL = "5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0006"
APP_ERR = {0x80: "invalid (ssid required, ssid<=32, pass<=64)", 0x81: "full (max 8 networks)", 0x82: "not found"}


def status_of(ad):
    raw = ad.manufacturer_data.get(COMPANY_ID)
    return decode(raw) if raw else None


async def find(dev_id: str | None, timeout: float = 15):
    def ok(d, ad):
        if not is_m5cam(d, ad):
            return False
        if dev_id is None:
            return True
        st = status_of(ad)
        return st is not None and st.dev_id == dev_id

    dev = await BleakScanner.find_device_by_filter(ok, timeout=timeout)
    if dev is None:
        sys.exit("M5Cam not found" + (f" (id {dev_id})" if dev_id else ""))
    return dev


async def wait_for_sta(dev_id: str | None, timeout: float = 45) -> None:
    """登録後、広告の状態が「接続中（IP あり）」になるまで待って表示する"""
    print("waiting for STA to connect (watching advertisements)...", flush=True)
    seen = {}
    deadline = time.time() + timeout

    def on_adv(d, ad):
        st = status_of(ad)
        if st and (dev_id is None or st.dev_id == dev_id):
            seen[st.dev_id] = st

    async with BleakScanner(on_adv):
        while time.time() < deadline:
            await asyncio.sleep(1)
            for st in seen.values():
                if st.flags & F_STA_CONNECTED:
                    print(st.line())
                    return
    for st in seen.values():
        print(st.line())
        if not st.flags & F_STA_CONFIGURED:
            print("  -> device still reports no credentials; write may not have been saved")
        else:
            print("  -> not connected yet (wrong passphrase? out of range?). Keep watching with tools/status_scan.py")
    if not seen:
        print("  -> no status advertisement seen after disconnect")


async def main(argv: list[str]) -> None:
    dev_id = None
    if "--id" in argv:
        i = argv.index("--id")
        dev_id = argv[i + 1].lower()
        del argv[i:i + 2]
    if not argv:
        sys.exit(__doc__)

    if argv[0] == "--radio":
        dev = await find(dev_id)
        async with BleakClient(dev) as c:
            print(f"connected to {dev.name} ({dev.address})")
            print("before:", (await c.read_gatt_char(CTRL)).decode(errors="replace"))
            await c.write_gatt_char(CTRL, argv[1].encode(), response=True)
            if "reboot=1" in argv[1]:
                print("reboot requested; device will disconnect")
                return
            print("after: ", (await c.read_gatt_char(CTRL)).decode(errors="replace"))
        return
    if argv[0] == "--list":
        payload = None
    elif argv[0] == "--del":
        payload = f"ssid={quote(argv[1], safe='')}&del=1"
    else:
        ssid, pw = argv[0], argv[1] if len(argv) > 1 else ""
        prio = int(argv[2]) if len(argv) > 2 else 10
        payload = f"ssid={quote(ssid, safe='')}&pass={quote(pw, safe='')}&prio={prio}"

    dev = await find(dev_id)
    async with BleakClient(dev) as c:
        print(f"connected to {dev.name} ({dev.address})")
        if payload is not None:
            try:
                await c.write_gatt_char(WIFI, payload.encode(), response=True)
            except Exception as e:  # bleak は ATT エラーをプラットフォーム依存の例外にする
                code = next((k for k in APP_ERR if f"{k:#x}" in str(e).lower() or str(k) in str(e)), None)
                sys.exit(f"device rejected: {APP_ERR.get(code, str(e))}")
            print("deleted" if argv[0] == "--del" else "saved to NVS")
        print("registered:", (await c.read_gatt_char(WIFI)).decode(errors="replace"))
    if payload is not None and argv[0] != "--del":
        await wait_for_sta(dev_id)


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1:]))
