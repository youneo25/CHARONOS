cmake_minimum_required(VERSION 3.21)
project(CHRONOS VERSION 0.0.1 LANGUAGES CXX)
set(CHARONOS_DISPLAY_VERSION "Beta ${PROJECT_VERSION}")
set(CHARONOS_PACKAGE_VERSION "${PROJECT_VERSION}-beta")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

set(FFMPEG_ROOT "C:/dev/ffmpeg" CACHE PATH "FFmpeg shared dev build (include/, lib/, bin/)")

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Svg)

add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE "${FFMPEG_ROOT}/include")
foreach(lib avformat avcodec avutil swscale swresample)
  target_link_libraries(ffmpeg INTERFACE "${FFMPEG_ROOT}/lib/${lib}.lib")
endforeach()

add_executable(CHRONOS WIN32
  src/main.cpp
  src/media/MediaInfo.h
  src/media/FrameSource.h
  src/media/VideoDecoder.h        src/media/VideoDecoder.cpp
  src/media/ImageSequenceSource.h src/media/ImageSequenceSource.cpp
  src/media/FrameCache.h          src/media/FrameCache.cpp
  src/media/FrameServer.h         src/media/FrameServer.cpp
  src/media/MediaLibrary.h        src/media/MediaLibrary.cpp
  src/media/AudioClip.h           src/media/AudioClip.cpp
  src/media/AudioEngine.h         src/media/AudioEngine.cpp
  src/media/AudioClock.h
  src/model/Sequence.h
  src/model/ReviewRange.h
  src/export/VideoEncoder.h       src/export/VideoEncoder.cpp
  src/export/Exporter.h           src/export/Exporter.cpp
  src/media/CanvasSource.h
  src/ui/ConsoleDock.h src/ui/ConsoleDock.cpp
  src/ui/HotkeyManager.h src/ui/HotkeyManager.cpp
  src/ui/ExportDialog.h           src/ui/ExportDialog.cpp
  src/ui/FrameExportDialog.h      src/ui/FrameExportDialog.cpp
  src/ui/Icons.h                  src/ui/Icons.cpp
  src/ui/Palette.h
  src/ui/StrokePanel.h            src/ui/StrokePanel.cpp
  src/ui/ABDelegate.h             src/ui/ABDelegate.cpp
  src/model/Track.h               src/model/Track.cpp
  src/track/Tracker.h             src/track/Tracker.cpp
  src/model/Annotation.h          src/model/Annotation.cpp
  src/model/AnnotationPainter.h   src/model/AnnotationPainter.cpp
  src/model/Project.h             src/model/Project.cpp
  src/ui/Viewport.h               src/ui/Viewport.cpp
  src/ui/Timeline.h               src/ui/Timeline.cpp
  src/ui/MainWindow.h             src/ui/MainWindow.cpp
  src/ui/Style.h                  src/ui/Style.cpp
  src/FileAssociation.h           src/FileAssociation.cpp
  res/app.rc
)

target_include_directories(CHRONOS PRIVATE src)
set_target_properties(CHRONOS PROPERTIES OUTPUT_NAME CHARONOS)
target_compile_definitions(CHRONOS PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
  FRAMEREVIEW_VERSION="${CHARONOS_DISPLAY_VERSION}")
if(MSVC)
  target_compile_options(CHRONOS PRIVATE /utf-8 /W3 /permissive- /MP)
endif()
target_link_libraries(CHRONOS PRIVATE Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Svg ffmpeg winmm shell32)

# Frame-exactness verification tool (console)
add_executable(frame_verify tools/frame_verify.cpp
  src/media/VideoDecoder.cpp src/media/VideoDecoder.h)
target_include_directories(frame_verify PRIVATE src)
target_compile_definitions(frame_verify PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(frame_verify PRIVATE /utf-8 /permissive-)
endif()
target_link_libraries(frame_verify PRIVATE Qt6::Core Qt6::Gui ffmpeg)

# Korean translation for Qt's built-in dialogs
add_executable(playback_probe tools/playback_probe.cpp
  src/media/VideoDecoder.cpp src/media/FrameCache.cpp src/media/FrameServer.cpp src/media/FrameServer.h)
target_include_directories(playback_probe PRIVATE src)
target_compile_definitions(playback_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(playback_probe PRIVATE /utf-8 /permissive-)
endif()
target_link_libraries(playback_probe PRIVATE Qt6::Core Qt6::Gui ffmpeg)

# Korean translation for Qt's built-in dialogs
get_target_property(QT_QMAKE Qt6::qmake IMPORTED_LOCATION)
get_filename_component(QT_ROOT "${QT_QMAKE}/../.." ABSOLUTE)
add_custom_command(TARGET CHRONOS POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:CHRONOS>/translations
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${QT_ROOT}/translations/qtbase_ko.qm" $<TARGET_FILE_DIR:CHRONOS>/translations/)

# Window icon (the .exe icon comes from res/app.rc)
add_custom_command(TARGET CHRONOS POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/res/app.png" $<TARGET_FILE_DIR:CHRONOS>/app.png)

# FFmpeg runtime DLLs next to the executable
file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
add_custom_command(TARGET CHRONOS POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:CHRONOS>)

# Tracker accuracy test (console)
add_executable(track_test tools/track_test.cpp src/track/Tracker.cpp
  src/media/VideoDecoder.cpp src/media/VideoDecoder.h)
target_include_directories(track_test PRIVATE src)
target_compile_definitions(track_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(track_test PRIVATE /utf-8 /permissive-)
endif()
target_link_libraries(track_test PRIVATE Qt6::Core Qt6::Gui ffmpeg)

# Pressure stroke rendering check (console)
add_executable(pressure_test tools/pressure_test.cpp src/model/AnnotationPainter.cpp src/model/Annotation.cpp)
target_include_directories(pressure_test PRIVATE src)
if(MSVC)
  target_compile_options(pressure_test PRIVATE /utf-8 /permissive-)
endif()
target_link_libraries(pressure_test PRIVATE Qt6::Gui)

# Audio decode / clock check (console)
add_executable(audio_test tools/audio_test.cpp src/media/AudioClip.cpp src/media/AudioEngine.cpp src/media/VideoDecoder.cpp)
target_include_directories(audio_test PRIVATE src)
target_compile_definitions(audio_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(audio_test PRIVATE /utf-8 /permissive-)
endif()
target_link_libraries(audio_test PRIVATE Qt6::Core Qt6::Gui ffmpeg winmm)

option(BUILD_FEATURE_TESTS "Build interactive feature regression checks" OFF)
if(BUILD_FEATURE_TESTS)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  get_target_property(FEATURE_SOURCES CHRONOS SOURCES)
  list(REMOVE_ITEM FEATURE_SOURCES src/main.cpp res/app.rc)
  add_executable(feature_regression tools/feature_regression.cpp ${FEATURE_SOURCES})
  target_include_directories(feature_regression PRIVATE src)
  target_compile_definitions(feature_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN FRAMEREVIEW_VERSION="${CHARONOS_DISPLAY_VERSION}")
  if(MSVC)
    target_compile_options(feature_regression PRIVATE /utf-8 /permissive-)
  endif()
  target_link_libraries(feature_regression PRIVATE Qt6::Test Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Svg ffmpeg winmm shell32)
  enable_testing()
  add_test(NAME feature_regression COMMAND feature_regression)
endif()
