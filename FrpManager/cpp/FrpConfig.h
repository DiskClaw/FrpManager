// FrpConfig.h
#pragma once
#include <string>
#include <vector>
#include <optional>

struct FrpProxy {
    std::string name;
    std::string type = "tcp";
    std::optional<std::string> localIP;
    std::optional<int> localPort;
    std::optional<int> remotePort;
};

struct FrpConfig {
    // 通用/服务端连接
    std::optional<std::string> common_server_addr;
    std::optional<int> common_server_port;
    std::optional<std::string> common_assets_base_dir;
    std::optional<int> common_pool_count;
    std::optional<bool> common_tcp_mux;
    std::optional<std::string> common_protocol;
    std::optional<std::vector<std::string>> common_start;
    std::optional<std::string> common_log_file;
    std::optional<std::string> common_log_level;
    std::optional<int> common_log_max_days;
    std::optional<std::string> common_auth_token;

    // 服务端监听
    std::optional<int> bind_port;
    std::optional<int> vhost_http_port;
    std::optional<int> vhost_https_port;

    // 仪表盘
    std::optional<int> dashboard_port;
    std::optional<std::string> dashboard_user;
    std::optional<std::string> dashboard_pwd;

    // 代理列表
    std::vector<FrpProxy> proxies;
};