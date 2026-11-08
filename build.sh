#!/bin/sh
# Run after sourcing qnxsdp-env.sh from your QNX SDP 8.0 install
set -e
mkdir -p build
qcc -Vgcc_ntox86_64 -Wall -O2 -Iinclude -Isrc -o build/flashforged src/flashforged.c
qcc -Vgcc_ntox86_64 -Wall -O2 -Iinclude -o build/ff_test tests/ff_test.c
qcc -Vgcc_ntox86_64 -Wall -O2 -Iinclude -o build/ff_fault_test tests/ff_fault_test.c
qcc -Vgcc_ntox86_64 -Wall -O2 -Iinclude -o build/ff_campaign tests/ff_campaign.c
qcc -Vgcc_ntox86_64 -Wall -O2 -Iinclude -o build/ff_rt_demo tests/ff_rt_demo.c
echo "built: flashforged ff_test ff_fault_test ff_campaign ff_rt_demo"
