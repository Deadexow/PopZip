#pragma once

#include <windows.h>

#include <string>

enum class ConflictChoice { Overwrite, Skip, Rename, Cancel };

// 目标目录已存在同名文件时询问处理方式；owner 会被临时禁用。
ConflictChoice ShowConflictDialog(HWND owner, const std::wstring& targetDir);