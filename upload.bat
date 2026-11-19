@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] 未找到 Git，请先安装 Git for Windows。
    pause
    exit /b 1
)

if not exist .git (
    echo [1/4] 初始化仓库...
    git init
    git branch -M main
)

echo [2/4] 设置远程仓库...
git remote remove origin >nul 2>nul
git remote add origin https://github.com/Deadexow/PopZip.git

echo [3/4] 暂存并提交...
git add -A
git commit -m "Update %DATE% %TIME%"
if errorlevel 1 echo [INFO] 没有新的改动需要提交。

if /i "%~1"=="--no-push" (
    echo [INFO] 已跳过推送（--no-push）。

    exit /b 0
)

echo [4/4] 推送到 GitHub...
git push -u origin main
if errorlevel 1 (
    echo.
    echo [ERROR] 推送失败。常见原因：
    echo   - 未登录 GitHub 或凭据过期（重新登录 Git Credential Manager / 使用 PAT）
    echo   - 远程仓库已有内容，需要先执行：git pull --rebase origin main
    echo   - 没有该仓库的写入权限
) else (
    echo.
    echo [OK] 已上传到 https://github.com/Deadexow/PopZip
)

pause