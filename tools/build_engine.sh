#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sdk="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
ndk="${ANDROID_NDK_HOME:-$sdk/ndk/28.2.13676358}"
cxx="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang++"
mkdir -p module/bin build/engine
sources=(module/engine/core.cpp module/engine/platform.cpp module/engine/bpf.cpp)
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror "${sources[@]}" tests/engine_test.cpp -o build/engine/tests
build/engine/tests
# Closed-loop convergence check. Optimized, because it plans ~4500 times and the sanitized
# build makes it slow enough to discourage running it; correctness is covered above.
g++ -std=c++17 -O2 -Wall -Wextra -Werror module/engine/core.cpp tests/engine_sim.cpp -o build/engine/sim
build/engine/sim
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" -target bpf -O2 -g0 \
  -Wall -Wextra -Werror -c module/engine/bpf/runqueue.bpf.c -o module/bin/runqueue.bpf.o
"$cxx" -std=c++17 -Os -Wall -Wextra -Werror -static-libstdc++ -fPIE -pie \
  -Wl,-z,max-page-size=16384 -Wl,--gc-sections -ffunction-sections -fdata-sections \
  "${sources[@]}" module/engine/main.cpp -o module/bin/m54-adaptive
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" module/bin/m54-adaptive
