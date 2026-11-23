@echo off
rem Builds BetaFeel.asi (players' build) and BetaFeel_dev.asi (with the measuring aids, BETAFEEL_DEV).
rem Needs the Visual Studio 2022 C++ build tools. fighters.cpp keeps FighterSmooth's /EHsc, poses.cpp PoseSmooth's /EHa.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
call :one BetaFeel "" || exit /b 1
call :one BetaFeel_dev "/DBETAFEEL_DEV" || exit /b 1
del /q *.obj *.lib *.exp 2>nul
exit /b 0

:one
set F=/nologo /O2 /MT /W3 /c %~2
cl %F% /EHsc main.cpp fighters.cpp boot.cpp lab.cpp /Fo:.\ || exit /b 1
cl %F% /EHa poses.cpp /Fo:.\ || exit /b 1
link /nologo /DLL /OUT:%1.asi main.obj fighters.obj boot.obj lab.obj poses.obj user32.lib || exit /b 1
echo built %1.asi
exit /b 0
