@echo off
rem =====================================================================
rem  MRMSClient 构建脚本（MinGW-w64，静态链接，产物零依赖）
rem  1) 安装 MinGW-w64（推荐 winlibs 版）：https://winlibs.com/
rem     解压后把 bin 目录加入 PATH（含 g++）
rem  2) 双击本脚本编译，产出 MRMSClient.exe
rem  说明：默认跟随工具链位数；x86 构建可同时兼容 32/64 位系统
rem =====================================================================
chcp 65001 >nul 2>&1
cd /d "%~dp0"
where g++ >nul 2>&1
if errorlevel 1 (
  echo [错误] 未找到 g++，请先安装 MinGW-w64 并加入 PATH。
  echo        下载: https://winlibs.com/  或  https://www.msys2.org/
  pause & exit /b 1
)
echo [*] 正在编译 MRMSClient.exe ...
g++ MRMSClient.cpp -O2 -std=c++17 -municode -mwindows -static -static-libgcc -static-libstdc++ ^
   -o MRMSClient.exe -lgdiplus -lwinhttp -lshell32 -lole32 -ladvapi32
if errorlevel 1 (
  echo [错误] 编译失败，请把错误信息反馈给维护人员。
  pause & exit /b 1
)
echo [√] 编译完成: MRMSClient.exe
echo     直接双击运行（后台常驻），或用 /install 参数仅安装自启动。
pause
