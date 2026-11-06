#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["bleak"]
# ///
"""BLE 経由で M5Cam のスナップを取得して保存する。

使い方:
  tools/ble_snap.py out.jpg                      # 既定（160x120, q=20）
  tools/ble_snap.py out.jpg "w=96&h=96&q=0&gray=1&rot=90"
クエリは HTTP の /jpg と同じ書式。
"""
import asyncio
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

SVC = "5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0001"
REQ = "5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0002"
DATA = "5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0003"


async def main(path: str, query: str) -> None:
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or ad.local_name or "").startswith("M5Cam") or SVC in [u.lower() for u in ad.service_uuids],
        timeout=15,
    )
    if dev is None:
        sys.exit("M5Cam not found")
    chunks: dict[int, bytes] = {}
    done = asyncio.Event()
    err: list[str] = []
    total = 0

    def on_data(_, data: bytearray) -> None:
        nonlocal total
        seq = struct.unpack_from("<H", data)[0]
        if seq == 0xFFFF:
            err.append(bytes(data[2:]).decode(errors="replace"))
            done.set()
            return
        payload = bytes(data[2:])
        if seq == 0:
            total = struct.unpack_from("<I", payload)[0]
            payload = payload[4:]
        chunks[seq] = payload
        if total and sum(len(c) for c in chunks.values()) >= total:
            done.set()

    async with BleakClient(dev) as c:
        print(f"connected to {dev.name} ({dev.address}), mtu={c.mtu_size}")
        await c.start_notify(DATA, on_data)
        t0 = time.time()
        await c.write_gatt_char(REQ, query.encode(), response=True)
        await asyncio.wait_for(done.wait(), timeout=120)
        dt = time.time() - t0
    if err:
        sys.exit(f"error from device: {err[0]}")
    jpg = b"".join(chunks[i] for i in sorted(chunks))[:total]
    open(path, "wb").write(jpg)
    print(f"saved {path}: {len(jpg)} bytes in {dt:.2f}s ({len(jpg) / dt / 1024:.1f} KiB/s)")


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else "snap.jpg", sys.argv[2] if len(sys.argv) > 2 else ""))
