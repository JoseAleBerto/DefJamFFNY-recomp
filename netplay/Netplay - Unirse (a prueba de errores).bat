@echo off
title Def Jam netplay - Unirse (a prueba de errores)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0netplay.ps1" -Mode join -Diag
pause
