@echo off
rem Run a command inside the VS 2026 Build Tools x64 environment (adds cl, cmake, ninja to PATH).
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
%*
