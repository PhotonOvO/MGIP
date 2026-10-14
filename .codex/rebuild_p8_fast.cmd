@echo off
setlocal EnableExtensions DisableDelayedExpansion
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -no_logo -arch=amd64 -host_arch=amd64
if errorlevel 1 exit /b %errorlevel%
if /i "%~1"=="Debug" goto Debug
if /i "%~1"=="Release" goto Release
echo usage: rebuild_p8_fast.cmd Debug^|Release
exit /b 2
:Debug
set "BUILD_ROOT=G:\Temp\mgif-p6-90-msvc-20260831T134500Z-Debug"
goto Build
:Release
set "BUILD_ROOT=G:\Temp\mgif-p6-90-msvc-20260831T134500Z-Release"
:Build
where cl.exe
if errorlevel 1 exit /b %errorlevel%
cmake --build "%BUILD_ROOT%" --target Demo editor_controller_animation_tests editor_controller_transaction_tests editor_selection_picking_integration_tests editor_renderer_integration_tests editor_rhi_contract_tests editor_gizmo_integration_tests --parallel 4
exit /b %errorlevel%
