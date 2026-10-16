# Windows upload checklist

1. Extract the folder and open `MedicalWearableBlynk.ino` in Arduino IDE 2.
2. Install **Arduino UNO R4 Boards** from Boards Manager.
3. Install these Library Manager packages: **OneWire**, **DallasTemperature**, **SparkFun MAX3010x Pulse and Proximity Sensor Library**, **Adafruit BMP085 Library**, and **Adafruit Unified Sensor**.
4. Open the `arduino_secrets.h` tab and check the Wi-Fi name, password, Blynk token, and wearable ID. If you use an iPhone hotspot, turn on **Settings > Personal Hotspot > Maximize Compatibility**; the UNO R4 WiFi only joins 2.4 GHz networks. Keep the hotspot screen open while the board connects the first time.
5. Select **Tools > Board > Arduino UNO R4 Boards > Arduino UNO R4 WiFi** and select the correct COM port.
6. Compile and upload. Open Serial Monitor at **9600 baud**.
7. Confirm that it prints `Wi-Fi connected` and then `Uploaded to Blynk` every five seconds.
8. Rest a fingertip gently on the MAX30102 and keep still. Heart rate and SpO2 appear after about 4–8 seconds. With no finger, the board sends 0 on V1/V2 and the CareFlow app shows "—" instead of an old value.

## Connect the CareFlow app

In the app, tap the antenna button, enter `blynk.cloud` (or your regional server) and the same device auth token, then tap **Connect to Blynk**. The patient's wearable ID must match `WEARABLE_ID` (`WR-1001`).

A missing sensor no longer stops the board: it prints which one wasn't found and keeps uploading the others.

If secure Blynk connections fail, update the UNO R4 WiFi connectivity firmware and certificates using Arduino IDE's firmware updater. Do not switch to unencrypted HTTP.

### If Serial Monitor says `Could not connect to Blynk Cloud`

1. Close Serial Monitor.
2. In Arduino IDE 2.2.1 or newer, open **Tools > Firmware Updater**.
3. Select the connected UNO R4 WiFi, click **Check Updates**, select the latest connectivity firmware, and install it.
4. Unplug and reconnect the board. The firmware update overwrites the sketch, so upload `MedicalWearableBlynk.ino` again.
5. In Blynk Console, read the regional server shown in the bottom-right corner. If it is, for example, `sgp1.blynk.cloud`, put that exact hostname in `BLYNK_SERVER` inside `arduino_secrets.h`.
6. Try a normal home network or a 2.4 GHz phone hotspot. Captive-portal, university, guest, and enterprise Wi-Fi can block embedded TLS clients.

## Wiring

- DS18B20 data -> D2, with a 4.7 kOhm pull-up from data to its supply.
- MAX30102 SDA/SCL -> UNO R4 SDA/SCL.
- BMP180 SDA/SCL -> the same UNO R4 SDA/SCL bus.
- All modules must share GND.
- Confirm each breakout's allowed supply and I2C voltage before powering it. Bare MAX30102/BMP180 sensors are not directly 5 V safe; use suitable breakout modules/level shifting.

The BMP180 measures atmospheric pressure, not blood pressure.

The firmware uses standard 100 kHz I2C and retries MAX30102 detection five times. This is more tolerant of jumper wires and a shared MAX30102/BMP180 bus than fast 400 kHz mode.
