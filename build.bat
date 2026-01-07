@echo off
REM 构建脚本（Windows）

echo === xTdb 构建脚本 ===

REM 创建构建目录
if not exist build mkdir build
cd build

REM 配置 CMake
echo 配置 CMake...
cmake .. -G "Visual Studio 18 2026" -A x64

REM 编译
echo 编译项目...
cmake --build . --config Release

REM 运行测试（如果启用）
if "%1"=="--test" (
    echo 运行测试...
    ctest --output-on-failure
)

echo === 构建完成 ===

cd ..



