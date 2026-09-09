# Building PUCTuner (CLI / WSL2)

Instructions for building the APK and native artifacts **without Android Studio**, using a Linux shell (e.g. WSL2).

The APK is written in Kotlin/Compose. The project includes an independent ARM64 C++17 daemon and an eBPF object, both compiled with Android NDK 28.2.13676358. JDK + SDK + Gradle are sufficient if you only want to build the APK.

To generate all artifacts in a configured WSL2 environment:

```bash
bash tools/build_engine.sh  # ASan/UBSan unit tests, closed-loop convergence simulation, eBPF & ARM64 binary
bash tools/build_app.sh     # Optimized release APK with R8 and lint
python3 tools/package.py    # Distributable Magisk/KernelSU ZIP with ELF and APK
```

`build_engine.sh` runs three checks before producing the final binary, failing immediately if any step fails:
1. Unit tests under ASan/UBSan (`tests/engine_test.cpp`).
2. Closed-loop simulation (`tests/engine_sim.cpp`, verifying convergence, waste rejection, and thermal limits).
3. Cross-compilation of the eBPF probe with `clang -target bpf`.

The eBPF object is placed in `module/bin/runqueue.bpf.o` and loaded at runtime by the daemon. **There is no dependency on libbpf**: PUCTuner features a custom lightweight loader (~9 KB) that verifies tracepoint ABI against `/sys/kernel/tracing/events/*/format` before attaching, refusing to run if kernel layouts mismatch. See `docs/EBPF.md`.

> Gradle wrapper 8.10.2 is versioned in the repository. Use `./gradlew`; no global Gradle installation is needed.

## Required Toolchain Versions

| Component | Version | Source |
|---|---|---|
| Android Gradle Plugin | **8.7.3** | `build.gradle.kts` |
| Gradle | **8.10.2** | `gradle/wrapper/` |
| JDK | **17** | OpenJDK 17 |
| Kotlin + Compose compiler | **2.1.0** | `build.gradle.kts` |
| compileSdk / targetSdk | **36** | `app/build.gradle.kts` |
| minSdk | **33** | `app/build.gradle.kts` |
| Build Tools | **34.0.0+** (36.0.0 recommended) | Android SDK |

External dependency: `com.github.topjohnwu.libsu:core:6.0.0` (configured via JitPack in `settings.gradle.kts`).

## Step 1 — JDK 17

```bash
sudo apt update && sudo apt install -y openjdk-17-jdk unzip
java -version   # must show 17.x
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
```

## Step 2 — Android SDK (Command-Line Tools)

```bash
export ANDROID_HOME="$HOME/Android/Sdk"
mkdir -p "$ANDROID_HOME/cmdline-tools"
cd /tmp
# Download "commandlinetools-linux-*.zip" from developer.android.com/studio#command-tools
unzip -q commandlinetools-linux-*_latest.zip -d "$ANDROID_HOME/cmdline-tools"
mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest"

export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"
yes | sdkmanager --licenses
sdkmanager "platforms;android-36" "build-tools;36.0.0" "platform-tools"
```

## Step 3 — Android NDK (r28)

```bash
sdkmanager "ndk;28.2.13676358"
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/28.2.13676358"
```

## Step 4 — Build Engine & Module

```bash
bash tools/build_engine.sh
python3 tools/package.py
```
