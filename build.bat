@echo off
rem Manual MSVC + Windows SDK environment (no vcvarsall / reg.exe)
set "VSROOT=D:\Program Files\Visual Studio\Visual Studio1"
set "MSVC=%VSROOT%\VC\Tools\MSVC\14.52.37025"
set "SDK=D:\Windows Kits\10"
set "SDKVER=10.0.26100.0"

set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%SDK%\bin\x64;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt;%SDK%\Include\%SDKVER%\cppwinrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"

set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

"%CMAKE%" -G Ninja -B "D:\Xiangmu\S-Rack\build" -S "D:\Xiangmu\S-Rack" -DCMAKE_BUILD_TYPE=Release %*
if errorlevel 1 exit /b 1

"%CMAKE%" --build "D:\Xiangmu\S-Rack\build" --config Release --target VstMediaPlayer_VST3 VstMediaPlayer_Standalone AudioDump SepVerify %*
