---
title: "Serial console"
description: "Debug commands and screen captures over USB."
weight: 7
---

The throttle takes commands over USB serial at 115200 baud. Type a command and press Enter.

```bash
make monitor                   # or: make monitor PORT=/dev/ttyACM1
```

## Throttle and loco

| Command | Action |
| --- | --- |
| `status` | Print WiFi, server and loco state |
| `acq` | Acquire the configured loco |
| `rel` | Release the loco |
| `loco <addr> <s/l>` | Set the active loco, short or long, and acquire it |
| `steal` | Take a loco another throttle holds |
| `speed <0-126>` | Set the speed step |
| `notch <n>` | Set the throttle notch, from 0 to the notch count |
| `dir <f/r/t>` | Forward, reverse or toggle |
| `fn <0-28> <on/off/t>` | Function on, off or toggle |
| `estop` | Emergency stop |
| `clear` | Clear the E-Stop latch (`estop clear` also works) |

## Connection

| Command | Action |
| --- | --- |
| `wifi` | Reconnect WiFi |
| `wt` | Reconnect to the server |
| `send <raw>` | Send a raw WiThrottle line, e.g. `send M0A*<;>qV` |
| `lcc` | LCC node ID, alias and controller state |

## Settings

| Command | Action |
| --- | --- |
| `settings` | Print the settings and open the settings menu |
| `set ssid <name>` | Set and save the WiFi name |
| `set pass <password>` | Set and save the WiFi password |
| `set ip <a.b.c.d>` | Set and save the server address |
| `set port <n>` | Set and save the server port |
| `set proto <wt/lcc>` | Choose WiThrottle or LCC GridConnect |
| `defaults` | Reset every setting to the `config.h` defaults |

## Power and diagnostics

| Command | Action |
| --- | --- |
| `diag` | Idle timer, last input source, encoder count, notch and pending half-step, redraw and loop timing. Doesn't reset the idle timer |
| `bat` | Battery reading, power source and charge state |
| `tier <0/1/2>` | Force a backlight tier. `tier 2` checks that the screen really goes dark |
| `off` | Power off now |
| `help` | List every command |

## Testing

| Command | Action |
| --- | --- |
| `enc <counts>` | Inject raw encoder counts, 2 per detent |
| `btn [long]` | Fake a dial button press, or a long press |
| `simbat <pct/-1>` | Fake a battery percentage; `-1` clears it |
| `wtin <line>` | Inject a line as if the WiThrottle server sent it |
| `lccin <frame>` | Inject a GridConnect frame |
| `shot` | Dump the screen as hex |

To see what the throttle would send without a layout, build with
`make flash DEFINES=-DSERIAL_OUTPUT_ONLY_DEFAULT=1`. Outgoing WiThrottle lines are printed as
`WT> ...` instead of being transmitted.

## Screenshots

`tools/screenshot.py` sends `shot`, decodes the frame and writes a PNG. It needs `pyserial` and
`Pillow`. Every screenshot on this site was captured this way.

```bash
make screenshot PORT=/dev/ttyACM0
python3 tools/screenshot.py --setup "notch 5" --settle 3 shot.png
```

`--setup` runs a serial command before the capture and can be repeated. `--settle` waits that many
seconds afterwards, which lets momentum ramp the speed up. Without Pillow the raw hex dump is saved
next to the output, and `--decode dump.hex out.png` converts it later.
