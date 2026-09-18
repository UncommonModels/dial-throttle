---
title: "Troubleshooting"
description: "JMRI setup and the problems that come up most."
weight: 8
---

## JMRI setup

1. In JMRI, start the WiThrottle server.
2. Confirm the server port, `12090` by default.
3. Make sure the Dial and the JMRI computer are on the same network.
4. If a firewall is enabled, allow TCP port `12090`.

## Keypad not detected

- Check the Qwiic cable and the connector.
- Confirm the keypad address is `0x4B`, the SparkFun default.
- The status line shows `Qwiic keypad NOT found` at boot. A keypad plugged in later is detected
  within about 10 seconds.

## Can't connect to WiThrottle

- Check the server address and port under **Network** in the settings menu, or with `settings`
  on the serial console.
- Make sure the JMRI WiThrottle server is running.
- `status` on the serial console shows the WiFi and server state.

## Loco doesn't respond

- Check the loco address and whether it is a short or long address.
- If another throttle holds the loco, send `steal`, or set `WITHROTTLE_AUTO_STEAL`.
- If E-Stop is latched, press the dial button to clear it.

## Board doesn't appear, or the upload fails

- On Linux, add yourself to the `dialout` group, then log out and back in:
  `sudo usermod -aG dialout "$USER"`.
- A light-sleeping unit may not show up when plugged into a computer. Touch the screen or turn the
  dial first.
- With more than one ESP32 attached, `/dev/ttyACM0` may be the wrong board. Pass the stable name:
  `make flash PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00`.
- In the Arduino IDE, check that USB CDC On Boot is enabled. Otherwise the console is silent.

## Screen stays lit while powered off

Older firmware didn't hold the backlight pin low in deep sleep, so on a
wall charger the screen could stay lit after an idle power-off. Update to the current firmware.
