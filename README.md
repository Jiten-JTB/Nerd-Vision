# Nerd Vision

Nerd Vision is an ESP32-powered smart glasses firmware that turns a pair of glasses into a Bluetooth audio device. It streams music from your Smartphone via A2DP, handles hands-free calls via HFP with a MEMS microphone, and lets you control everything with a single button — all delivered silently through bone conduction so you stay aware of your surroundings.

---

## Project in Action

Check out Nerd Vision on social media:

- 📸 [**Instagram Reel**](https://www.instagram.com/reel/DWoYZvcTyCL/?igsh=MTR2b2swYmJiejNhcw%3D%3D)
- 🎬 [**YouTube Short**](https://youtube.com/shorts/if-15_G5w14?si=MH0i1Gnol6Ymscfx)

---

## Hardware

| Component | Details |
|---|---|
| **Microcontroller** | ESP32 Wrover-E — 520 KB SRAM, 4 MB Flash, 8 MB PSRAM |
| **Amplifier** | Adafruit MAX98357A — I2S Class D amplifier |
| **Speaker** | Adafruit Bone Conduction Transducer |
| **Microphone** | ICS-43434 MEMS I2S Microphone (SEL pin tied to GND → Left channel) |
| **Button** | Momentary push button on GPIO 32 |

### Pin Definitions

| Signal | GPIO |
|---|---|
| MIC BCLK | 26 |
| MIC WS | 25 |
| MIC DIN | 33 |
| SPK BCLK | 14 |
| SPK WS | 15 |
| SPK DOUT | 22 |
| AMP SD (shutdown) | 27 |
| Button | 32 |

---

## Bluetooth Profiles

| Profile | Role | Purpose |
|---|---|---|
| **A2DP** | Sink | Receive stereo audio from phone — music streaming |
| **AVRCP** | Target + Controller | Play/Pause control from button |
| **HFP** | Hands Free Unit (HF) | Phone calls — bidirectional audio via SCO |

---

## IDE & Toolchain

- **IDE:** ESP-IDF v5.4
- **Target:** `esp32`
- **Build system:** CMake via `idf.py`

---

## Features

- **A2DP music streaming** — high quality audio through bone conduction
- **HFP hands-free calling** — mSBC wideband (16 kHz) when supported
- **Microphone input** — ICS-43434 captures voice during calls
- **Single click** — Play / Pause music
- **Double click** — Trigger Siri or Google Assistant
- **Short Single press during call** — Answer incoming call
- **Long press during call** — Reject or end call
- **Auto-resume A2DP** after call ends, restoring correct sample rate
- **Dynamic I2S reconfiguration** — switches between stereo (A2DP) and mono (HFP SCO) automatically

---

## Project Structure

```
smart_glasses/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig
└── main/
    ├── CMakeLists.txt
    ├── main.c          # Entry point, button handling
    ├── bt_a2dp.c/h     # A2DP sink + AVRCP
    ├── bt_hfp.c/h      # HFP hands-free unit
    └── i2s_audio.c/h   # I2S speaker driver
```

---

## Getting Started

### 1. Prerequisites

- ESP-IDF v5.4 installed and sourced
- ESP32 Wrover-E board
- All hardware connected per the pin table above

### 2. Clone the repository

```bash
git clone https://github.com/Jiten-JTB/Nerd-Vision.git
```

### 3. Set target

```bash
idf.py set-target esp32
```

### 4. Apply menuconfig changes

Run menuconfig and apply all settings listed in the [Menuconfig](#menuconfig) section below:

```bash
idf.py menuconfig
```

### 5. Build, flash and monitor

```bash
idf.py build flash monitor -p /dev/your-port
```

Replace `/dev/your-port` with your actual serial port.

### 6. Pair your phone

Search for **"SmartGlasses"** in your phone's Bluetooth settings and pair. Both A2DP and HFP will connect automatically.

---

## Menuconfig

Run `idf.py menuconfig` and apply the following changes before building.

### Partition Table

```
Partition Table
  → Partition Table → Custom partition table CSV
  → Custom partition CSV file → partitions.csv
```

### Flash Size

```
Serial flasher config
  → Flash size → 4 MB
```

### Bluetooth Controller Options

```
Component config → Bluetooth → Controller Options
  → Bluetooth controller mode → BR/EDR Only
  → BR/EDR ACL Max Connections → 2
  → BR/EDR Sync (SCO/eSCO) Max Connections → 1
  → BR/EDR Sync (SCO/eSCO) default data path → HCI
```

### Bluetooth Bluedroid Options

```
Component config → Bluetooth → Bluedroid Options
  → Bluetooth event callback task stack size → 8192
  → Bluetooth Bluedroid Host Stack task stack size → 8192
  → [*] Classic Bluetooth
      → [*] A2DP
      → [*] AVRCP
      → [*] Hands Free/Handset Profile
          → [*] Hands Free Unit
          → [ ] Audio Gateway         ← disable this
          → audio(SCO) data path → HCI
          → [*] Wide Band Speech
  → [ ] Bluetooth Low Energy          ← disable this
  → [*] BT/BLE will first malloc the memory from the PSRAM
```

### PSRAM

```
Component config → ESP PSRAM
  → [*] Support for external, SPI-connected RAM
  → SPI RAM config
      → Type of SPI RAM chip → Auto-detect
      → [*] Initialize SPI RAM during startup
      → SPI RAM access method → Make RAM allocatable using malloc() as well
```

---

## Custom Partition Table

The default IDF partition table is too small for the Bluedroid stack. The project uses a custom `partitions.csv`:

```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xF000,   0x1000,
factory,  app,  factory, 0x10000,  0x1F0000,
```

---

## Audio Architecture

```
A2DP mode (Higher Quality, Simplex):
Phone → BT A2DP → Ring buffer (PSRAM) → I2S TX (stereo, 44.1/48 kHz) → MAX98357A → Bone conductor

HFP mode (Lower Quality, Full-duplex):
• Phone → BT SCO → Ring buffer (PSRAM) → I2S TX (mono, 16 kHz) → MAX98357A → Bone conductor
• ICS-43434 → I2S RX (mono, 32-bit, 16 kHz) → hfp_outgoing_data_cb → BT SCO → Phone
```

---

## Knowledge & References

Suggested Video resources that were helpful in understanding the concepts behind this project:

### MAX98357A Configuration

[**Atomic14's MAX98357A Video**](https://youtu.be/At8PDQ3g7FQ?si=MoPm6pqlfECXZnD4) — Important video for MAX98357A wire configuration.

### Bluetooth HFP on ESP32

[**Atomic14's HFP Video**](https://youtu.be/0jR-QNTfydA?si=vwXPoK3o9qQmruYq) — To gain knowledge about Bluetooth HFP on ESP32.

### ESP32 Bluetooth

[**DroneBot Workshop's 
Bluetooth Video**](https://youtu.be/0Q_4q1zU6Zc?si=h9SjRoNIHXa_2AAc) — To learn about 
Bluetooth Classic & BLE on ESP32.