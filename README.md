# CHARONOS 1.2.1

CHARONOS is a frame-accurate reference and review tool for animators. Load video or image sequences, inspect timing frame by frame, draw annotations, compare takes, track motion, and export review clips.

**Latest release:** [CHARONOS 1.2.1](https://github.com/youneo25/CHARONOS/releases/latest) · **License:** see the repository and bundled third-party notices.

## Editions

| Edition | Platform | Included features |
| --- | --- | --- |
| CHARONOS | Windows 10/11, x64 | Full timeline editor, Motion Graph, AI/MCP tools, and all review features |
| CHARONOS Lite | Windows 10/11, x64 | Review and annotation features without the timeline editor, Motion Graph, or AI/MCP tools |
| CHARONOS Lite | Android 8+, arm64 | Lite features with touch and pen support |

All editions use the same `.frvm` project format. Keep media beside the project when moving it between devices.

## Download and install

Get the Windows installers, portable archives, and Android APK from the [latest release](https://github.com/youneo25/CHARONOS/releases/latest). The Windows installer does not require administrator rights. On Android, grant file access when prompted.

## Video downloader

The video downloader is available in the Windows Full edition. Choose **File → Video Downloader** and paste a link supported by yt-dlp, including YouTube and bilibili. Pick a destination and start the download. CHARONOS adds the completed video to **Sources** without inserting it into the timeline. The dialog supports video with audio, video-only, audio-only, and an optional cookies file for login-restricted content.

The Lite edition, including the Android APK, does not include video downloading.

## Core features

- Frame-accurate playback for video and image sequences
- Multi-track editing, linked audio, trimming, compositing, and undo
- Drawing, text, shapes, frame notes, and onion skin
- A/B comparison with wipe, split, blend, synchronization, and offset controls
- Motion tracking, Motion Graph, bookmarks, and viewing aids
- Silhouette and motion analysis filters
- Video, audio, still-frame, and image-sequence export

## Documentation

- [User Guide](docs/CHARONOS-User-Guide.html)
- [CHARONOS 1.2.1 notes](docs/CHANGES-1.2.1.md)
- [Android Lite build guide](docs/BUILD-ANDROID.md)

## Build from source

Build the Windows edition with CMake, Qt 6, MSVC 2022, and FFmpeg. Android Lite requires the Qt Android kit, Android SDK/NDK, JDK 17, and Android FFmpeg libraries. See the platform build guide for the Android configuration.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=C:/dev/Qt/6.10.3/msvc2022_64 `
  -DFFMPEG_ROOT=C:/dev/ffmpeg
cmake --build build --target CHRONOS
```

Set `-DCHARONOS_LITE=ON` to build CHARONOS Lite for Windows. Android builds are always Lite.
