#!/bin/bash

# Creates a ExtPack.manifest-style file, which contains one entry for each file in the extpack like the following:

# MD5 (darwin.amd64/VBoxUsbWebcamR3.dylib) = 54e84211cad1630b278d25c99ab13a41
# SHA256 (darwin.amd64/VBoxUsbWebcamR3.dylib) = cfde0b80981f70893dade0a06b448fcde47f31fbaf63041554d6852a8402452c
# SHA512 (darwin.amd64/VBoxUsbWebcamR3.dylib) = 9ddfb08dacd4c03756681bdab1b95be8b1b1ebb06e3e9c7656314d5a2392aef30eee91c52ca9071f8fa4571da9d515ab57f5cb5b7edeae4303e07432e8ef3d7f
# SHA1 (darwin.amd64/VBoxUsbWebcamR3.dylib) = bec791d4298971c8dc9e3a5ed361e5bcc51fa479
# SIZE (darwin.amd64/VBoxUsbWebcamR3.dylib) = 70800

for file in "$@"
do
	if [[ "$file" == "ExtPack.manifest" ]]; then
		continue
	fi

	for alg in md5 sha256 sha512 sha1
	do
		${alg}sum --tag "$file"
	done
	stat --printf="SIZE ($file) = %s\n" "$file"
done
