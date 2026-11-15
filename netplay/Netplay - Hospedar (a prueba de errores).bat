@echo off
title Def Jam netplay - Hospedar (a prueba de errores)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0netplay.ps1" -Mode host -Diag
pause
