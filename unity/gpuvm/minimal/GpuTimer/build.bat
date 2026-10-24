@call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /LD /O2 /EHsc /I C:\Development\VRChat\Unity-2022.3.22f1-fxc2\Data\PluginAPI gputimer.cpp d3d11.lib /Fe:GpuTimer.dll
