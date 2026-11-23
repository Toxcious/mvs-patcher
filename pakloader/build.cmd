@echo off
rem Builds MvsPakLoader.asi (needs the Visual Studio 2022 C++ build tools).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /LD /EHsc /W3 MvsPakLoader.cpp /Fe:MvsPakLoader.asi /link /NOLOGO || exit /b 1
del /q MvsPakLoader.obj MvsPakLoader.lib MvsPakLoader.exp 2>nul
echo built MvsPakLoader.asi
