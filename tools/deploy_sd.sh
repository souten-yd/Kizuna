#!/usr/bin/env bash
# Copies the generated pack onto a microSD card and checks the things that
# actually go wrong in practice.
#
# The big one: a 64 GB card ships formatted as exFAT, and the ESP32 Arduino SD
# library only speaks FAT12/16/32. The card will simply refuse to mount, with
# no hint as to why, so this script fails loudly instead.
set -euo pipefail

usage() {
    cat <<'USAGE'
usage: deploy_sd.sh <mountpoint> [source]

  mountpoint  where the card is mounted, e.g. /media/you/M5COMPANION
  source      pack tree to copy (default: build/sd)

Formatting a 64 GB card for the M5GO (destroys everything on it):

  sudo umount /dev/sdX1
  sudo parted /dev/sdX --script mklabel msdos mkpart primary fat32 1MiB 100%
  sudo mkfs.vfat -F 32 -s 64 -n M5COMPANION /dev/sdX1

  -F 32  forces FAT32 rather than exFAT
  -s 64  uses 32 KiB clusters, which keeps the FAT small on a large card
USAGE
}

[ $# -ge 1 ] || { usage; exit 2; }
MOUNT="$1"
SRC="${2:-$(dirname "$0")/../build/sd}"

[ -d "$MOUNT" ] || { echo "error: $MOUNT is not a directory"; exit 1; }
[ -d "$SRC/companion" ] || { echo "error: no pack at $SRC (run tools/pack_assets.py first)"; exit 1; }

FSTYPE=$(findmnt -no FSTYPE --target "$MOUNT" || true)
case "$FSTYPE" in
    vfat|msdos) ;;
    "")   echo "warning: could not determine the filesystem of $MOUNT" ;;
    *)    echo "error: $MOUNT is $FSTYPE; the M5GO needs FAT32. See --help."; exit 1 ;;
esac

echo "copying $SRC -> $MOUNT"
mkdir -p "$MOUNT/companion"
cp -r "$SRC/companion/." "$MOUNT/companion/"
sync

PACKS=$(find "$MOUNT/companion/packs" -name manifest.json | wc -l)
CLIPS=$(find "$MOUNT/companion" -name '*.m5a' | wc -l)
BYTES=$(du -sh "$MOUNT/companion" | cut -f1)
echo "done: $PACKS pack(s), $CLIPS clips, $BYTES"
echo
echo "Eject, put the card in the M5GO and power-cycle it."
