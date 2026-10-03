#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
build=$(realpath "${1:-build}")
work=$(mktemp -d)
mounted=0
cleanup() {
    if [ "$mounted" = 1 ]; then sudo -n umount "$work/mount" || return; fi
    rm -f "$work/card.img"
    rmdir "$work/mount" "$work"
}
trap cleanup EXIT
mkdir "$work/mount"
truncate -s 128M "$work/card.img"
mkfs.vfat "$work/card.img"
sudo -n mount -o "loop,uid=$(id -u),gid=$(id -g)" "$work/card.img" "$work/mount"
mounted=1
MAINUI_FAT_ROOT="$work/mount" MAINUI_FAT_IMAGE="$work/card.img"     python3 tests/integration/run.py --build-dir "$build" fat_delete favorite_edit
