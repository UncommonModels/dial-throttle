-- Sample schema for RFID -> locomotive lookup used by Dial Throttle.
-- Defaults match DialThrottle/config.h:
--   DB:    wifithrottle
--   Table: rfid_loco_map
--   UID:   rfid_uid
--   Loco:  loco_id
--   Long:  is_long (optional)

CREATE DATABASE IF NOT EXISTS wifithrottle;
USE wifithrottle;

CREATE TABLE IF NOT EXISTS rfid_loco_map (
  rfid_uid VARCHAR(32) NOT NULL,
  loco_id VARCHAR(8) NOT NULL,
  -- Optional override: 1=true(long), 0=false(short), NULL=derive from loco_id/address.
  is_long TINYINT(1) NULL DEFAULT NULL,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (rfid_uid)
);

-- Optional explicit index (PRIMARY KEY already covers lookups by uid).
CREATE INDEX idx_rfid_uid ON rfid_loco_map (rfid_uid);

-- Seed rows (UID uppercase hex with no separators).
INSERT INTO rfid_loco_map (rfid_uid, loco_id, is_long) VALUES
  ('DEADBEEF', 'S3', 0),
  ('04A1B2C3D4', '128', NULL),
  ('11223344', 'L4012', 1)
ON DUPLICATE KEY UPDATE
  loco_id = VALUES(loco_id),
  is_long = VALUES(is_long);

-- Quick manual test query:
-- SELECT loco_id, is_long FROM rfid_loco_map WHERE rfid_uid='DEADBEEF' LIMIT 1;
