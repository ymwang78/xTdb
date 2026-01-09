@echo off
REM 构建脚本（Windows）

echo === xTdb 构建脚本 ===

REM 创建构建目录
if not exist build mkdir build
cd build

REM 配置 CMake
echo 配置 CMake...
REM 尝试使用常见的 Visual Studio 生成器，按优先级顺序
cmake .. -G "Visual Studio 18 2026" -A x64
if errorlevel 1 (
    cmake .. -G "Visual Studio 17 2022" -A x64 2>nul
    if errorlevel 1 (
        echo 错误：无法配置 CMake，请手动指定生成器
        exit /b 1
    )
)

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



