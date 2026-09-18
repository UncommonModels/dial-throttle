---
title: "Getting started"
description: "Hardware, wiring, and building and flashing the firmware."
weight: 1
---

## Hardware

- M5Stack Dial (ESP32-S3)
- SparkFun Qwiic Keypad/Numpad (COM-15290), optional
- Qwiic cable

The [parts list]({{< relref "docs/parts" >}}) has the exact items, with Digi-Key numbers. A
printable case is in the repository under `case/` (`M5DialController.stl`, with the FreeCAD
source alongside).

## Wiring

The Qwiic connector carries `3V3`, `GND`, `SDA` and `SCL`. Plug the Qwiic cable into the Dial's
Grove port (Port A), or into an adapter that exposes `3V3/GND/SDA/SCL`.

The firmware expects the keypad at the SparkFun default I2C address, `0x4B`. A keypad plugged in
after boot is picked up within about 10 seconds.

### Battery
The M5Stack Dial has a Molex Picoblade 1.25mm pitch connector under the Stamp module on the bottom of the dial. The parts list suggests a preassembled cable to break this out into wire leads. **BE EXTREMELY CAREFUL ABOUT POLARITY**. The PCB should have an indicator of the positive terminal. This may not be the red cable in a red/black pair for an assembled cable. If you choose not to use the 18650 battery setup (which requires soldering of the leads to the battery holder), you likely can find a smaller battery and shrink the height of the case. However, this will likely require constructing a custom adapter cable.

## Project layout

| Path | Contents |
| --- | --- |
| `DialThrottle/` | The Arduino sketch. `setup()` and `loop()` are in `main.cpp` |
| `DialThrottle/config.h` | Compile-time defaults and hardware options |
| `DialThrottle/sketch.yaml` | Board options and pinned core and library versions |
| `Makefile` | Build, flash, monitor and docs targets |
| `install.py` | Guided installer for people who don't want to use a terminal much |
| `parts/partslist.csv` | The [parts list]({{< relref "docs/parts" >}}), rendered onto this site |
| `docs/` | This documentation site (Hugo) |
| `tools/screenshot.py` | Captures the screen over serial |
| `rfid_mysql_schema.sql` | Sample table for RFID lookups |

## Configure

WiFi, the server address, the protocol and the default loco can all be set on the device after
flashing (see [Using the throttle]({{< relref "docs/using#settings-menu" >}})). A unit with
nothing saved opens the settings menu on first boot.

To bake in defaults instead, edit `DialThrottle/config.h` before building:

- `WIFI_SSID`, `WIFI_PASS`
- `WITHROTTLE_HOST`, `WITHROTTLE_PORT`
- `LOCO_ADDRESS`, `LOCO_IS_LONG_ADDRESS`
- `THROTTLE_PROTOCOL_DEFAULT`, see [Protocols]({{< relref "docs/protocols" >}})

Settings saved on the device take priority over these values, and survive a reflash.

## Build and flash with make

You need [arduino-cli](https://arduino.github.io/arduino-cli/) and `make`. The board, the ESP32
core and every library are pinned in `DialThrottle/sketch.yaml`, so the first build downloads
what it needs. The ESP32 core alone is several hundred MB.

```bash
make build                     # compile
make flash                     # compile and upload to /dev/ttyACM0
make monitor                   # serial console at 115200 baud
make help                      # list every target
```

With more than one board attached, pass the port:

```bash
make flash PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00
```

`DEFINES` adds preprocessor flags without editing `config.h`. Changing it forces a rebuild:

```bash
make flash DEFINES=-DTHROTTLE_PROTOCOL_DEFAULT=PROTOCOL_LCC_GRIDCONNECT
```

## Build with the Arduino IDE

1. Install the ESP32 boards package. Add
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   under **File > Preferences > Additional boards manager URLs**, then install **esp32** by
   Espressif, version 3.1.3, from the Boards Manager.
2. Install these libraries from the Library Manager: **M5Dial**, **M5Unified**, **M5GFX**,
   **SparkFun Qwiic Keypad Arduino Library** and **MySQL Connector Arduino**. Running
   `make deps` does both steps from a terminal, at the pinned versions.
3. Open `DialThrottle/DialThrottle.ino`.
4. Under **Tools**, choose board **M5Dial** and set:
   - USB CDC On Boot: **Enabled**
   - USB Mode: **Hardware CDC and JTAG**
   - Flash Size: **8MB (64Mb)**
   - Partition Scheme: **8M with spiffs (3MB APP/1.5MB SPIFFS)**
5. Select the port and click **Upload**.

> USB CDC On Boot must be enabled, or the serial console goes to pins that the Dial doesn't
> expose.

## Guided installer

For less technical users, the installer downloads arduino-cli if needed, offers to open
`config.h`, lets you pick the serial port, then builds and flashes.

| Platform | Command |
| --- | --- |
| Linux, macOS | `./install.sh` |
| Windows PowerShell | `.\install.ps1` |
| Windows Command Prompt | `install.bat` |

All three need Python 3. For unattended installs at clubs or events:

```bash
./install.sh --yes --port /dev/ttyACM0 --no-monitor
```

| Flag | Effect |
| --- | --- |
| `--yes`, `-y` | Accept defaults, never prompt |
| `--port <path>` | Use this port without asking |
| `--monitor`, `--no-monitor` | Open, or skip, the serial monitor after flashing |
| `--edit-config`, `--no-edit-config` | Open, or skip, the config editor |
| `--help` | Show all options |

## Serial permissions on Linux

If the board doesn't show up or the upload is refused, add yourself to the `dialout` group, then
log out and back in:

```bash
sudo usermod -aG dialout "$USER"
```
