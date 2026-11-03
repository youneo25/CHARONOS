# Building CHARONOS Lite for Android

The Android build is always the Lite edition (no timeline editor, motion graph or AI / MCP).
It targets arm64-v8a phones and tablets with Android 8.0 (API 26) or newer.

## Tools

| Tool | Version used for 1.2.0 | Where |
| --- | --- | --- |
| Qt for Android | 6.10.3 `android_arm64_v8a` | `aqt install-qt all_os android 6.10.3 android_arm64_v8a -O C:\dev\Qt` |
| Qt for the host | 6.10.3 `msvc2022_64` | already used for the Windows build |
| Android SDK | platforms 35+, build-tools 35 | Android Studio SDK manager or `sdkmanager` |
| Android NDK | 27.0.12077973 | SDK manager |
| JDK | 17 | e.g. Microsoft Build of OpenJDK 17 (zip) |
| CMake / Ninja | 3.21+ | the same as the Windows build |

## FFmpeg for Android

The APK ships its own FFmpeg (LGPL, shared). MP4 export on Android uses FFmpeg's own MPEG-4
encoder (no x264 in this build; the device MediaCodec encoder was not reliable enough), ProRes and
image sequences work as on Windows. Built from the official 8.1.3 source with the NDK's clang
from Git Bash; `make` is the NDK's `prebuilt/windows-x86_64/bin/make.exe`:

```
./configure --prefix=/c/dev/ffmpeg-android/arm64-v8a \
  --target-os=android --arch=aarch64 --cpu=armv8-a --enable-cross-compile \
  --cc=$TC/bin/clang.exe --cxx=$TC/bin/clang++.exe --ld=$TC/bin/clang.exe \
  --ar=$TC/bin/llvm-ar.exe --nm=$TC/bin/llvm-nm.exe --ranlib=$TC/bin/llvm-ranlib.exe --strip=$TC/bin/llvm-strip.exe \
  --sysroot=$TC/sysroot --host-cc=$TC/bin/clang.exe \
  --host-cflags="--target=aarch64-linux-android26 --sysroot=$TC/sysroot" \
  --extra-cflags="--target=aarch64-linux-android26 -O3 -fPIC" \
  --extra-ldflags="--target=aarch64-linux-android26 -Wl,-z,max-page-size=16384" \
  --enable-shared --disable-static --enable-pic --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter --disable-network --disable-debug --disable-symver \
  --enable-jni --enable-mediacodec --pkg-config=false
```

(`$TC` = `<NDK>/toolchains/llvm/prebuilt/windows-x86_64`; set `TMPDIR` to a POSIX path such as
`/c/dev/android-deps/tmp`.) On Windows the shared-library link line is longer than a command line
may be: in `ffbuild/library.mak` write the object list with make's `$(file >$@.objs,...)` instead of
`echo ... > $@.objs`.

## Configure and build

```
cmake -S . -B build-android -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=C:\dev\Qt\6.10.3\android_arm64_v8a\lib\cmake\Qt6\qt.toolchain.cmake ^
  -DQT_HOST_PATH=C:\dev\Qt\6.10.3\msvc2022_64 ^
  -DANDROID_SDK_ROOT=%LOCALAPPDATA%\Android\Sdk -DANDROID_NDK_ROOT=%LOCALAPPDATA%\Android\Sdk\ndk\27.0.12077973 ^
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 ^
  -DFFMPEG_ROOT=C:/dev/ffmpeg-android/arm64-v8a
cmake --build build-android --target apk
```

Sign the release APK with `apksigner` from the SDK build-tools (zipalign first). Keep the
keystore: Android only installs an update over an existing install when it is signed with the same
key.

## What is different on Android

- Sound plays through AAudio (`src/media/AudioEngineAndroid.cpp`); everything else is the shared code.
- Files are opened by path through Qt's own file dialog (the app asks for "All files access"), so a
  project saved on the phone keeps paths that also resolve on Windows; missing media is looked up
  by name next to the project.
- Touch: one finger draws / selects, two fingers pan and pinch-zoom, three fingers scrub, long press
  opens the context menu. Once a pen has been used, fingers only pan (Tools › Draw with Pen Only).
- Updates: Help › Check for Updates uses the Android HTTP stack (Qt has no TLS backend there) and
  opens the APK download in the browser.
