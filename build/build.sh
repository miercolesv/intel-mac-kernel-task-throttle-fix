#!/bin/sh
# Build smcfan.
#   On a Mac with the Command Line Tools:   sh build/build.sh
#   On Linux (clang + lld, no macOS SDK):   sh build/build.sh
# The Linux path links against the small .tbd stubs in this folder instead of the SDK.
set -e
cd "$(dirname "$0")/.."
mkdir -p bin

if [ "$(uname)" = "Darwin" ]; then
    echo "Building natively on macOS..."
    clang -O2 -Wall -arch x86_64 -o bin/smcfan smcfan.c -framework IOKit
else
    echo "Cross-compiling for x86_64 macOS from $(uname)..."
    clang -target x86_64-apple-macos10.15 -ffreestanding -fno-stack-protector -O2 -Wall \
          -c smcfan.c -o build/smcfan.o
    ld.lld -flavor darwin -arch x86_64 -platform_version macos 10.15 15.0 \
           -o bin/smcfan build/smcfan.o build/libSystem.tbd build/IOKit.tbd
    rm -f build/smcfan.o
fi
echo "Built bin/smcfan"
