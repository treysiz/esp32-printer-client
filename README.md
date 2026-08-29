# ESP32 Printer Client / ESP32 打印机客户端

[English](#english) | [中文](#chinese)

> **v2.0.0 — "WiFi 拉单" (WiFi pull) HTTP mode.** This firmware no longer uses
> WebSocket push. It adapts to the backend's HTTP pull-order channel:
> Bearer-authenticated polling, raw ESC/POS raster payloads (Chinese prints
> correctly), and `done`/`failed` job reporting. No backend changes required.

---

<h2 id="english">🇬🇧 English</h2>

An ESP32-S3 client that **polls** a backend over HTTP(S), pulls pending print
jobs for its printer, and streams the raw ESC/POS bytes to a local thermal
printer on TCP port 9100.

### How it works (main loop)
- **Heartbeat** — `POST <base>/printer-api/heartbeat` every **30 s** (keeps the
  printer "Online" in the admin UI).
- **Pull jobs** — `GET <base>/printer-api/jobs?limit=1&encoding=base64` every
  **~4 s**: one job at a time, oldest first. Each job's `content` is decoded to
  exact bytes and queued for printing `copies` times.
- **Report** — `POST .../jobs/<id>/done` on success, `.../jobs/<id>/failed` on
  failure (printer offline, out of paper, etc.).
- Every request carries `Authorization: Bearer <API_TOKEN>`.
- Last-20-job de-duplication prevents reprinting a job still `pending` in a
  poll that races with our report.

### The important bit: `content` decoding
`content` is the whole receipt rendered to an image as ESC/POS **raster** bytes
(`GS v 0`). Every job carries a `contentEncoding` field saying how those bytes
were packed into JSON, so the firmware never has to guess:

- **`base64`** — what we ask for (`?encoding=base64`), and the normal path.
  Pure ASCII, so `mbedtls_base64_decode()` hands back the original bytes
  verbatim.
- **`latin1`** — the legacy shape, still sent by a backend that predates the
  query param. Raw bytes latin1-decoded into a JSON string, transmitted as
  UTF-8; the firmware recovers the exact bytes by JSON-unescaping,
  UTF-8-decoding to code points, and taking `codepoint & 0xFF`. Kept as a
  fallback so one firmware image works against both backends.

Either way the result is byte-identical to the original ESC/POS, which is why
**Chinese menu names print correctly** — the text lives in the image, not in
ESC/POS text mode. The payload routinely contains `0x00` bytes (white pixels),
so it is binary end-to-end (explicit length, never `strlen`). See the decode
section of `main/http_client.c`.

### Hardware
- ESP32-S3 board (**PSRAM required** — see Memory below)
- LAN thermal printer listening on port 9100

### Configure
First boot (empty NVS) starts AP mode: connect to `PrinterBox_Setup`
(password `12345678`) and open `http://192.168.4.1`. Set WiFi, Printer IP/port,
**Backend URL** (e.g. `http://192.168.1.50:3000`, or `https://<domain>` in
production), and the **API Token** shown once when you add a *WiFi 拉单* printer
in the admin UI.

Compile-time defaults live in `main/config.h`:
```c
#define DEFAULT_WIFI_SSID    "YourWiFi"
#define DEFAULT_WIFI_PASS    "YourPassword"
#define DEFAULT_PRINTER_IP   "192.168.1.100"
#define DEFAULT_PRINTER_PORT 9100
#define DEFAULT_SERVER_URL   "http://your-server.com:3000"  // base URL, no path
#define DEFAULT_API_TOKEN    ""                              // Bearer token
```
Store ID / Device ID are no longer required — the backend identifies the
printer by its API token.

### Build & Flash (ESP-IDF v5.x)
```
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```
HTTPS works out of the box via the bundled root-CA store
(`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` in `sdkconfig.defaults`).

### Memory (PSRAM is required, not optional)
Measured over 535 real receipts, a `?encoding=base64` response runs **~137 KB
median and ~210 KB at the top end**. The response buffer
(`HTTP_RESP_PREFERRED`, 256 KB) covers that — **but only out of PSRAM**.

Without PSRAM the firmware deliberately caps the buffer at
`HTTP_RESP_INTERNAL_MAX` (48 KB), because a large buffer in internal RAM
starves the TLS handshake (`mbedtls_ssl_setup` → `-0x7F00`) and breaks HTTPS
outright. 48 KB can never hold a real receipt, so such a board will reach the
backend, log an overflow on every poll, and print nothing. **Use a board with
PSRAM.**

(For scale: the older `latin1` encoding put each white-pixel `0x00` on the wire
as a 6-byte JSON escape, inflating the same receipts ~5.6x to a 371 KB
median — which is why base64 is now the default request.)

### Backend API contract (consumed by the firmware)
| Method | Path | Body | Success |
|---|---|---|---|
| POST | `/printer-api/heartbeat` | none | `{"success":true,"printerId","name"}` |
| GET  | `/printer-api/jobs?limit=1&encoding=base64` | none | `{"success":true,"data":[ {id,content,contentEncoding,copies,...} ]}` |
| POST | `/printer-api/jobs/:id/done` | none | `{"success":true}` |
| POST | `/printer-api/jobs/:id/failed` | none | `{"success":true}` |

Auth errors: `401` (missing/invalid token), `403` (token's printer is not
`provider='wifi'`).

### Local testing without the backend
`test_server.py` is a mock backend implementing the same contract, including
`?limit=` (default 1, cap 20, illegal values fall back to the default) and
`?encoding=base64` with the matching `contentEncoding` field — so both the
base64 path and the latin1 fallback can be exercised locally. With Pillow
installed it renders a real Chinese raster receipt (`GS v 0`); otherwise it
falls back to plain ESC/POS text.
```
python3 test_server.py --port 3000 --token testtoken
```
Point the firmware at `http://<your-pc-ip>:3000` with token `testtoken`, then
press Enter in the console to queue jobs.

---

<h2 id="chinese">🇨🇳 中文</h2>

运行在 ESP32-S3 上的打印机客户端：通过 **HTTP(S) 轮询**后台，拉取本机待打印任务，
再把原始 ESC/POS 字节通过 TCP 9100 端口发给局域网热敏打印机。**本版本已从
WebSocket 推送改为后台“WiFi 拉单”HTTP 模式，无需改动后台任何代码。**

### 工作原理（主循环）
- **心跳**：每 **30 秒** `POST <base>/printer-api/heartbeat`（后台显示 Online）。
- **拉单**：每 **约 4 秒** `GET <base>/printer-api/jobs?limit=1&encoding=base64`，
  一次只取最早的一条，把任务的 `content` 还原为精确字节，按 `copies` 份数打印。
- **回报**：成功 `POST .../jobs/<id>/done`，失败 `POST .../jobs/<id>/failed`。
- 每个请求都带 `Authorization: Bearer <API_TOKEN>`。
- 保留“最近 20 单去重”，避免在回报与轮询竞争时重复打印仍为 `pending` 的任务。

### 关键点：content 的解码
`content` 是后台把整张小票渲染成图片后生成的 ESC/POS **光栅**指令（`GS v 0`）。
后台把原始字节用 latin1 解成字符串放进 JSON，传输时 JSON 为 UTF-8。固件通过
“JSON 反转义 → UTF-8 解码成码点 → 取 码点 & 0xFF”还原出**完全一致**的原始字节。
这就是**中文菜名能正常打印**的原因——文字在图片里，而非 ESC/POS 文本模式。光栅
数据里大量是 `0x00`（白色像素），所以全程按二进制处理（显式长度，绝不用 strlen）。
详见 `main/http_client.c` 的解码部分。

### 硬件需求
- ESP32-S3 开发板（**必须带 PSRAM**，见下方“内存说明”）
- 局域网热敏打印机（开放 9100 端口）

### 如何配置
全新烧录后进入热点模式：手机连接 `PrinterBox_Setup`（密码 `12345678`），浏览器
打开 `http://192.168.4.1`，填写 WiFi、打印机 IP/端口、**后台地址**（如
`http://192.168.1.50:3000`，生产用 `https://<域名>`）以及后台新增 *WiFi 拉单*
打印机时弹出的一次性 **API Token**。

也可在 `main/config.h` 写入编译期默认值：
```c
#define DEFAULT_WIFI_SSID    "您的WiFi"
#define DEFAULT_WIFI_PASS    "WiFi密码"
#define DEFAULT_PRINTER_IP   "192.168.1.100"
#define DEFAULT_PRINTER_PORT 9100
#define DEFAULT_SERVER_URL   "http://您的服务器地址:3000"  // 基础地址，不含路径
#define DEFAULT_API_TOKEN    ""                            // Bearer Token
```
店铺编号 / 设备编号已不再需要——后台通过 API Token 唯一定位打印机。

### 编译与烧录 (ESP-IDF v5.x)
```
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```
HTTPS 开箱即用（`sdkconfig.defaults` 已开启 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`）。

### 内存说明（为什么要 PSRAM）
小票大部分是白色像素 `0x00`，后台把每个 `0x00` 编码成 6 字节的 JSON 转义。所以
12KB 的光栅会膨胀成约 70KB 的 JSON，而 `GET /jobs` 会一次性返回**所有**待打印任务。
响应缓冲区（`HTTP_RESP_PREFERRED`，默认 256KB）优先从 PSRAM 分配，没有 PSRAM 时
自动缩小以适配内部 RAM。

### 后台 API 契约（固件调用）
| 方法 | 路径 | 请求体 | 成功 |
|---|---|---|---|
| POST | `/printer-api/heartbeat` | 无 | `{"success":true,"printerId","name"}` |
| GET  | `/printer-api/jobs?limit=1&encoding=base64` | 无 | `{"success":true,"data":[ {id,content,contentEncoding,copies,...} ]}` |
| POST | `/printer-api/jobs/:id/done` | 无 | `{"success":true}` |
| POST | `/printer-api/jobs/:id/failed` | 无 | `{"success":true}` |

鉴权错误：`401`（缺失/无效 Token），`403`（Token 对应打印机不是 `provider='wifi'`）。

### 不依赖后台的本地测试
`test_server.py` 是实现同一契约的模拟后台，同样支持 `?limit=`（默认 1、
上限 20、非法值收敛到默认值）与 `?encoding=base64` 及对应的 `contentEncoding`
字段——base64 主路径和 latin1 兜底路径都能在本地验证。装了 Pillow 会渲染出
真实中文光栅小票（`GS v 0`），否则降级为纯 ESC/POS 文本。
```
python3 test_server.py --port 3000 --token testtoken
```
然后把固件的后台地址设为 `http://<你的电脑IP>:3000`、Token 设为 `testtoken`，
在控制台按回车即可下发测试订单。
