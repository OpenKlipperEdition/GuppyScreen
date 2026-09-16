#!/bin/bash

RELEASES_DIR=./releases/guppyscreen
rm -rf $RELEASES_DIR
mkdir -p $RELEASES_DIR

ASSET_NAME=$1

"$CROSS_COMPILE"strip ./build/bin/guppyscreen
cp ./build/bin/guppyscreen $RELEASES_DIR/guppyscreen
if [ -f ./build/bin/guppybeep ]; then
    "$CROSS_COMPILE"strip ./build/bin/guppybeep
    cp ./build/bin/guppybeep $RELEASES_DIR/guppybeep
fi
cp -r ./k1/k1_mods $RELEASES_DIR
cp -r ./k1/scripts $RELEASES_DIR
cp -r ./themes $RELEASES_DIR
# NebulaOS Phase 0 cleanup (2026-08-16): installer.sh, installer-deb.sh,
# update.sh, and debian/ (the OpenKE stock-firmware installer/patch/systemd-
# packaging tree, including the debian/kd_graphic_mode placement this section
# used to do) were deleted from this repo as confirmed-dead weight — never
# fetched or consumed by any NebulaOS boot path (NebulaOS-firmware pins and
# cross-compiles this repo directly; see wiki/Integration-with-NebulaOS.md).
# This release tarball now packages only what's actually load-bearing
# anywhere: the compiled binaries, k1_mods (now just buzzer/ + tmcstatus.py),
# k1/scripts, and themes/.
if [ -f ./custom_upgrade.sh ]; then
    cp ./custom_upgrade.sh $RELEASES_DIR
fi


echo "{\"version\": \"$OPENKE_VERSION\", \"theme\": \"$GUPPY_THEME\", \"asset_name\": \"$ASSET_NAME.tar.gz\"}" > $RELEASES_DIR/.version
tar czf $ASSET_NAME.tar.gz -C releases .
