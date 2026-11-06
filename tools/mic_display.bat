@echo off
python "%~dp0mic_display.py" %*
if errorlevel 1 pause
