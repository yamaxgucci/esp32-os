@echo off
rem ArgonOS - build the agent that runs on the far machine.
rem
rem One .exe, no dependencies, no installer, no admin: it captures the screen
rem with GDI, compresses tiles with the miniz already vendored in this tree, and
rem talks to a COM port.  Everything it links against ships with Windows.
rem
rem   tools\kvmagent\build.cmd
rem
rem Output: build\kvmagent.exe
rem
rem The compiler is mingw-w64 - the same one ARGON_HOST_CC_BIN names for the
rem host tests.  Vendored miniz is compiled on its own, without this tree's
rem warning flags: it is somebody else's code and warning about it is noise.

setlocal
set ROOT=%~dp0..\..
set OUT=%ROOT%\build\kvmagent.exe
set OBJ=%ROOT%\build\kvmagent

if not exist "%ROOT%\build" mkdir "%ROOT%\build"
if not exist "%OBJ%" mkdir "%OBJ%"

set CC=gcc
if exist "D:\dev\mingw64\bin\gcc.exe" set CC=D:\dev\mingw64\bin\gcc.exe

set MZ=%ROOT%\third_party\miniz

for %%F in (miniz miniz_tdef miniz_tinfl) do (
  "%CC%" -O2 -w -I"%MZ%" -c "%MZ%\%%F.c" -o "%OBJ%\%%F.o"
  if errorlevel 1 exit /b 1
)

"%CC%" -O2 -Wall -Wextra -Wno-unused-parameter -I"%MZ%" ^
  -c "%~dp0kvmagent.c" -o "%OBJ%\kvmagent.o"
if errorlevel 1 exit /b 1

"%CC%" "%OBJ%\kvmagent.o" "%OBJ%\miniz.o" "%OBJ%\miniz_tdef.o" ^
  "%OBJ%\miniz_tinfl.o" -o "%OUT%" ^
  -lgdi32 -luser32 -lshell32 -lsetupapi -lwinusb -lws2_32 -ladvapi32
if errorlevel 1 (
  echo build failed
  exit /b 1
)
echo built %OUT%
