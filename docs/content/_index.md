---
title: "Dial Throttle"
---
| Idle, no loco | Half-step between notches | Ramping to notch 5 | Full speed |
| --- | --- | --- | --- |
| {{< shot idle.png "idle" >}} | {{< shot half-step.png "half step" >}} | {{< shot ramping.png "ramping" >}} | {{< shot full.png "full" >}} |

| Reverse | E-Stop latched | Battery gauge | Settings menu |
| --- | --- | --- | --- |
| {{< shot reverse.png "reverse" >}} | {{< shot estop.png "estop" >}} | {{< shot battery.png "battery" >}} | {{< shot settings.png "settings" >}} |

## What it does

- Connects to your WiFi network and to a JMRI WiThrottle server (default port `12090`), or to an
  LCC GridConnect server as a real OpenLCB node.
- Uses the dial as a notched throttle with momentum. You choose 4, 8, 12 or 16 notches, and speed
  runs over steps `0..126`.
- Caps reverse at half the notches by default, so the dial only reaches half speed backwards.
- Brakes while you hold the touchscreen. A press of the dial button stops the loco, or reverses it
  when already stopped.
- Adds function, turnout and loco-select keys through a SparkFun Qwiic keypad.
- Selects a loco by RFID tag, looked up in a MySQL table, a local map, or the tag UID itself.
- Saves power on battery by dimming, turning the display off, suspending WiFi and finally
  powering off, but never while the train is moving.
- Keeps WiFi, server and throttle settings in flash, editable on the device.

## Where to go next

| Page | Covers |
| --- | --- |
| [Getting started]({{< relref "docs/getting-started" >}}) | Hardware, wiring, building and flashing |
| [Parts list]({{< relref "docs/parts" >}}) | What to order, with Digi-Key numbers |
| [Using the throttle]({{< relref "docs/using" >}}) | Dial controls, keypad, settings menu |
| [Protocols]({{< relref "docs/protocols" >}}) | WiThrottle and LCC details |
| [Power and battery]({{< relref "docs/power" >}}) | Idle tiers, battery gauge and charge detection |
| [RFID]({{< relref "docs/rfid" >}}) | Tag lookup and the MySQL table |
| [Serial console]({{< relref "docs/serial" >}}) | Debug commands and screenshots |
| [Troubleshooting]({{< relref "docs/troubleshooting" >}}) | JMRI setup and common problems |
