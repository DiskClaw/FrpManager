#include "TomlHelper.h"
#include "FrpConfig.h"
#include "toml++/toml.hpp"
#include <windows.h>
#include <stdexcept>
#include <string>

namespace {

    template<class T>
    std::optional<T> getValue(const toml::node* node) {
        if (!node) return std::nullopt;
        if constexpr (std::is_same_v<T, std::string>) {
            if (auto s = node->as_string()) return std::string(s->get());
        }
        else if constexpr (std::is_same_v<T, int>) {
            if (auto i = node->as_integer()) return static_cast<int>(i->get());
        }
        else if constexpr (std::is_same_v<T, bool>) {
            if (auto b = node->as_boolean()) return b->get();
        }
        return std::nullopt;
    }

    std::optional<std::vector<std::string>> getStringArray(const toml::node* node) {
        if (auto* arr = node ? node->as_array() : nullptr) {
            std::vector<std::string> result;
            for (const auto& item : *arr) {
                if (auto s = item.as_string()) result.emplace_back(s->get());
            }
            if (!result.empty()) return result;
        }
        return std::nullopt;
    }

    void loadFields(const toml::table& src, FrpConfig& dst) {
        dst.common_server_addr = getValue<std::string>(src.get("serverAddr"));
        dst.common_server_port = getValue<int>(src.get("serverPort"));
        dst.common_assets_base_dir = getValue<std::string>(src.get("assetsBaseDir"));
        dst.bind_port = getValue<int>(src.get("bindPort"));
        dst.vhost_http_port = getValue<int>(src.get("vhostHTTPPort"));
        dst.vhost_https_port = getValue<int>(src.get("vhostHTTPSPort"));
        dst.common_pool_count = getValue<int>(src.get("poolCount"));
        dst.common_tcp_mux = getValue<bool>(src.get("tcpMux"));
        dst.common_protocol = getValue<std::string>(src.get("protocol"));
        dst.common_start = getStringArray(src.get("start"));
        dst.common_log_file = getValue<std::string>(src.get("logFile"));
        dst.common_log_level = getValue<std::string>(src.get("logLevel"));

        if (auto* auth = src.get_as<toml::table>("auth")) {
            dst.common_auth_token = getValue<std::string>(auth->get("token"));
        }
        if (auto* log = src.get_as<toml::table>("log")) {
            dst.common_log_max_days = getValue<int>(log->get("maxDays"));
        }
    }

    // 尝试从表中读取一个字符串，支持多个字段名（按优先级）
    std::optional<std::string> getFirstString(const toml::table* tbl, std::initializer_list<const char*> keys) {
        if (!tbl) return std::nullopt;
        for (const auto& key : keys) {
            if (auto v = getValue<std::string>(tbl->get(key))) {
                return v;
            }
        }
        return std::nullopt;
    }

    // 解析仪表盘信息，支持多种节名称和字段名
    void parseDashboard(const toml::table& root, FrpConfig& out) {
        // 尝试多个节名称
        const toml::table* tbl = nullptr;
        for (const auto& section : { "webServer", "dashboard", "dashboardUI" }) {
            tbl = root.get_as<toml::table>(section);
            if (tbl) break;
        }
        if (!tbl) return;

        // 端口
        out.dashboard_port = getValue<int>(tbl->get("port"));

        // 用户名：尝试 user, username
        auto user = getFirstString(tbl, { "user", "username" });
        if (user) out.dashboard_user = *user;

        // 密码：尝试 password, pwd, pass
        auto pwd = getFirstString(tbl, { "password", "pwd", "pass" });
        if (pwd) out.dashboard_pwd = *pwd;
    }

} // anonymous namespace

// 解析 TOML 配置文件到 FrpConfig 结构体
bool TomlHelper::Load(const std::wstring& path, FrpConfig& out) {
    try {
        out = {};
        auto tbl = toml::parse_file(path);

        // 根表字段
        loadFields(tbl, out);
        // [common] 表覆盖
        if (auto common = tbl.get_as<toml::table>("common")) {
            loadFields(*common, out);
        }

        // 解析仪表盘（支持多种节名和字段名）
        parseDashboard(tbl, out);

        // 解析 [[proxies]]
        if (auto* proxies = tbl.get_as<toml::array>("proxies")) {
            for (const auto& item : *proxies) {
                if (auto* proxy = item.as_table()) {
                    FrpProxy p;
                    p.name = getValue<std::string>(proxy->get("name")).value_or("");
                    if (p.name.empty()) continue;
                    p.type = getValue<std::string>(proxy->get("type")).value_or("tcp");
                    p.localIP = getValue<std::string>(proxy->get("localIP"));
                    p.localPort = getValue<int>(proxy->get("localPort"));
                    p.remotePort = getValue<int>(proxy->get("remotePort"));
                    out.proxies.push_back(std::move(p));
                }
            }
        }

        return true;
    }
    catch (const toml::parse_error& err) {
        OutputDebugStringA(err.what());
        return false;
    }
    catch (const std::exception& ex) {
        OutputDebugStringA(ex.what());
        return false;
    }
}

// UTF-8 字符串转 Unicode 宽字符串
std::wstring TomlHelper::Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), len);
    return result;
}