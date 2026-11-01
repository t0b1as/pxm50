#!/bin/bash
# Erstbuild ~45-90 min. Aufruf aus WSL2/Linux:  ./build.sh
set -e
EXT="$(cd "$(dirname "$0")" && pwd)"
BR="${BR:-$HOME/buildroot}"
OUT="$EXT/output"
# WSL: Windows-PATH mit Leerzeichen bricht Buildroot
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

[ -d "$BR" ] || git clone -b 2025.02.x https://gitlab.com/buildroot.org/buildroot.git "$BR"
[ -f "$OUT/.config" ] || make -C "$BR" BR2_EXTERNAL="$EXT" O="$OUT" pxm50_defconfig
make -C "$BR" O="$OUT" -j"$(nproc)"
ls -lh "$OUT/images/sdcard.img"
