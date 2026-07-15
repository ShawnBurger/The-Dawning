@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0smoke_test.ps1" %*

