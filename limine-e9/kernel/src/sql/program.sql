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

-- page allocator 
CREATE TABLE meta (id INT, cursor INT);
CREATE TABLE pages (frame INT, owner TEXT);
INSERT INTO meta (id, cursor) VALUES (0, 32);

-- claim frame-a
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

-- claim frame-b
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

-- free frame-a
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

-- claim frame-c
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

-- virtual memory, entirely in SQL: extend the live PML4 through phys,
-- activate it with cr3_write, then read and write through the mapping.
-- cr3 is the root Limine left running; slot 1 is guarded to be zero.
SELECT cr3 FROM boot_info;
SELECT value FROM phys WHERE address = (SELECT cr3 FROM boot_info) + 8;

-- claim four frames for pdpt / pd / pt / data
INSERT INTO pages (frame, owner)
  SELECT address * 8 + CASE ((~value) & (value + 1))
         WHEN 1 THEN 0 WHEN 2 THEN 1 WHEN 4 THEN 2 WHEN 8 THEN 3
         WHEN 16 THEN 4 WHEN 32 THEN 5 WHEN 64 THEN 6 ELSE 7 END, 'pdpt'
  FROM volatile_memory
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE volatile_memory SET value = value | ((~value) & (value + 1))
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE meta SET cursor = cursor + 1 WHERE id = 0
  AND (SELECT value FROM volatile_memory
       WHERE address = (SELECT cursor FROM meta WHERE id = 0)) = 255;

INSERT INTO pages (frame, owner)
  SELECT address * 8 + CASE ((~value) & (value + 1))
         WHEN 1 THEN 0 WHEN 2 THEN 1 WHEN 4 THEN 2 WHEN 8 THEN 3
         WHEN 16 THEN 4 WHEN 32 THEN 5 WHEN 64 THEN 6 ELSE 7 END, 'pd'
  FROM volatile_memory
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE volatile_memory SET value = value | ((~value) & (value + 1))
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE meta SET cursor = cursor + 1 WHERE id = 0
  AND (SELECT value FROM volatile_memory
       WHERE address = (SELECT cursor FROM meta WHERE id = 0)) = 255;

INSERT INTO pages (frame, owner)
  SELECT address * 8 + CASE ((~value) & (value + 1))
         WHEN 1 THEN 0 WHEN 2 THEN 1 WHEN 4 THEN 2 WHEN 8 THEN 3
         WHEN 16 THEN 4 WHEN 32 THEN 5 WHEN 64 THEN 6 ELSE 7 END, 'pt'
  FROM volatile_memory
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE volatile_memory SET value = value | ((~value) & (value + 1))
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE meta SET cursor = cursor + 1 WHERE id = 0
  AND (SELECT value FROM volatile_memory
       WHERE address = (SELECT cursor FROM meta WHERE id = 0)) = 255;

INSERT INTO pages (frame, owner)
  SELECT address * 8 + CASE ((~value) & (value + 1))
         WHEN 1 THEN 0 WHEN 2 THEN 1 WHEN 4 THEN 2 WHEN 8 THEN 3
         WHEN 16 THEN 4 WHEN 32 THEN 5 WHEN 64 THEN 6 ELSE 7 END, 'data'
  FROM volatile_memory
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE volatile_memory SET value = value | ((~value) & (value + 1))
  WHERE address = (SELECT cursor FROM meta WHERE id = 0)
    AND value < 255 AND address <= 8191;
UPDATE meta SET cursor = cursor + 1 WHERE id = 0
  AND (SELECT value FROM volatile_memory
       WHERE address = (SELECT cursor FROM meta WHERE id = 0)) = 255;

-- link root[1] -> pdpt -> pd[3] -> pt[4] -> data (present|writable),
-- the first write guarded by the live slot still being zero
INSERT INTO phys (address, value)
  SELECT (SELECT cr3 FROM boot_info) + 8, frame * 4096 + 3
  FROM pages WHERE owner = 'pdpt'
    AND (SELECT value FROM phys
         WHERE address = (SELECT cr3 FROM boot_info) + 8) = 0;
INSERT INTO phys (address, value)
  SELECT (SELECT frame FROM pages WHERE owner = 'pdpt') * 4096 + 16,
         frame * 4096 + 3
  FROM pages WHERE owner = 'pd';
INSERT INTO phys (address, value)
  SELECT (SELECT frame FROM pages WHERE owner = 'pd') * 4096 + 24,
         frame * 4096 + 3
  FROM pages WHERE owner = 'pt';
INSERT INTO phys (address, value)
  SELECT (SELECT frame FROM pages WHERE owner = 'pt') * 4096 + 32,
         frame * 4096 + 3
  FROM pages WHERE owner = 'data';

-- activate (reload CR3, flush the TLB) and zero the data frame's first
-- word so the read-back below is exact
INSERT INTO cr3_write (value) SELECT cr3 FROM boot_info;
UPDATE phys SET value = 0
  WHERE address = (SELECT frame FROM pages WHERE owner = 'data') * 4096;

-- write 77 through virtual address 1*2^39 + 2*2^30 + 3*2^21 + 4*2^12 + 5,
-- read it back through the mapping and through the frame's word
INSERT INTO virt_memory (address, value) VALUES (551909605381, 77);
SELECT value FROM virt_memory WHERE address = 551909605381;
SELECT value FROM phys
  WHERE address = (SELECT frame FROM pages WHERE owner = 'data') * 4096;

SELECT frame, owner FROM pages;
SELECT value FROM volatile_memory WHERE address = 32;

INSERT INTO io_8_write (port, value) VALUES (233, 69);
-- E 