#!/bin/bash

set -ex

vm="$1"

# Adlib
VBoxManage setextradata "$vm" VBoxInternal/Devices/adlib/0/Trusted 1
VBoxManage setextradata "$vm" VBoxInternal/Devices/adlib/0/Config/MirrorPort "0x220"

# MPU-401
VBoxManage setextradata "$vm" VBoxInternal/Devices/mpu401/0/Trusted 1

# EMU8000
awe32_romfile=~/.pcem/roms/awe32.raw # Mandatory!
VBoxManage setextradata "$vm" VBoxInternal/Devices/emu8000/0/Config/ROMFile "$awe32_romfile"
