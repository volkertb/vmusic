#!/bin/bash

set -e

src_path=out/linux.amd64
tgt_path=/usr/lib/virtualbox/ExtensionPacks/VMusic/linux.amd64

install -m 0644 -v $src_path/*.so "$tgt_path"
