#!/bin/bash

set -e

vm="$1"

if [[ -z "$vm" ]]; then
	echo "Usage: $0 <VM>"
	exit 1
fi

export VBOX_LOG_DEST="nofile stderr"
export VBOX_LOG_FLAGS="thread time"
export VBOX_LOG="+dev_sb16.e.l.f.l3.l5.l7.l9"
export VBOX_RELEASE_LOG_DEST="nofile stderr"
export VBOX_RELEASE_LOG="-all +dev_sb16.e.l.f"

# --debug

exec /usr/lib/virtualbox/VirtualBoxVM --startvm "$vm"
