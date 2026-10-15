@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%APPDATA%\Python\Python314\Scripts;%PATH%
set CMAKE_EXE=%APPDATA%\Python\Python314\site-packages\cmake\data\bin\cmake.exe
if not exist "%CMAKE_EXE%" set CMAKE_EXE=cmake
"%CMAKE_EXE%" -S "%~dp0.." -B "%~dp0..\build-test" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/dev/Qt/6.10.3/msvc2022_64 -DBUILD_FEATURE_TESTS=ON || exit /b 1
"%CMAKE_EXE%" --build "%~dp0..\build-test" --target CHRONOS feature_regression || exit /b 1
"C:\dev\Qt\6.10.3\msvc2022_64\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%~dp0..\build-test\feature_regression.exe" >nul || exit /b 1
"C:\dev\ffmpeg\bin\ffmpeg.exe" -hide_banner -loglevel error -y -f lavfi -i "color=c=black:s=320x180:r=24:d=1" -c:v libx264 -pix_fmt yuv420p "%~dp0..\build-test\qa-media.mp4" || exit /b 1
"C:\dev\ffmpeg\bin\ffmpeg.exe" -hide_banner -loglevel error -y -f lavfi -i "testsrc2=s=320x180:r=24:d=2" -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" -c:v libx264 -g 48 -pix_fmt yuv420p -c:a aac -shortest "%~dp0..\build-test\qa-audio-loop.mp4" || exit /b 1
"%~dp0..\build-test\feature_regression.exe" -o "%~dp0..\build-test\feature-results.txt",txt
exit /b %ERRORLEVEL%
