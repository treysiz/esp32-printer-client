# ESP32 Printer Client

This is a robust ESP32-S3 client application that connects to a cloud WebSocket server to receive print orders, and forwards them to a local thermal printer via TCP port 9100.

## Features
- **WiFi & Network**: Supports DHCP (default) and Static IP, with robust auto-reconnect.
- **WebSocket**: Maintains a persistent connection to the cloud server, handles registration, responds to ping/pong heartbeats, and automatically reconnects with an exponential backoff mechanism upon disconnection.
- **Print Forwarding**: Listens for incoming JSON orders, deduplicates them (remembers the last 20 orders), and places them in a FreeRTOS queue. A dedicated task sends the order to a local printer on port 9100.
- **Reliability**: Uses queues to decouple tasks to prevent blocking. The printer task retries failed jobs up to 3 times before giving up. Status results are sent back to the cloud.
- **NVS Configuration**: All core parameters are loaded from NVS. If NVS is empty, it falls back to compile-time defaults.

## Hardware Requirements
- ESP32-S3 Development Board
- Local Network with a Thermal Receipt Printer (Port 9100)

## Configuration
When the firmware is flashed for the first time (empty NVS), it uses the defaults in `main/config.h`. 
To test your own environment, either edit `main/config.h` before building, or use an NVS initialization script/tool to flash values.

```c
#define DEFAULT_WIFI_SSID       "YourWiFi"
#define DEFAULT_WIFI_PASS       "YourPassword"
#define DEFAULT_STORE_ID        "store_001"
#define DEFAULT_DEVICE_ID       "printer_001"
#define DEFAULT_PRINTER_IP      "192.168.1.100"
#define DEFAULT_PRINTER_PORT    9100
#define DEFAULT_SERVER_URL      "ws://your-server.com:3001/printer"
```

## Build & Flash (ESP-IDF v5.x)

1. Set up the ESP-IDF environment:
   ```bash
   . $HOME/esp/esp-idf/export.sh
   ```

2. Set the target to ESP32-S3:
   ```bash
   idf.py set-target esp32s3
   ```

3. Build the project:
   ```bash
   idf.py build
   ```

4. Flash the firmware and monitor serial output:
   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```
   *(Replace `/dev/ttyUSB0` or `COMx` with your actual serial port)*

## Cloud WebSocket API

### 1. Device Registration (ESP32 -> Server)
Upon successful WebSocket connection, the device sends:
```json
{
  "type": "register",
  "store_id": "store_001",
  "device_id": "printer_001",
  "firmware_version": "1.0.0"
}
```

### 2. Print Order (Server -> ESP32)
The server should push orders in this format:
```json
{
  "type": "print",
  "order_id": "123456",
  "content": "***************\n NEW ORDER\n***************\nChicken Fried Rice x1\nBeef Lo Mein x2\n----------------\nThank you!\n"
}
```

### 3. Print Result (ESP32 -> Server)
After attempting to print, the device reports the status:

**Success:**
```json
{
  "type": "print_result",
  "status": "success",
  "order_id": "123456"
}
```

**Failed:**
```json
{
  "type": "print_result",
  "status": "failed",
  "order_id": "123456",
  "reason": "connect_timeout"
}
```

### 4. Heartbeat
The ESP32 is configured to handle `ping` messages and will automatically respond with a `pong` message.
**Server Ping:**
```json
{
  "type": "ping"
}
```
**ESP32 Pong:**
```json
{
  "type": "pong"
}
```

## Testing

You can use the included `test_server.py` script to simulate the cloud server.

1. Install websockets: `pip install websockets`
2. Run the server: `python test_server.py`
3. Configure `DEFAULT_SERVER_URL` in `config.h` to point to the PC's IP, e.g., `ws://192.168.1.50:3001/printer`
4. The server script provides an interactive prompt to send test orders.
