@echo off
setlocal
set QT_DIR=C:\dev\Qt\6.10.3\msvc2022_64
set PATH=%APPDATA%\Python\Python314\Scripts;%PATH%
set CMAKE_EXE=%APPDATA%\Python\Python314\site-packages\cmake\data\bin\cmake.exe
if not exist "%CMAKE_EXE%" set CMAKE_EXE=cmake
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
"%CMAKE_EXE%" -S "%~dp0." -B "%~dp0build-test" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QT_DIR% >nul || exit /b 1
"%CMAKE_EXE%" --build "%~dp0build-test" --target CHRONOS || exit /b 1
if not exist "%~dp0build-test\Qt6Svg.dll" "%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%~dp0build-test\CHARONOS.exe" >nul
echo Build OK
