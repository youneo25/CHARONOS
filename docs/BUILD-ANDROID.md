# Build CHARONOS Lite for Android

The Android app is always CHARONOS Lite and targets arm64-v8a devices running Android 8.0 (API 26) or later.

## Requirements

- Qt for Android and a matching host Qt installation
- Android SDK platform and build-tools
- Android NDK
- JDK 17
- CMake 3.21+ and Ninja
- FFmpeg shared libraries built for Android arm64-v8a

The project build used Qt 6.10.3, NDK 27.0.12077973, and JDK 17. Use compatible versions for your local toolchain.

## Build FFmpeg

Configure FFmpeg for Android arm64-v8a with the NDK toolchain. The APK expects shared libraries for `avformat`, `avcodec`, `avutil`, `swscale`, and `swresample`. Set `FFMPEG_ROOT` to the directory containing their `include/` and `lib/` folders.

## Configure and build the APK

From a Visual Studio Developer Command Prompt or a shell with the Android toolchain configured:

```powershell
$env:JAVA_HOME = 'C:\dev\jdk17\jdk-17.0.21.0.2+1'
$env:ANDROID_SDK_ROOT = "$env:LOCALAPPDATA\Android\Sdk"
$env:ANDROID_NDK_ROOT = "$env:ANDROID_SDK_ROOT\ndk\27.0.12077973"

cmake -S . -B build-android -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=C:/dev/Qt/6.10.3/android_arm64_v8a/lib/cmake/Qt6/qt.toolchain.cmake `
  -DQT_HOST_PATH=C:/dev/Qt/6.10.3/msvc2022_64 `
  -DANDROID_ABI=arm64-v8a `
  -DANDROID_PLATFORM=android-26 `
  -DFFMPEG_ROOT=C:/dev/ffmpeg-android/arm64-v8a
cmake --build build-android --target apk
```

The build output is an unsigned release APK. Align and sign it with `zipalign` and `apksigner` using the release keystore for this app. Keep using the same signing key for updates so Android can install them over an existing copy.

The Android APK is the Lite edition and does not include video downloading.
