# FrpManager 🔐

轻量级 Windows FRP 客户端/服务端进程管理器，原生 Win32 实现。

## 特性

- 同时管理 frpc 和 frps 进程，启动/停止一键操作
- 实时显示配置摘要（服务端地址、端口、代理列表、Dashboard 信息）
- TOML 配置文件热监测，外部修改自动刷新
- 可执行文件版本变更自动检测
- 开机自启（注册表方式）
- 初始化清理：一键停止所有 frp 进程、关闭自启、删除服务
- 系统托盘最小化，右键菜单控制
- 中英文自动切换（跟随系统 locale）
- 单实例保护，防止多开冲突
- DPI 感知（Per-Monitor V2），4K 缩放正常显示
- explorer 崩溃后自动重建托盘图标

## 界面

| 顶部工具栏 | 版本号 + 设置按钮 + frpc/frps 启停控制 |
|-----------|--------------------------------------|
| 中部卡片   | frpc/frps 配置摘要，实时输出滚动       |
| 底部状态栏 | 状态提示 + 开机自启勾选                |

## 构建

需要 Visual Studio 2022（v144/v145 工具集），C++20：

```bash
msbuild FrpManager.sln /p:Configuration=Release /p:Platform=x64
```

## 配置

首次运行会弹出设置对话框，指定 FRP 根目录（包含 frpc.exe/frps.exe 的文件夹）。程序会自动检测：

- `frpc.exe` / `frps.exe`
- `frpc.toml` / `frps.toml`（优先）或 `frpc.ini` / `frps.ini`

## 安全说明

- Dashboard 密码在摘要卡片中以掩码显示
- 初始化清理操作需二次确认
- 私钥等敏感配置不会被程序读取或显示

## License

MIT
