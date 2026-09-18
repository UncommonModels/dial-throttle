---
title: "Parts list"
description: "Everything needed to build one throttle."
weight: 2
---

One throttle, as built. Every line is stocked at Digi-Key, and the quantities are per unit.

{{< parts >}}

## What each part is for

- **M5Dial** is the whole throttle: an ESP32-S3, a round 240×240 touchscreen, a rotary encoder
  with a push button, an RFID reader and a speaker in one enclosure.
- **Qwiic keypad and cable** add the function, turnout and loco-select keys. The throttle works
  without them, and picks one up whenever it is plugged in.
- **18650 cell, holder and Pico Blade cable** make it cordless. The Dial charges the cell over
  USB-C. The cable matches the Dial's 1.25 mm 2-pin battery connector, so check the polarity
  against the case before the first connection.
- **Panel-mount USB-C cable** brings the Dial's USB port out to the edge of the case, for
  charging and reflashing without opening it.

A printable case is in `case/` in the repository, as an STL with the FreeCAD source beside it. It is designed so all parts mount with  M2 screws without threaded inserts.

## Optional extras

None of these are in the list above; all are described elsewhere in these docs.

| Add | For | See |
| --- | --- | --- |
| Two 100 kΩ resistors | Battery voltage divider, which the gauge needs | [Battery gauge]({{< relref "docs/power#battery-gauge" >}}) |
| Two more 100 kΩ resistors | Sensing external power on the 5 V input rail | [Charging and power source]({{< relref "docs/power#charging-and-power-source" >}}) |
| One wire to the charger's `CHRG` pin | Showing charge state | [Charging and power source]({{< relref "docs/power#charging-and-power-source" >}}) |
| RFID tags, 13.56 MHz | Selecting a loco by tag | [RFID]({{< relref "docs/rfid" >}}) |
