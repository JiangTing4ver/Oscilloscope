@echo off
REM ============================================================
REM  Oscilloscope (STM32F103RC, no GUI library) -- build entry (Keil ARMCC)
REM  Usage: build.bat [make args]     e.g.  build.bat clean
REM
REM  Why this wrapper is needed instead of calling make directly:
REM  armcc writes a temporary file for embedded assembler and reads it
REM  back. When %TEMP% points to a directory that gets recycled by
REM  security/cleanup software, the build fails with
REM    #564: cannot open embedded assembler output file
REM  So TMP/TEMP are pinned to a folder inside the project.
REM
REM  NOTE: keep this file ASCII-only. cmd.exe reads .bat with the
REM  ANSI code page, so UTF-8 Chinese comments break execution.
REM ============================================================
setlocal

set "ROOT=%~dp0"
set "TMPDIR=%ROOT%..\.tmp"
set "TMP=%TMPDIR%"
set "TEMP=%TMPDIR%"
if not exist "%TMPDIR%" mkdir "%TMPDIR%"

set "MAKE=%ROOT%..\.tools\make\make.exe"
if not exist "%MAKE%" (
  echo [ERROR] make.exe not found at: %MAKE%
  exit /b 1
)

cd /d "%ROOT%"
"%MAKE%" %*
exit /b %ERRORLEVEL%
