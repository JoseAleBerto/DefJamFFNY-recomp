@echo off
title Def Jam (a prueba de errores)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0netplay.ps1" -Mode solo -Diag
pause
