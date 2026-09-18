---
title: "Protocols"
description: "WiThrottle for JMRI, or LCC/OpenLCB over GridConnect."
weight: 4
---

The throttle speaks one of two protocols. Choose it on the device under **Network**, or set the
default for an unconfigured unit with `THROTTLE_PROTOCOL_DEFAULT` in `config.h`.

## WiThrottle

WiThrottle is the default. The implementation was checked against the JMRI WiThrottle protocol
specification and JMRI's own server code.

### Commands sent

| Command | Purpose |
| --- | --- |
| `N` | Client name |
| `HU` | Unique client ID |
| `*+`, then `*` every 2 seconds | Heartbeat |
| `M0+` | Acquire the loco |
| `M0A...<;>V` | Speed |
| `M0A...<;>R` | Direction |
| `M0A...<;>f` | Set a function's state |
| `M0A...<;>X` | Emergency stop |

Functions use the force-function form, which sets a function's state directly instead of
emulating a key press. It needs WiThrottle protocol 2.0 or later.

### Messages handled

The receive path handles the version line, alerts and info messages, acquire confirmations,
steal prompts, releases, and property notifications for speed, direction and function state.
Reports for other locomotives on the throttle are ignored.

A negative speed from the server means the command station has the address in emergency stop.
That latches the throttle's own E-Stop, so momentum cannot ramp the train back up.

### Acquiring

Acquiring is confirmed, not assumed. If the server doesn't answer, the request is retried up to
`ACQUIRE_MAX_ATTEMPTS` times. If another throttle holds the loco, the server asks whether to
steal it. `WITHROTTLE_AUTO_STEAL` decides whether that happens automatically or waits for the
`steal` serial command.

Once the loco is confirmed, the throttle pushes its current direction, speed and function states
to the server. That is what restores the train after a reconnect.

## LCC / OpenLCB over GridConnect

Choose **LCC** under **Network** in the settings menu, or build with:

```bash
make flash DEFINES=-DTHROTTLE_PROTOCOL_DEFAULT=PROTOCOL_LCC_GRIDCONNECT
```

The throttle opens a GridConnect TCP connection to the configured server, either JMRI's LCC
GridConnect server or a CAN gateway, and joins the network as a real node:

- Allocates a CAN alias with the standard CID, RID and AMD sequence, and reclaims on a conflict.
- Announces itself with Initialization Complete, and answers Verify Node ID and Alias Map
  Enquiry.
- Finds the locomotive by its well-known DCC train node ID, `06.01.00.00.xx.xx`, where long
  addresses set the top two bits. Short address 3 is `06.01.00.00.00.03`; long address 341 is
  `06.01.00.00.C1.55`.
- Assigns itself as that train's controller, then sends speed, functions and emergency stop with
  the traction protocol. Speed is an IEEE half-precision value in metres per second whose sign
  carries direction, so reverse at a standstill is negative zero.

### Node ID

Set `LCC_NODE_ID` for this throttle. The default keeps the `05.01.01` prefix, which openlcb.org
set aside for non-commercial use, and fills the low three bytes from the board's MAC address, so
each unit is unique.

### Check against your layout

- **Speed scale.** `LCC_FULL_SPEED_MPH` maps the top notch onto a prototype speed. The default of
  126 makes one speed step one mph, which matches the usual OpenMRN command station mapping.
- **Turnouts.** LCC has no address-based turnout command, so turnout `n` produces event
  `LCC_TURNOUT_EVENT_BASE + 2n` or `+ 2n + 1`. That is a convention, not a standard.

## Testing without a layout

Build with serial-only output to inspect either protocol:

```bash
make flash DEFINES=-DSERIAL_OUTPUT_ONLY_DEFAULT=1
```

Every frame the throttle would send is printed to the console instead. The serial commands
`wtin <line>` and `lccin <frame>` feed in messages as if a server had sent them, and `lcc` prints
the node ID, alias and controller state.
