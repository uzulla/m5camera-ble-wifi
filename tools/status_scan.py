#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["bleak"]
# ///
"""接続せずに M5Cam の状態（STA の IP など）を一覧する。

  tools/status_scan.py            # BLE 広告をスキャン（macOS / Linux。既定 6 秒）
  tools/status_scan.py ble 10     # スキャン時間を指定
  tools/status_scan.py wifi wlp1s0   # Wi-Fi ビーコンのベンダー IE を読む（Linux。iw か iwlist。root でなければ sudo -n）

バイト列の形式は main/status_adv.h を参照。
"""
import asyncio
import re
import shutil
import struct
import subprocess
import sys
import time
from dataclasses import dataclass

SVC = "5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0001"
COMPANY_ID = 0xFFFF
WIFI_OUI = "5a:3c:00"
WIFI_OUI_TYPE = 0x01
FIXED = struct.Struct("<BB4s3sbHBB")

F_STA_CONNECTED, F_STA_CONFIGURED, F_CAMERA_OK, F_AP_CLIENTS, F_BUSY = 0x01, 0x02, 0x04, 0x08, 0x10
F_AP_OFF, F_STA_OFF = 0x20, 0x40


@dataclass
class Status:
    flags: int
    ip: str
    dev_id: str
    sta_rssi: int
    uptime_min: int
    heap_kb: int
    ap_clients: int
    ssid: str

    def line(self) -> str:
        f = self.flags
        if f & F_STA_OFF:
            sta = "STA off" + ("" if f & F_AP_OFF else " (AP only, 192.168.4.1)")
        elif f & F_STA_CONNECTED:
            sta = f"STA {self.ip} ({self.sta_rssi} dBm) \"{self.ssid}\""
        elif f & F_STA_CONFIGURED:
            sta = f"STA connecting to \"{self.ssid}\"..."
        else:
            sta = "STA not configured (AP only, 192.168.4.1)"
        extras = [
            "camera ok" if f & F_CAMERA_OK else "NO CAMERA",
            *(["AP off"] if f & F_AP_OFF else []),
            f"ap_clients {self.ap_clients}",
            f"heap {self.heap_kb}K",
            f"up {self.uptime_min // 60}h{self.uptime_min % 60:02d}m",
        ]
        if f & F_BUSY:
            extras.append("busy")
        return f"M5Cam-{self.dev_id}  {sta}  |  " + "  ".join(extras)


def decode(raw: bytes) -> Status | None:
    if len(raw) < FIXED.size or raw[0] != 1:
        return None
    _ver, flags, ip, mac3, rssi, up, heap, ncl = FIXED.unpack_from(raw)
    ssid = raw[FIXED.size:].split(b"\0")[0].decode(errors="replace")
    return Status(flags, ".".join(map(str, ip)), mac3.hex(), rssi, up, heap, ncl, ssid)


async def scan_ble(seconds: float) -> None:
    from bleak import BleakScanner

    found: dict[str, tuple[Status, int]] = {}

    def on_adv(dev, ad):
        if SVC not in [u.lower() for u in ad.service_uuids]:
            return
        raw = ad.manufacturer_data.get(COMPANY_ID)
        st = decode(raw) if raw else None
        if st:
            found[st.dev_id] = (st, ad.rssi)

    async with BleakScanner(on_adv):
        await asyncio.sleep(seconds)
    if not found:
        sys.exit("no M5Cam status advertisement found")
    for st, rssi in sorted(found.values(), key=lambda x: x[0].dev_id):
        print(f"[BLE {rssi} dBm] {st.line()}")


def _which(name: str) -> str | None:
    import os

    # iw / iwlist は /usr/sbin にあり、一般ユーザーの PATH に無いことが多い
    return shutil.which(name) or shutil.which(name, path=os.pathsep.join(["/usr/sbin", "/sbin", "/usr/bin", "/bin"]))


def _run_scan(cmd: list[str]) -> str:
    import os

    cmd[0] = _which(cmd[0]) or cmd[0]
    if os.geteuid() != 0:
        cmd = ["sudo", "-n"] + cmd
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"{cmd[-3]} failed: {out.stderr.strip()}")
    return out.stdout


def _report(bssid: str, sig: str, oui: str, data: bytes) -> bool:
    """oui は小文字 'aa:bb:cc'、data は OUI の次（type）から。該当すれば表示して True"""
    if oui != WIFI_OUI or not data or data[0] != WIFI_OUI_TYPE:
        return False
    st = decode(data[1:])
    if not st:
        return False
    print(f"[Wi-Fi {bssid} {sig} dBm] {st.line()}")
    return True


def scan_wifi(iface: str) -> None:
    n = 0
    if _which("iw"):
        out = _run_scan(["iw", "dev", iface, "scan"])
        for block in re.split(r"^BSS ", out, flags=re.M)[1:]:
            bssid = block.split("(", 1)[0].strip()
            sig = re.search(r"signal: (-?[\d.]+) dBm", block)
            for m in re.finditer(r"Vendor specific: OUI ([0-9a-f:]+), data: ([0-9a-f ]+)", block):
                data = bytes.fromhex(m.group(2).replace(" ", ""))
                n += _report(bssid, sig.group(1) if sig else "?", m.group(1), data)
    elif _which("iwlist"):
        # wireless-tools の iwlist は未知の IE を "IE: Unknown: DD<len><OUI><type><payload>" の 16 進で出す
        out = _run_scan(["iwlist", iface, "scanning"])
        for block in re.split(r"^\s*Cell \d+ - ", out, flags=re.M)[1:]:
            bssid = re.search(r"Address: ([0-9A-Fa-f:]+)", block)
            sig = re.search(r"Signal level=(-?\d+) dBm", block)
            for m in re.finditer(r"IE: Unknown: DD([0-9A-Fa-f]+)", block):
                ie = bytes.fromhex(m.group(1))   # [len][oui 3][type][payload]
                if len(ie) < 5:
                    continue
                oui = ":".join(f"{b:02x}" for b in ie[1:4])
                n += _report(bssid.group(1).lower() if bssid else "?", sig.group(1) if sig else "?", oui, ie[4:1 + ie[0]])
    else:
        sys.exit("neither iw nor iwlist found (Debian/Ubuntu: sudo apt install iw)")
    if n == 0:
        sys.exit("no M5Cam beacon with status IE found")


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "ble"
    if mode == "ble":
        asyncio.run(scan_ble(float(sys.argv[2]) if len(sys.argv) > 2 else 6))
    elif mode == "wifi":
        scan_wifi(sys.argv[2] if len(sys.argv) > 2 else "wlan0")
    else:
        sys.exit(__doc__)
