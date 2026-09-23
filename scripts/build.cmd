@echo off
setlocal
pushd "%~dp0.."
if not defined VCToolsInstallDir (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "QB_VS=%%i"
)
if not defined VCToolsInstallDir call "%QB_VS%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /DQB_ASI /LD /Fe:build\QuantumBreakCinematicUnlock.asi /Fo:build\ src\native\candidate_module.cpp src\native\asi_bootstrap.cpp /link /IMPLIB:build\QuantumBreakCinematicUnlock.lib
set "QB_RESULT=%errorlevel%"
popd
exit /b %QB_RESULT%
