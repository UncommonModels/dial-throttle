---
title: "RFID"
description: "Selecting a locomotive by tag, with an optional MySQL lookup."
weight: 6
---

Hold a tagged car or loco to the Dial's built-in RFID reader to select it. The reader can be
turned off under **System** in the settings menu.

## Lookup order

A tag UID is resolved to a loco address in this order:

1. A MySQL lookup on the same host as the WiThrottle server, if enabled.
2. The local `RFID_LOCO_MAP` table in `config.h`.
3. The last 4 hex digits of the tag UID, normalized to an address in `1..9999`.

The status line shows which one matched: `RFID-DB`, `RFID-MAP` or `RFID-TAIL`.

## MySQL lookup

Set these in `config.h`:

| Setting | Purpose |
| --- | --- |
| `ENABLE_RFID_MYSQL_LOOKUP` | Turn the lookup on |
| `RFID_MYSQL_HOST` | Server IP address, as a numeric string |
| `RFID_MYSQL_PORT` | Server port |
| `RFID_MYSQL_USER`, `RFID_MYSQL_PASS` | Credentials |
| `RFID_MYSQL_DB`, `RFID_MYSQL_TABLE` | Database and table |
| `RFID_MYSQL_UID_COLUMN` | Column holding the tag UID |
| `RFID_MYSQL_LOCO_COLUMN` | Column holding the loco ID |
| `RFID_MYSQL_IS_LONG_COLUMN` | Optional long-address flag column |

> `RFID_MYSQL_HOST` must be an IP address. Host names aren't resolved.

A `loco_id` value can be prefixed (`S3`, `L128`) or numeric only (`3`, `128`). A numeric value
above 127 is treated as a long address.

The repository includes a sample schema with seed data:

```bash
mysql -h <server-ip> -u <user> -p < rfid_mysql_schema.sql
```

## Verification checklist

1. In `config.h`, set `ENABLE_RFID_MYSQL_LOOKUP = true`, set `RFID_MYSQL_HOST` to the server's IP
   address, and match the credentials and column names to your table.
2. Confirm a row exists for a test tag:
   ```sql
   SELECT loco_id, is_long FROM wifithrottle.rfid_loco_map WHERE rfid_uid='DEADBEEF' LIMIT 1;
   ```
3. Flash the firmware and open the serial monitor.
4. Scan the tag. The console shows source `(db)` and the status line shows `RFID-DB`.
5. Remove or rename that row and scan again. It falls back to `(map)` / `RFID-MAP`.
6. Remove the local map entry too and scan again. It falls back to `(tail)` / `RFID-TAIL`.
7. Confirm the address is in `1..9999` and that JMRI acquires it.
