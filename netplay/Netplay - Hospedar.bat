@echo off
title Def Jam netplay - Hospedar
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0netplay.ps1" -Mode host
pause
