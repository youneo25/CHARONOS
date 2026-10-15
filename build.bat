@echo off
rem Builds CHRONOS (Release) into .\build and deploys the Qt runtime next to the exe.
setlocal
set QT_DIR=C:\dev\Qt\6.10.3\msvc2022_64
set PATH=%APPDATA%\Python\Python314\Scripts;%PATH%
set CMAKE_EXE=%APPDATA%\Python\Python314\site-packages\cmake\data\bin\cmake.exe
if not exist "%CMAKE_EXE%" set CMAKE_EXE=cmake

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

"%CMAKE_EXE%" -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QT_DIR% -DBUILD_FEATURE_TESTS=OFF || exit /b 1
"%CMAKE_EXE%" --build "%~dp0build" || exit /b 1

"%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%~dp0build\CHARONOS.exe" >nul || exit /b 1
echo.
echo Build OK: %~dp0build\CHARONOS.exe
