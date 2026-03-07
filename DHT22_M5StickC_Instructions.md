
# Quick Start: DHT22 on M5StickC with ESP-IDF

## 0. Install ESP-IDF Extension and Tools

1. In VS Code, go to the Extensions view (Ctrl+Shift+X).
2. Search for **ESP-IDF** and install the official extension by Espressif Systems.
3. After installation, open the **ESP-IDF: Doctor** or **ESP-IDF: Installation Manager** from the Command Palette (Ctrl+Shift+P).
4. Use the Installation Manager to install or update all required ESP-IDF tools and Python dependencies.
5. Restart VS Code when setup is complete.

## 1. Build and Flash Firmware

1. Open the **ESP-IDF Terminal** in VS Code.
2. Navigate to your project folder:
   ```sh
   cd Your_Folder\Smart-Air-Monitoring
   ```
3. Build the project:
   ```sh
   idf.py build
   ```
4. Flash and monitor (replace COM10 with your port if needed):
   ```sh
   idf.py -p COM10 -b 115200 flash monitor
   ```
5. To exit monitor: Press `Ctrl + ]`

## 2. DHT22 (3-pin) to M5StickC Wiring

- **DHT22 VCC**  →  **3.3V** on M5StickC
- **DHT22 DATA** →  **G26** on M5StickC
- **DHT22 GND**  →  **GND** on M5StickC

## 3. What to Expect
- Serial monitor will show repeated temperature and humidity readings.
- LCD will display the same values.
- If you see errors, double-check wiring and that your code uses `setDHTgpio(26);`.

## 4. Troubleshooting
- **Sensor Timeout**: Check DATA pin and wiring.
- **CheckSum error**: Try shorter wires, stable power.
- **No readings**: Confirm correct GPIO and pull-up resistor.

## Attribution

DHT22 driver logic and explanation adapted from  
[https://esp32tutorials.com/dht22-esp32-esp-idf/](https://esp32tutorials.com/dht22-esp32-esp-idf/)  
(licensed under CC0/public domain).
---

