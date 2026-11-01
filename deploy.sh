#!/bin/bash
# zImage + DTBs + Overlay + tslib/evtest + pxm50-panel per SSH aufs laufende Panel, dann Reboot.
# Aufruf: ./deploy.sh <IP>
# Tipp gegen Passwort-Spam: ssh root@IP 'mkdir -p ~/.ssh; cat >> ~/.ssh/authorized_keys' < ~/.ssh/id_ed25519.pub
set -e
IP="${1:?Aufruf: ./deploy.sh <IP>}"
EXT="$(cd "$(dirname "$0")" && pwd)"
IMG="$EXT/output/images"
TGT="$EXT/output/target"
OV="$EXT/board/pxm50/rootfs-overlay"
S="ssh -o StrictHostKeyChecking=accept-new root@$IP"

# Boot-Partition: zImage + DTBs
tar -C "$IMG" -cf - zImage am335x-pxm50.dtb am335x-pxm50-int22.dtb \
| $S 'mount /dev/mmcblk0p1 /mnt 2>/dev/null; tar -C /mnt -xf - && sync && umount /mnt'

# Rootfs: Overlay komplett + tslib/evtest aus dem Build
( cd "$TGT" && ls -d usr/lib/libts* usr/lib/ts usr/bin/ts_* usr/bin/evtest usr/bin/pxm50-panel usr/lib/libcurl* usr/lib/libcjson* usr/lib/libjpeg* usr/lib/libturbojpeg* usr/lib/libz.so* usr/share/fonts/dejavu 2>/dev/null || true ) > /tmp/pxm50-deploy.lst
{ tar -C "$OV" -cf - . ; } | $S 'tar -C / -xf -'
tar -C "$TGT" -cf - -T /tmp/pxm50-deploy.lst | $S 'tar -C / -xf -'

$S 'sync; reboot' || true
echo "deploy ok – Panel startet neu"
