@echo off
setlocal EnableExtensions DisableDelayedExpansion
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -no_logo -arch=amd64 -host_arch=amd64
if errorlevel 1 exit /b %errorlevel%
set "BUILD_ROOT=G:\Temp\mgif-p6-90-msvc-20260903T150000Z-Debug"
cmake --build "%BUILD_ROOT%" --target editor_ui_os_replay editor_ui_os_replay_plan_tests editor_ui_os_replay_runtime_tests editor_ui_os_replay_cli_tests --parallel 4
exit /b %errorlevel%
