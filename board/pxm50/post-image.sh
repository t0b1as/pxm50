#!/bin/sh
# Baut die DTB gegen die Kernel-Quellen und erzeugt sdcard.img
set -e
BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"
LINUX_DIR="$(ls -d "${BUILD_DIR}"/linux-[0-9]* | head -n1)"

build_dtb() {	# $1 = Ausgabename, $2.. = zusätzliche cpp-Defines
	OUT="$1"; shift
	cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp "$@" \
		-I "${LINUX_DIR}/include" \
		-I "${LINUX_DIR}/arch/arm/boot/dts/ti/omap" \
		-I "${LINUX_DIR}/arch/arm/boot/dts" \
		"${BOARD_DIR}/am335x-pxm50.dts" \
	| "${HOST_DIR}/bin/dtc" -I dts -O dtb -W no-unit_address_vs_reg \
		-o "${BINARIES_DIR}/${OUT}" -
}
build_dtb am335x-pxm50.dtb                       # Touch-INT gpio1_24 (verifiziert)
build_dtb am335x-pxm50-int22.dtb -DTOUCH_INT=22  # Alt-Variante

cp "${BOARD_DIR}/extlinux.conf" "${BINARIES_DIR}/extlinux.conf"

support/scripts/genimage.sh -c "${BOARD_DIR}/genimage.cfg"
