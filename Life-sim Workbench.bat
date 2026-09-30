@echo off
if exist "%~dp0native\build\workbench.exe" (
  start "" "%~dp0native\build\workbench.exe"
) else (
  start "" "%~dp0native\workbench.exe"
)
