#!/bin/sh

BOARD_DIR="$(dirname $0)"
BOARD_NAME="$(basename ${BOARD_DIR})"
GENIMAGE_CFG="${BOARD_DIR}/genimage-${BOARD_NAME}.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

mkdir -p ${BINARIES_DIR}/rpi-firmware/overlays
cp -v ${BUILD_DIR}/rpi-firmware-93aae1391121c44c7bbddf66abaf38725ffa2dc0/boot/overlays/dwc2.dtbo ${BINARIES_DIR}/rpi-firmware/overlays
cp -v ${BOARD_DIR}/state.img ${BINARIES_DIR}
cp -v ${BOARD_DIR}/config.txt ${BINARIES_DIR}/rpi-firmware
cp -v ${BOARD_DIR}/cmdline.txt ${BINARIES_DIR}/rpi-firmware
cp -v ${BOARD_DIR}/wpa_supplicant.conf ${BINARIES_DIR}

rm -rf "${GENIMAGE_TMP}"

genimage                           \
	--rootpath "${TARGET_DIR}"     \
	--tmppath "${GENIMAGE_TMP}"    \
	--inputpath "${BINARIES_DIR}"  \
	--outputpath "${BINARIES_DIR}" \
	--config "${GENIMAGE_CFG}"

exit $?
