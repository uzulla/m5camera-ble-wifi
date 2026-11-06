#!/bin/bash
# web/ を GitHub Pages（gh-pages ブランチ）に公開する。main に push した後に実行する。
#   https://uzulla.github.io/m5camera-ble-wifi/
set -euo pipefail
cd "$(dirname "$0")/.."
git subtree push --prefix web origin gh-pages
