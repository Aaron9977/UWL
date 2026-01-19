## 项目：UWL（ESP32‑C6 多通道 I/O 控制 + 手机 WebUI）

本项目面向 **ESP32‑C6 DevKitC‑1**，实现通过 **USB / Wi‑Fi / BLE** 三种通道控制板载/排针 GPIO，并在手机网页端实时显示状态。

### 功能概览
- **Wi‑Fi SoftAP**：手机直连开发板热点
- **HTTP Server**：提供手机端网页（两页：控制 / 配置）
- **WebSocket**：网页端实时双向控制与状态推送
- **BLE GATT（NimBLE）**：可用网页 Web Bluetooth 或 nRF Connect 控制/读状态
- **统一 I/O 状态核心**：三通道共享一套 GPIO 白名单与状态分发（`uwl_io_state`）
- **状态灯（WS2812）**：用不同颜色/闪烁表示运行状态（可开关）
- **统一指令协议**：Wi‑Fi(WS) 与 BLE 使用同一套命令/回包（短字段 + 兼容旧字段）

### 快速上手
#### 1) 构建
在已正确安装 ESP‑IDF（建议 v5.3.1）并完成环境导出后：

```powershell
cd D:\Lean\ESP32\UWL
. D:\Espressif\frameworks\esp-idf-v5.3.1\export.ps1
idf.py build
```

#### 2) 烧录
```powershell
idf.py -p COM3 -b 115200 flash
```

#### 3) 连接与访问
- 手机连接开发板 SoftAP（SSID/密码可在 menuconfig 配）
- 浏览器访问：
  - **控制页**：`http://192.168.4.1/` 或 `http://192.168.4.1/control`
  - **配置页**：`http://192.168.4.1/config`

> 网页资源已加 **禁用缓存** 响应头，但若你之前打开过旧页面，仍建议手机端“强制刷新/无痕模式”。

### WebUI（两页）
- **配置页 `/config`**
  - 可对排针（J1/J3）做“启用/禁用、别名、用途”配置（存手机本地 `localStorage`）
  - 支持“仅启用固件已配置”的快速按钮
  - 若某 GPIO 不在固件白名单，会标记 **固件未启用**（不可控）
- **控制页 `/control`**
  - 仅显示你在配置页中“启用”的 IO，尽量做到一屏操作（减少上下滑动）

### 指令协议（Wi‑Fi WS 与 BLE 统一）
#### 推荐：短字段（v2）
- **设置输出**
  - `{"t":"s","p":18,"v":1,"i":7}`
- **读取单 GPIO**
  - `{"t":"g","p":18,"i":8}`
- **列出/状态**
  - `{"t":"l","i":9}` / `{"t":"state","i":10}`

#### 兼容：旧字段（v1）
- `{"type":"gpio_set","pin":18,"value":1,"id":7}`
- `{"type":"state"}`

#### 统一回包（ACK/ERR）
- **成功**：`{"type":"resp","id":7,"ok":true,"data":{...}}`
- **失败**：`{"type":"err","id":7,"code":"NOT_FOUND|NOT_OUTPUT|BAD_ARG|...","msg":"..."}`

### BLE 使用方式
#### 1) 网页（Web Bluetooth）
网页内可直接点“连接 BLE”，浏览器会弹出设备选择（需要满足 Web Bluetooth 的浏览器与权限）。

> 注意：部分手机/浏览器对 Web Bluetooth 有限制；若遇到问题建议用 nRF Connect 验证 BLE 是否正常。

#### 2) nRF Connect（推荐调试）
除 JSON 外，固件还支持纯文本命令（更适合手动输入）：
- `s 18 1`（设置 GPIO18=1）
- `g 18`（读取 GPIO18）
- `l`（列出/状态）
- `state`（状态）

### USB 控制台（可选）
启用后可通过 USB Serial/JTAG 控制台执行命令（例如 GPIO/Wi‑Fi/WS/BLE 状态等）。
具体命令以固件编译时启用的功能为准。

### 配置（menuconfig）
项目提供 `Kconfig.projbuild` 配置项，用于开启/关闭：
- SoftAP SSID/密码
- 默认 GPIO 白名单/预设排针（DevKitC‑1 安全子集）
- USB 控制台 / BLE / 状态灯
- 状态灯 GPIO/亮度等

进入配置：
```powershell
idf.py menuconfig
```

### 常见问题
- **网页 UI 看不到更新**
  - 确保已重新烧录
  - 手机浏览器用无痕/强制刷新
  - 始终访问 `http://192.168.4.1/`，避免系统弹出的 portal 缓存页
- **WebSocket 断断续续**
  - 浏览器会并行打开多个连接（html/css/js + WS + /api/status），已在 HTTP server 配置里提高 socket 上限并启用 LRU purge
- **BLE 搜不到/连不上**
  - 确认已启用 NimBLE 相关配置
  - 手机开启蓝牙与定位权限（Android 常见）
  - 优先用 nRF Connect 验证，再回到网页端调试

### 目录结构（核心）
```
UWL/
├── CMakeLists.txt
├── sdkconfig
├── partitions.csv
└── main/
    ├── main.c
    ├── uwl_io_state.c/.h        # 统一 GPIO 白名单 + 状态分发
    ├── uwl_gpio.c/.h            # GPIO 驱动封装 + ISR
    ├── uwl_wifi_softap.c/.h     # SoftAP 管理（连接数）
    ├── uwl_http.c/.h            # HTTP 资源 + /api/status + 禁缓存
    ├── uwl_ws.c/.h              # WebSocket（统一协议、实时推送）
    ├── uwl_ble_gatt.c/.h        # BLE GATT（统一协议、文本命令）
    ├── uwl_usb_console.c/.h     # USB 控制台命令
    ├── uwl_status_led.c/.h      # WS2812 状态灯
    └── web/
        ├── control.html         # 控制页
        ├── config.html          # 配置页
        ├── app.js
        └── style.css
```
