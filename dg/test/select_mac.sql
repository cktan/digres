-- Verify macaddr and macaddr8, which ty does (ty/DESIGN.md section 12), and
-- what a client reads from them.
-- 1. macaddr is read in each of its forms and written in one.
-- 2. macaddr8 is read with six or eight pairs, and any spacer.
-- 3. The operators: order, NOT, AND, OR, trunc, set7bit.
-- 4. The casts between macaddr and macaddr8.
-- 5. Sort order. (A column of these types is not supported by digres's
--    storage yet, so there is no key.)
-- 6. The hash: equal addresses alike, the standard low half of the extended.
-- (Errors are server.sh's: a run stops at its first.)
SELECT '08:00:2b:01:02:03'::macaddr AS a, '08-00-2b-01-02-03'::macaddr AS b, '08002b:010203'::macaddr AS c, '08002b-010203'::macaddr AS d, '0800.2b01.0203'::macaddr AS e, '0800-2b01-0203'::macaddr AS f, '08002b010203'::macaddr AS g, '  08:00:2B:01:02:03  '::macaddr AS h, '8:0:2b:1:2:3'::macaddr AS i, 'FF:FF:FF:FF:FF:FF'::macaddr AS j;
SELECT '08:00:2b:01:02:03:04:05'::macaddr8 AS a, '08-00-2b-01-02-03-04-05'::macaddr8 AS b, '0800.2b01.0203.0405'::macaddr8 AS c, '08002b0102030405'::macaddr8 AS d, '08:00:2b:01:02:03'::macaddr8 AS e, '08002b010203'::macaddr8 AS f, ' 08.00.2b.01.02.03 '::macaddr8 AS g, '0800:2b01:0203:0405'::macaddr8 AS h;
SELECT '08:00:2b:01:02:03'::macaddr < '08:00:2b:01:02:04'::macaddr AS lt, '08:00:2b:01:02:03'::macaddr = '08002b010203'::macaddr AS eq, macaddr_cmp('08:00:2b:ff:00:00', '08:00:2b:01:00:00') AS cmp, ~'08:00:2b:01:02:03'::macaddr AS not_, '08:00:2b:01:02:03'::macaddr & 'ff:ff:ff:00:00:00'::macaddr AS and_, '08:00:2b:01:02:03'::macaddr | 'ff:ff:ff:00:00:00'::macaddr AS or_, trunc('08:00:2b:01:02:03'::macaddr) AS trunc_;
SELECT '08:00:2b:01:02:03:04:05'::macaddr8 < '08:00:2b:01:02:03:04:06'::macaddr8 AS lt, macaddr8_cmp('09:00:00:00:00:00:00:00', '08:ff:ff:ff:ff:ff:ff:ff') AS cmp, ~'08:00:2b:01:02:03:04:05'::macaddr8 AS not_, '08:00:2b:01:02:03:04:05'::macaddr8 & 'ff:ff:ff:00:00:00:00:00'::macaddr8 AS and_, '08:00:2b:01:02:03:04:05'::macaddr8 | 'ff:ff:ff:00:00:00:00:00'::macaddr8 AS or_, trunc('08:00:2b:01:02:03:04:05'::macaddr8) AS trunc_, macaddr8_set7bit('08:00:2b:01:02:03:04:05') AS set7, macaddr8_set7bit('0a:00:2b:01:02:03:04:05') AS set7b;
SELECT '08:00:2b:01:02:03'::macaddr::macaddr8 AS wide, '08:00:2b:ff:fe:01:02:03'::macaddr8::macaddr AS narrow, ('08:00:2b:01:02:03'::macaddr::macaddr8)::macaddr = '08:00:2b:01:02:03'::macaddr AS round;
SELECT m FROM (VALUES ('08:00:2b:01:02:04'::macaddr), ('00:00:00:00:00:00'), ('ff:ff:ff:ff:ff:ff'), ('08:00:2b:01:02:03')) v(m) ORDER BY m;
SELECT m FROM (VALUES ('08:00:2b:01:02:03:04:06'::macaddr8), ('00:00:00:00:00:00:00:00'), ('ff:ff:ff:ff:ff:ff:ff:ff'), ('08:00:2b:01:02:03:04:05')) v(m) ORDER BY m;
SELECT hashmacaddr('08:00:2b:01:02:03') = hashmacaddr('08002b010203') AS eq, hashmacaddr('08:00:2b:01:02:03') <> hashmacaddr('08:00:2b:01:02:04') AS differs, (hashmacaddrextended('08:00:2b:01:02:03', 0) & 4294967295) = (hashmacaddr('08:00:2b:01:02:03')::int8 & 4294967295) AS low32, hashmacaddr8('08:00:2b:01:02:03:04:05') = hashmacaddr8('08002b0102030405') AS eq8, (hashmacaddr8extended('08:00:2b:01:02:03:04:05', 0) & 4294967295) = (hashmacaddr8('08:00:2b:01:02:03:04:05')::int8 & 4294967295) AS low32_8;
