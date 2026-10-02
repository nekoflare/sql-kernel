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

INSERT INTO io_8_write (port, value) VALUES (233, 69);
-- E 