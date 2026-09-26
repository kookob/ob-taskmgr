@echo off
cd /d %~dp0
windres taskmgr.rc -O coff -o res.o || exit /b 1
gcc -Os -s -static -municode -mwindows -Wall taskmgr.c res.o -o obtaskmgr.exe -lcomctl32 -lntdll -lshlwapi -luxtheme -lpdh || exit /b 1
del res.o
