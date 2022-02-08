#!/bin/bash

set -ex

vm="$1"

VBoxManage setextradata "$vm" VBoxInternal/Devices/adlib/0/Trusted
VBoxManage setextradata "$vm" VBoxInternal/Devices/adlib/0/Config/MirrorPort
VBoxManage setextradata "$vm" VBoxInternal/Devices/mpu401/0/Trusted
VBoxManage setextradata "$vm" VBoxInternal/Devices/mpu401/0/Config/IRQ
VBoxManage setextradata "$vm" VBoxInternal/Devices/emu8000/0/Config/RomFile
