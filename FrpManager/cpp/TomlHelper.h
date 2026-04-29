// TomlHelper.h - TOML 读取封装（基于 toml++）
#pragma once
#include <string>
#include <string_view>
#include <optional>
#include "FrpConfig.h"

namespace TomlHelper {

bool Load(const std::wstring& path, FrpConfig& out);
std::wstring Utf8ToWide(std::string_view text);

}
