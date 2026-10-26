@echo off
rem Build the MK3D Online launcher (Release) into build\launcher and copy the Qt runtime next to it.
rem Qt comes from external\qt\6.12.0\msvc2022_64 unless QT_DIR points elsewhere (see docs\LAUNCHER.md).
setlocal
set "ROOT=%~dp0.."
if "%QT_DIR%"=="" set "QT_DIR=%ROOT%\external\qt\6.12.0\msvc2022_64"
if not exist "%QT_DIR%\bin\windeployqt.exe" (
    echo Qt not found in %QT_DIR%. Install it first, see docs\LAUNCHER.md.
    exit /b 1
)
call "%ROOT%\tools\vsenv.cmd" cmake -S "%ROOT%\launcher" -B "%ROOT%\build\launcher" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=%QT_DIR%"
if errorlevel 1 exit /b 1
call "%ROOT%\tools\vsenv.cmd" cmake --build "%ROOT%\build\launcher"
if errorlevel 1 exit /b 1
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%ROOT%\build\launcher\MK3DLauncher.exe"
if errorlevel 1 exit /b 1
echo.
echo Built %ROOT%\build\launcher\MK3DLauncher.exe
