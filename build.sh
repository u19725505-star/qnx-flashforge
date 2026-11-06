#!/bin/sh
# Run after sourcing qnxsdp-env.sh from your QNX SDP 8.0 install
set -e
mkdir -p build
qcc -Vgcc_ntox86_64 -Wall -O2 -Iinclude -o build/flashforged src/flashforged.c
qcc -Vgcc_ntox86_64 -Wall -O2 -Iinclude -o build/ff_test tests/ff_test.c
echo "built: build/flashforged build/ff_test"
