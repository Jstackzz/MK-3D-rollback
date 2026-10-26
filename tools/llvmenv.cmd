@echo off
rem Put the portable LLVM (clang, lld) and the VS 2026 x64 tools on PATH, then run the arguments.
call "%~dp0vsenv.cmd" rem
set "PATH=%~dp0..\external\llvm\clang+llvm-23.1.4-x86_64-pc-windows-msvc\bin;%PATH%"
%*
