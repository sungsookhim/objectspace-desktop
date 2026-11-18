#!/usr/bin/env bash
# Test bed for the M0 prototype: three loopback filesystems.
#
#   testbed.sh up     create and mount ext4, btrfs and FAT images
#   testbed.sh down   unmount and remove them
#
# Mounts go under $WORK/mnt/{ext4,btrfs,fat}; $WORK defaults to
# /var/tmp/wps-m0-$USER so that the test UIDs can reach it (a home folder is
# usually mode 700). Needs sudo. Creates no user accounts: the extra test
# users are bare UIDs 60001 and 60002 in group 60000, run through setpriv.
set -euo pipefail

WORK=${WORK:-/var/tmp/wps-m0-$(id -un)}
ME=$(id -u)
MYGID=$(id -g)
GROUP_SHARED=60000

up() {
    mkdir -p "$WORK/img" "$WORK/mnt/ext4" "$WORK/mnt/btrfs" "$WORK/mnt/fat"
    truncate -s 512M "$WORK/img/ext4.img"
    truncate -s 512M "$WORK/img/btrfs.img"
    truncate -s 128M "$WORK/img/fat.img"
    mkfs.ext4 -q -F -N 262144 -L wpsext4 "$WORK/img/ext4.img"
    mkfs.btrfs -q -f -L wpsbtrfs "$WORK/img/btrfs.img" >/dev/null
    mkfs.fat -F 32 -n WPSFAT "$WORK/img/fat.img" >/dev/null
    sudo mount -o loop "$WORK/img/ext4.img" "$WORK/mnt/ext4"
    sudo mount -o loop "$WORK/img/btrfs.img" "$WORK/mnt/btrfs"
    sudo mount -o loop,uid=$ME,gid=$MYGID "$WORK/img/fat.img" "$WORK/mnt/fat"
    for fs in ext4 btrfs; do
        sudo chown "$ME:$MYGID" "$WORK/mnt/$fs"
        # A group-shared folder, writable by the test group, setgid.
        sudo mkdir -p "$WORK/mnt/$fs/shared"
        sudo chown "60001:$GROUP_SHARED" "$WORK/mnt/$fs/shared"
        sudo chmod 2775 "$WORK/mnt/$fs/shared"
        # A private folder of test user 60001.
        sudo mkdir -p "$WORK/mnt/$fs/private60001"
        sudo chown 60001:60001 "$WORK/mnt/$fs/private60001"
        sudo chmod 0700 "$WORK/mnt/$fs/private60001"
    done
    echo "testbed up at $WORK/mnt"
}

down() {
    for fs in ext4 btrfs fat; do
        if mountpoint -q "$WORK/mnt/$fs"; then sudo umount "$WORK/mnt/$fs"; fi
    done
    sudo rm -rf "$WORK"
    echo "testbed down"
}

case "${1:-}" in
    up) up ;;
    down) down ;;
    *) echo "usage: $0 up|down" >&2; exit 2 ;;
esac
