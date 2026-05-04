# ESP32 Printer Client / ESP32 打印机客户端

[English](#english) | [中文](#chinese)

---

<h2 id="english">🇬🇧 English</h2>

This is a robust ESP32-S3 client application that connects to a cloud WebSocket server to receive print orders, and forwards them to a local thermal printer via TCP port 9100. It is designed to be highly reliable in commercial environments (like restaurants).

### Features
- **Smart Web Config UI**: An embedded, mobile-friendly configuration portal. Boot into AP mode (`PrinterBox_Setup`) if WiFi is unavailable, allowing users to set up WiFi, Printer, and Server properties easily.
- **WiFi & Network**: Supports DHCP (default) and Static IP, with robust auto-reconnect and a 5-second automatic fallback to AP mode if the connection drops or fails.
- **WebSocket**: Maintains a persistent connection to the cloud server, handles registration, responds to ping/pong heartbeats, and automatically reconnects with an exponential backoff mechanism upon disconnection.
- **Print Forwarding**: Listens for incoming JSON orders, deduplicates them (remembers the last 20 orders), and places them in a FreeRTOS queue. A dedicated task sends the order to a local printer on port 9100.
- **Real-time Diagnostics**: Performs real TCP pings and health checks before printing or showing status, ensuring no "fake success" messages.
- **NVS Configuration**: All core parameters are loaded from NVS. If NVS is empty, it falls back to compile-time defaults. Long pressing the BOOT button for 5 seconds factory resets the device.

### Hardware Requirements
- ESP32-S3 Development Board
- Local Network with a Thermal Receipt Printer (Port 9100)

### Configuration
When the firmware is flashed for the first time (empty NVS), it automatically enters AP mode. You can connect to `PrinterBox_Setup` and browse to `http://192.168.4.1` to configure the device via a user-friendly UI. 

Alternatively, you can edit `main/config.h` before building:
```c
#define DEFAULT_WIFI_SSID       "YourWiFi"
#define DEFAULT_WIFI_PASS       "YourPassword"
#define DEFAULT_STORE_ID        "store_001"
#define DEFAULT_DEVICE_ID       "printer_001"
#define DEFAULT_PRINTER_IP      "192.168.1.100"
#define DEFAULT_PRINTER_PORT    9100
#define DEFAULT_SERVER_URL      "ws://your-server.com:3001/printer"
```

### Build & Flash (ESP-IDF v5.x)
1. Set up the ESP-IDF environment: `. $HOME/esp/esp-idf/export.sh`
2. Set the target: `idf.py set-target esp32s3`
3. Build the project: `idf.py build`
4. Flash the firmware: `idf.py flash monitor`

### Cloud WebSocket API
**1. Device Registration (ESP32 -> Server)**
```json
{
  "type": "register",
  "store_id": "store_001",
  "device_id": "printer_001",
  "firmware_version": "1.0.0"
}
```

**2. Print Order (Server -> ESP32)**
```json
{
  "type": "print",
  "order_id": "123456",
  "content": "***************\n NEW ORDER\n***************\nChicken Fried Rice x1\nBeef Lo Mein x2\n"
}
```

**3. Print Result (ESP32 -> Server)**
Success:
```json
{ "type": "print_result", "status": "success", "order_id": "123456" }
```
Failed:
```json
{ "type": "print_result", "status": "failed", "order_id": "123456", "reason": "connect_timeout" }
```

---

<h2 id="chinese">🇨🇳 中文</h2>

这是一个高可用性的 ESP32-S3 打印机客户端应用。它通过 WebSocket 协议连接到云端服务器接收打印订单，然后通过 TCP 9100 端口将订单转发给局域网内的热敏打印机，专为餐馆等高要求商用环境设计。

### 核心功能
- **智能网页配置 UI**：内置适合手机操作的网页后台。支持中英双语，提供傻瓜式的三步配网向导，支持一键测网络、测打印。
- **WiFi 与网络容错**：支持 DHCP 和静态 IP。开机时如果 5 秒内无法连接到 WiFi，设备会自动降级并开启 `PrinterBox_Setup` 蓝牙热点供用户紧急修复网络。
- **长连接与重连**：与云端服务器保持稳定的 WebSocket 连接，支持心跳检测，断线后通过指数退避算法自动重连。
- **订单打印与去重**：采用 FreeRTOS 异步队列处理打印任务，自动记录并过滤最近 20 个重复订单，防止网络抖动导致的重复打印。具备最高 3 次失败重试机制。
- **真实连通性拨测**：状态查询与一键测试功能均采用真实的 TCP 握手拨测，拒绝“假成功”，实时反馈打印机与服务器的物理连通状态。
- **硬件防砖**：所有配置保存在 NVS。支持长按开发板的 BOOT 键 5 秒强制恢复出厂设置并进入配网模式。

### 硬件需求
- ESP32-S3 开发板
- 支持局域网网口/WiFi 的热敏票据打印机（默认开放 9100 端口）

### 如何配置
全新烧录后，ESP32 会自动开启热点。
用手机连接 WiFi：`PrinterBox_Setup`（密码：`12345678`），然后浏览器访问 `http://192.168.4.1` 即可进入图形化配置后台。

如果您想直接将默认参数编译进固件，可修改 `main/config.h`：
```c
#define DEFAULT_WIFI_SSID       "您的WiFi"
#define DEFAULT_WIFI_PASS       "WiFi密码"
#define DEFAULT_STORE_ID        "store_001"
#define DEFAULT_DEVICE_ID       "printer_001"
#define DEFAULT_PRINTER_IP      "192.168.1.100"
#define DEFAULT_PRINTER_PORT    9100
#define DEFAULT_SERVER_URL      "ws://您的服务器地址:3001/printer"
```

### 编译与烧录 (基于 ESP-IDF v5.x)
1. 激活环境：`. $HOME/esp/esp-idf/export.sh`
2. 设置芯片：`idf.py set-target esp32s3`
3. 编译：`idf.py build`
4. 烧录并监控：`idf.py flash monitor`

### 云端 WebSocket API 接口规范
**1. 设备上线注册 (ESP32 -> 服务器)**
建立连接后自动发送：
```json
{
  "type": "register",
  "store_id": "store_001",
  "device_id": "printer_001",
  "firmware_version": "1.0.0"
}
```

**2. 下发打印订单 (服务器 -> ESP32)**
```json
{
  "type": "print",
  "order_id": "123456",
  "content": "***************\n 新订单\n***************\n黄焖鸡米饭 x1\n手撕包菜 x1\n"
}
```

**3. 回传打印结果 (ESP32 -> 服务器)**
成功：
```json
{ "type": "print_result", "status": "success", "order_id": "123456" }
```
失败：
```json
{ "type": "print_result", "status": "failed", "order_id": "123456", "reason": "connect_timeout" }
```
