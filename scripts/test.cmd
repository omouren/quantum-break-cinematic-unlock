@echo off
setlocal
pushd "%~dp0.."
if not defined VCToolsInstallDir (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "QB_VS=%%i"
)
if not defined VCToolsInstallDir call "%QB_VS%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
if not exist build mkdir build
for %%t in (test_candidate_dof_policy test_engine_adapter test_hook_plan test_asi_transaction test_asi_module) do (
  cl /nologo /std:c++17 /EHsc /W4 /WX /Od /MT /Fe:build\%%t.exe /Fo:build\%%t.obj tests\native\%%t.cpp
  if errorlevel 1 exit /b 1
  build\%%t.exe
  if errorlevel 1 exit /b 1
)
set "QB_RESULT=0"
popd
exit /b %QB_RESULT%
