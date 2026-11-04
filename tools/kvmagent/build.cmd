@echo off
rem ArgonOS - build the agent that runs on the far machine.
rem
rem   tools\kvmagent\build.cmd        -> build\kvmagent.exe
rem
rem One 32-bit executable for every Windows from 98 to 11 (64-bit Windows runs
rem it under WOW64).  No C runtime at all: compat.c supplies the few library
rem calls on top of Win32, because no msvcrt/UCRT a current toolchain targets is
rem present on all of those systems, while kernel32, user32 and gdi32 are.  The
rem PE header says subsystem 4.0 so 98 will load it, and nothing is imported
rem that 98 lacks: WinUSB and SetProcessDPIAware are looked up at run time.
rem
rem The compiler is llvm-mingw (clang + lld), for its i686 target and its
rem compiler-rt builtins (64-bit division, stack probes) that stand in for
rem libgcc.  Vendored miniz is compiled without stdio, time or zip support.

setlocal
set ROOT=%~dp0..\..
set OUT=%ROOT%\build\kvmagent.exe
set OBJ=%ROOT%\build\kvmagent
set LLVM=D:\dev\llvm-mingw-20261020-ucrt-x86_64
set CC=%LLVM%\bin\i686-w64-mingw32-clang.exe
for /f "delims=" %%B in ('dir /b /s "%LLVM%\lib\clang\libclang_rt.builtins-i386.a"') do set RTLIB=%%B

if not exist "%ROOT%\build" mkdir "%ROOT%\build"
if not exist "%OBJ%" mkdir "%OBJ%"

set MZ=%ROOT%\third_party\miniz
set CFLAGS=-Os -ffreestanding -fno-stack-protector -fno-ident -march=i586 ^
  -DNDEBUG -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS ^
  -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -I"%MZ%"

for %%F in (miniz miniz_tdef miniz_tinfl) do (
  "%CC%" %CFLAGS% -w -include "%~dp0compat.h" -c "%MZ%\%%F.c" -o "%OBJ%\%%F.o"
  if errorlevel 1 exit /b 1
)
"%CC%" %CFLAGS% -Wall -Wextra -Wno-unused-parameter -c "%~dp0compat.c" -o "%OBJ%\compat.o"
if errorlevel 1 exit /b 1
"%CC%" %CFLAGS% -Wall -Wextra -Wno-unused-parameter -c "%~dp0kvmagent.c" -o "%OBJ%\kvmagent.o"
if errorlevel 1 exit /b 1

"%CC%" -nostdlib -o "%OUT%" "%OBJ%\kvmagent.o" "%OBJ%\compat.o" ^
  "%OBJ%\miniz.o" "%OBJ%\miniz_tdef.o" "%OBJ%\miniz_tinfl.o" ^
  -Wl,--entry=_agent_start@0 -Wl,--subsystem,console:4.0 ^
  -lkernel32 -luser32 -lgdi32 -lshell32 -lsetupapi -lhid -lws2_32 -ladvapi32 ^
  "%RTLIB%"
if errorlevel 1 (
  echo build failed
  exit /b 1
)
echo built %OUT%
