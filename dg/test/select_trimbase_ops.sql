-- Verify to_bin, to_oct and to_hex, bytea's btrim, ltrim and rtrim, and
-- the order of date, time and timestamp, which ty gives (ty_int.h,
-- ty_bytea.h, ty_datetime.h): the extremes of each base, a set of one
-- byte, a few and more than 16, and the infinities of a date and a
-- timestamp in the order and in min and max.
SELECT to_bin(0) AS b0, to_oct(-1) AS o, to_hex(2147483647) AS h4, to_hex('-9223372036854775808'::int8) AS h8, to_bin('-1'::int8) AS b8;
SELECT btrim('\x0000010200'::bytea, '\x00'::bytea) AS b1, ltrim('\x0000010200'::bytea, '\x00'::bytea) AS l1, rtrim('\x0000010200'::bytea, '\x00'::bytea) AS r1, btrim('\x0a0b0c0b0a'::bytea, '\x0a0b'::bytea) AS b2, btrim('\x00'::bytea, '\x00'::bytea) AS empty, btrim(''::bytea, '\x00'::bytea) AS e2, btrim('\x0102'::bytea, ''::bytea) AS noset;
SELECT btrim('\x00112233445566778899aabbccddeeff10ff00'::bytea, '\x000102030405060708090a0b0c0d0e0f10ff'::bytea) AS long_set;
SELECT d, d < date '2024-01-01' AS lt, d = date 'infinity' AS inf FROM (VALUES (date '-infinity'), (date '2024-01-01'), (date 'infinity')) v(d) ORDER BY d DESC;
SELECT max(d), min(d), max(t), min(t), max(ts), min(ts) FROM (VALUES (date 'infinity', time '00:00', timestamp '-infinity'), (date '2024-01-01', time '23:59:59.999999', timestamp '2024-01-01'), (date '-infinity', time '12:00', timestamp 'infinity')) v(d, t, ts);
SELECT time '10:00' > time '09:59:59.999999' AS gt, date_cmp(date '2024-01-02', date '2024-01-01') AS dc, time_cmp(time '01:00', time '02:00') AS tc, timestamp_cmp(timestamp 'infinity', timestamp '2024-01-01') AS tsc, greatest(date '2024-01-01', date '2023-01-01') AS g, least(time '05:00', time '04:00') AS l;
