INSERT INTO memory (address, value) VALUES
(0, 87), (1, 69), (2, 76), (3, 67), (4, 48), (5, 77), (6, 69), (7, 32), (8, 84), (9, 79), (10, 32), (11, 83), (12, 81), (13, 76), (14, 45), (15, 79), (16, 83), (17, 32), (18, 86), (19, 73), (20, 65), (21, 32), (22, 80), (23, 79), (24, 82), (25, 84), (26, 32), (27, 69), (28, 57), (29, 10);
-- Look, at address 4 you've got 0 instead of O

UPDATE memory SET value = 79 WHERE address = 4;
-- UPDATE above just shows u can mutate the memory :p

INSERT INTO io_8_write (port, value)
  SELECT 233, value FROM memory WHERE address BETWEEN 0 AND 29;
-- Write to IO port E9

SELECT address, value FROM memory WHERE address BETWEEN 4 AND 7;
-- just printing some values

WITH RECURSIVE fib(n, a, b) AS (
  SELECT 0, 0, 1
  UNION ALL
  SELECT n + 1, b, a + b FROM fib WHERE n < 10
)
SELECT a FROM fib;
-- fibonacci

-- what the bootloader told us about this machine
SELECT * FROM boot_info;

-- the memory map Limine handed us
SELECT base, length, type FROM memory_map;

-- a page allocator in pure SQL: there is no allocator C++. The bitmap
-- lives in volatile_memory (bit set = claimed; the image starts zeroed),
-- the scan hint in the one-row meta table, ownership in pages.
-- Pool: bytes 32..8191 = frames 256..65535 (1 MiB..256 MiB).
CREATE TABLE meta (id INT, cursor INT);
CREATE TABLE pages (frame INT, owner TEXT);
INSERT INTO meta (id, cursor) VALUES (0, 32);

-- claim frame-a: take the hinted byte's lowest clear bit (1,2,4..128 is
-- the mask in the bitmap, 0..7 the index in the frame number), record it,
-- and move the cursor on once the byte is full
INSERT INTO pages (frame, owner)
  SELECT address * 8 + CASE ((~value) & (value + 1))
         WHEN 1 THEN 0 WHEN 2 THEN 1 WHEN 4 THEN 2 WHEN 8 THEN 3
         WHEN 16 THEN 4 WHEN 32 THEN 5 WHEN 64 THEN 6 ELSE 7 END, 'frame-a'
  FROM volatile_memory
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE volatile_memory SET value = value | ((~value) & (value + 1))
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE meta SET cursor = cursor + 1 WHERE id = 0
  AND (SELECT value FROM volatile_memory
       WHERE address = (SELECT cursor FROM meta WHERE id = 0)) = 255;

-- claim frame-b: same three statements, next bit of the same byte
INSERT INTO pages (frame, owner)
  SELECT address * 8 + CASE ((~value) & (value + 1))
         WHEN 1 THEN 0 WHEN 2 THEN 1 WHEN 4 THEN 2 WHEN 8 THEN 3
         WHEN 16 THEN 4 WHEN 32 THEN 5 WHEN 64 THEN 6 ELSE 7 END, 'frame-b'
  FROM volatile_memory
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE volatile_memory SET value = value | ((~value) & (value + 1))
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE meta SET cursor = cursor + 1 WHERE id = 0
  AND (SELECT value FROM volatile_memory
       WHERE address = (SELECT cursor FROM meta WHERE id = 0)) = 255;

SELECT frame, owner FROM pages;
SELECT value FROM volatile_memory WHERE address = 32;

-- free frame-a: clear its bit and rewind the cursor to the freed byte
UPDATE volatile_memory
  SET value = value & (255 - (1 << ((SELECT frame FROM pages
       WHERE owner = 'frame-a') % 8)))
  WHERE address = (SELECT frame FROM pages WHERE owner = 'frame-a') / 8;
UPDATE meta SET cursor = CASE
    WHEN (SELECT frame FROM pages WHERE owner = 'frame-a') / 8 < cursor
    THEN (SELECT frame FROM pages WHERE owner = 'frame-a') / 8
    ELSE cursor END
  WHERE id = 0;
DELETE FROM pages WHERE owner = 'frame-a';

-- claim frame-c: the freed frame comes back
INSERT INTO pages (frame, owner)
  SELECT address * 8 + CASE ((~value) & (value + 1))
         WHEN 1 THEN 0 WHEN 2 THEN 1 WHEN 4 THEN 2 WHEN 8 THEN 3
         WHEN 16 THEN 4 WHEN 32 THEN 5 WHEN 64 THEN 6 ELSE 7 END, 'frame-c'
  FROM volatile_memory
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE volatile_memory SET value = value | ((~value) & (value + 1))
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE meta SET cursor = cursor + 1 WHERE id = 0
  AND (SELECT value FROM volatile_memory
       WHERE address = (SELECT cursor FROM meta WHERE id = 0)) = 255;

SELECT frame, owner FROM pages;
SELECT value FROM volatile_memory WHERE address = 32;

INSERT INTO io_8_write (port, value) VALUES (233, 69);
-- E 