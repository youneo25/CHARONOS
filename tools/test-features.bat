@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%APPDATA%\Python\Python314\Scripts;%PATH%
set CMAKE_EXE=%APPDATA%\Python\Python314\site-packages\cmake\data\bin\cmake.exe
if not exist "%CMAKE_EXE%" set CMAKE_EXE=cmake
set FFMPEG=C:\dev\ffmpeg\bin\ffmpeg.exe
set OUT=%~dp0..\build-test
"%CMAKE_EXE%" -S "%~dp0.." -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/dev/Qt/6.10.3/msvc2022_64 -DBUILD_FEATURE_TESTS=ON || exit /b 1
"%CMAKE_EXE%" --build "%OUT%" --target CHRONOS feature_regression || exit /b 1
if not exist "%OUT%\Qt6Test.dll" "C:\dev\Qt\6.10.3\msvc2022_64\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%OUT%\feature_regression.exe" >nul || exit /b 1
rem Test media (small, generated on every run so the checks never depend on files outside the tree)
"%FFMPEG%" -hide_banner -loglevel error -y -f lavfi -i "color=c=black:s=320x180:r=24:d=1" -c:v libx264 -pix_fmt yuv420p "%OUT%\qa-media.mp4" || exit /b 1
"%FFMPEG%" -hide_banner -loglevel error -y -f lavfi -i "testsrc2=s=320x180:r=24:d=2" -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" -c:v libx264 -g 48 -pix_fmt yuv420p -c:a aac -shortest "%OUT%\qa-audio-loop.mp4" || exit /b 1
"%FFMPEG%" -hide_banner -loglevel error -y -f lavfi -i "color=c=red:s=160x90:r=24:d=1" -f lavfi -i "color=c=0x00FF00:s=160x90:r=24:d=1" -f lavfi -i "color=c=blue:s=160x90:r=24:d=1" -f lavfi -i "color=c=0x808080:s=160x90:r=24:d=1" -filter_complex "[0][1]hstack[t];[2][3]hstack[b];[t][b]vstack,format=yuv420p" -c:v libx264 -crf 10 "%OUT%\qa-yuv-quads.mp4" || exit /b 1
rem 1.1: a dark block moving across a light, static background (silhouette / background plate checks)
"%FFMPEG%" -hide_banner -loglevel error -y -f lavfi -i "color=c=0xD8D0C0:s=320x180:r=24:d=2" -f lavfi -i "color=c=0x302820:s=40x60:r=24:d=2" -filter_complex "[0][1]overlay=x='20+t*100':y=60,format=yuv420p" -c:v libx264 -crf 12 "%OUT%\qa-silhouette.mp4" || exit /b 1
rem 1.1: a second clip with a different picture and tone (multi-track compositing / audio mix checks)
"%FFMPEG%" -hide_banner -loglevel error -y -f lavfi -i "color=c=0x2060C0:s=320x180:r=24:d=1" -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=1" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "%OUT%\qa-blue-tone.mp4" || exit /b 1
"%FFMPEG%" -hide_banner -loglevel error -y -f lavfi -i "testsrc2=s=320x180:r=60:d=1" -f lavfi -i "sine=frequency=880:sample_rate=44100:duration=1" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "%OUT%\qa-60-tone.mp4" || exit /b 1
"%FFMPEG%" -hide_banner -loglevel error -y -f lavfi -i "testsrc2=s=320x180:r=30:d=1" -f lavfi -i "sine=frequency=660:sample_rate=48000:duration=1" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "%OUT%\qa-30-tone.mp4" || exit /b 1
"%FFMPEG%" -hide_banner -loglevel error -y -f lavfi -i "testsrc2=s=320x180:r=30000/1001" -f lavfi -i "sine=frequency=550:sample_rate=48000:duration=1.001" -frames:v 30 -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "%OUT%\qa-2997-tone.mp4" || exit /b 1
"%OUT%\feature_regression.exe" -o "%OUT%\feature-results.txt",txt
exit /b %ERRORLEVEL%
