#pragma once

#include <windows.h>

#include <filesystem>
#include <vector>

#include "compress_engine.h"

bool ShowCompressDialog(HWND parent,
                        const std::vector<std::filesystem::path>& sources,
                        CompressOptions& options);