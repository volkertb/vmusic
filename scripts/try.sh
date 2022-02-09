#!/bin/bash

set -e

vm="$1"

if [[ -z "$vm" ]]; then
	echo "Usage: $0 <VM>"
	exit 1
fi

source scripts/logenv.sh

# --debug

exec /usr/lib/virtualbox/VirtualBoxVM --startvm "$vm"
