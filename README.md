# ESP32 RGB Web Controller 🎨

A lightweight, high-performance web-based RGB LED controller built specifically for the ESP32 using the native **ESP-IDF** framework. 

This project provides a sleek, responsive, and mobile-friendly web dashboard hosted directly on the ESP32. It allows you to control a WS2812/NeoPixel RGB LED strip in real-time with zero noticeable latency, utilizing FreeRTOS tasks and hardware RMT for smooth operations.

## ✨ Features
- **Responsive Web Dashboard**: A modern, dark-themed UI with sliders for Red, Green, and Blue channels.
- **Zero-Latency Feel**: Highly optimized HTTP handlers and a dedicated FreeRTOS LED task ensure that UI slider changes reflect instantly on the hardware.
- **Wi-Fi Auto-Reconnect**: Robust Wi-Fi station mode handling that automatically retries connections if the signal drops.
- **Hardware Override**: Pressing the physical BOOT button instantly turns the LED off.
- **Configurable via Menuconfig**: Easily set your Wi-Fi SSID and Password without hardcoding them into the C source code.

---

## 📸 Gallery

*(Replace these placeholder links with your actual screenshots)*

### Web UI Dashboard
![Web Dashboard UI](docs/images/web_ui_screenshot.png)
*The mobile-friendly dashboard accessible via any browser on the local network.*

### Hardware Setup
![ESP32 Hardware Setup](docs/images/hardware_setup.jpg)
*ESP32 connected to the WS2812 RGB LED.*

---

## 🛠 Build It Yourself (DIY Guide)

### Bill of Materials (BOM)
To build this project, you will need the following components:
1. **ESP32 Development Board**: Any standard ESP32 or ESP32-S3 board will work.
2. **WS2812B RGB LED Strip**: Also known as NeoPixels. You can use a single LED or a strip.
3. **Power Supply**: 
   - If using a few LEDs, the 3.3V/5V pin from the ESP32 (via USB) is sufficient.
   - For longer strips (e.g., > 10 LEDs), use an external 5V power supply (ensure common ground with the ESP32).
4. **Jumper Wires**: To connect the LED to the ESP32.

### Wiring Guide
Connect the WS2812B LED strip to the ESP32 as follows:

| WS2812B Pin | ESP32 Pin | Notes |
| :--- | :--- | :--- |
| **VCC / 5V** | `VIN` or `5V` | Use external 5V power for long strips. |
| **GND** | `GND` | Must share common ground if using external power. |
| **DIN (Data In)** | `GPIO 48` | Configurable in `main.c` (`RGB_LED_GPIO`). |

*Optional:* The physical **BOOT** button (usually `GPIO 0` on most dev boards) is programmed to instantly turn off the LED when pressed. No extra wiring needed for this!

---

## 🚀 Getting Started

### 1. Prerequisites
Ensure you have the Espressif IoT Development Framework ([ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)) installed. This project requires **ESP-IDF v4.1 or newer**.

### 2. Clone the Repository
```bash
git clone https://github.com/Bouch2004/WIFI-LED.git
cd WIFI-LED
```

### 3. Configuration
Set your Wi-Fi credentials using the ESP-IDF menuconfig tool:
```bash
idf.py menuconfig
```
Navigate to **RGB Web Controller Configuration** and enter your:
- `WiFi SSID`
- `WiFi Password`

### 4. Build and Flash
Build the project, flash it to your ESP32, and open the serial monitor to get the IP address:
```bash
idf.py build flash monitor
```

### 5. Access the Dashboard
Once the ESP32 boots and connects to your Wi-Fi network, it will print its IP address in the serial monitor:
```text
I (1234) RGB_DASHBOARD: Got IP: 192.168.1.55 — open this in your browser!
```
Type that IP address into your smartphone or computer's web browser to access the dashboard.

---

## 📂 Project Structure
- `main/main.c`: Core application logic (Wi-Fi, HTTP Server, GPIO, FreeRTOS tasks).
- `main/Kconfig.projbuild`: Custom menuconfig definitions for Wi-Fi credentials.
- `main/idf_component.yml`: Dependency manager file (fetches `espressif/led_strip`).
- `CMakeLists.txt`: Build system configuration.
