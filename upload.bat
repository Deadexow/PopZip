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
    echo [1/5] 初始化仓库...
    git init
    git branch -M main
)

echo [2/5] 设置远程仓库...
git remote remove origin >nul 2>nul
git remote add origin https://github.com/Deadexow/PopZip.git

echo [3/5] 暂存并提交...
git add -A
git commit -m "Update %DATE% %TIME%"
if errorlevel 1 echo [INFO] 没有新的改动需要提交。

if /i "%~1"=="--no-push" (
    echo [INFO] 已跳过推送（--no-push）。
    exit /b 0
)

echo [4/5] 同步远端（首次上传可忽略）...
git fetch origin main >nul 2>nul
if not errorlevel 1 (
    git rev-parse --verify origin/main >nul 2>nul
    if not errorlevel 1 (
        rem 以远端为基，保留本地工作区内容，避免无关历史导致 push 被拒
        git reset --soft origin/main
        git add -A
        git commit -m "PopZip %DATE% %TIME%" >nul 2>nul
    )
)

echo [5/5] 推送到 GitHub...
git push -u origin main
if errorlevel 1 (
    echo.
    echo [ERROR] 推送失败，常见原因：
    echo   1. 无法连接 GitHub（国内网络通常需要代理）
    echo      可临时设置代理后重新运行本脚本，例如：
    echo        set HTTPS_PROXY=http://127.0.0.1:7890
    echo   2. 未登录 GitHub 或凭据过期（Git Credential Manager / PAT）
    echo   3. 没有该仓库的写入权限
    echo.
    echo [提示] 若仓库已有你不知道的提交，请先手动检查：
    echo        git log origin/main --oneline
) else (
    echo.
    echo [OK] 已上传到 https://github.com/Deadexow/PopZip
)

pause