@echo off
rem Developer helper: incremental build of the test tree. Usage: build-quick.bat [ninja targets...]
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%APPDATA%\Python\Python314\Scripts;%PATH%
set CMAKE_EXE=%APPDATA%\Python\Python314\site-packages\cmake\data\bin\cmake.exe
if not exist "%CMAKE_EXE%" set CMAKE_EXE=cmake
set OUT=%~dp0..\build-test
"%CMAKE_EXE%" -S "%~dp0.." -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/dev/Qt/6.10.3/msvc2022_64 -DBUILD_FEATURE_TESTS=ON >nul || exit /b 1
if "%~1"=="" (
  ninja -C "%OUT%" -k 0 CHRONOS feature_regression
) else (
  ninja -C "%OUT%" -k 0 %*
)
exit /b %ERRORLEVEL%
