@echo off
REM Portable prototest build for CI: assumes a Developer environment is ALREADY
REM active (vcvars has set INCLUDE/LIB/PATH), unlike build_prototest.cmd which
REM pins the local v100 toolchain. The prototest TUs are game-free - they touch
REM only Wire.h, Interp.h, and SaveXfer.h -> Engine.h (Wire.h) / NetLink.h (enet
REM headers) - so the only extra include roots are the vendored enet headers and
REM the vc10_compat shims. No enet .lib is linked (SaveXfer uses NetLink.h for
REM declarations only), so the enet HEADERS alone are enough.
setlocal

set "REPO=%~dp0.."
pushd "%REPO%" >nul
set "REPO=%CD%"
popd >nul

set "ENET=%REPO%\third_party\enet\enet\include"
set "COMPAT=%REPO%\third_party\vc10_compat"

if not exist "%ENET%\enet\enet.h" (
    echo ERROR: enet headers not found at "%ENET%". Clone lsalzman/enet @ v1.3.18
    echo        into third_party\enet\enet before running CI prototest.
    exit /b 2
)

set "INCLUDE=%ENET%;%COMPAT%;%INCLUDE%"

if not exist "%REPO%\dist" mkdir "%REPO%\dist"
if not exist "%REPO%\build\prototest" mkdir "%REPO%\build\prototest"

echo === Building prototest.exe (CI, ambient MSVC) ===
cl.exe /nologo /O2 /EHsc /W3 /D KENSHICOOP_PROTOTEST ^
    /Fo"%REPO%\build\prototest\\" ^
    /Fe"%REPO%\dist\prototest.exe" ^
    "%REPO%\src\prototest\main.cpp" ^
    "%REPO%\src\plugin\sync\Interp.cpp" ^
    "%REPO%\src\plugin\sync\SaveXfer.cpp"
if errorlevel 1 (
    echo prototest build FAILED
    exit /b 1
)
echo prototest built: %REPO%\dist\prototest.exe
exit /b 0
