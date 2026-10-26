#!/bin/bash
# ローカルのソースを buildhost に同期し、buildhost 上でビルド・書き込みする。
# 使い方: tools/sync.sh [build|flash]   （省略時は同期のみ）
set -euo pipefail
cd "$(dirname "$0")/.."
HOST=${HOST:-$(cat .synchost 2>/dev/null || echo buildhost)}   # 同期先ホスト。環境変数 HOST か、リポジトリ直下の .synchost（git 管理外）で指定
DIR=m5_jpg_snapshot
PORT=${PORT:-/dev/ttyUSB0}
ssh "$HOST" "mkdir -p ~/$DIR"
rsync -a --delete --exclude build --exclude managed_components --exclude backup --exclude sdkconfig \
  --exclude sdkconfig.old --exclude dependencies.lock ./ "$HOST:$DIR/"
IDF=". ~/esp/esp-idf-v6.1/export.sh >/dev/null 2>&1; cd ~/$DIR"
case "${1:-}" in
  build) ssh "$HOST" "$IDF; idf.py build 2>&1 | grep -E ': (error|warning):|undefined reference|^FAILED|Project build|smallest app|Error' | head -60" ;;
  flash) ssh "$HOST" "$IDF; idf.py -p $PORT flash 2>&1 | tail -15" ;;
esac
