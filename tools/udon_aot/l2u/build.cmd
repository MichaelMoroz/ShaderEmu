@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0
cl /nologo /O2 /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:l2u.exe l2u.cpp
