-- Verify the bytea operations, which ty does (ty/DESIGN.md section 9), and
-- what a client reads from them.
-- 1. Read the hex form, with spaces between pairs, and the escaped form;
--    write both, as bytea_output says.
-- 2. Concatenate, take the length, reverse.
-- 3. A substring: from a position, for a length, a start below 1, a length
--    that runs past the end, and a length of 0.
-- 4. An overlay, with and without FOR.
-- 5. position, bit_count, get_byte, get_bit, set_byte and set_bit.
-- 6. Compare, sort, and max and min.
-- 7. bytea to an integer and back.
-- 8. encode and decode, in hex, base64 and escape: the padding, the line
--    feeds of base64, space skipped on the way in, and a round trip.
-- 9. A bytea is a key: a primary key, and a lookup by it.
-- (Temporary tables, as the 8 sessions run.sh starts share one database.
-- Errors are server.sh's: a run stops at its first.)
SELECT '\x6162'::bytea AS a, '\x61 62
63'::bytea AS b, '\x'::bytea AS c, 'abc'::bytea AS d, '\141\142c'::bytea AS e, '\\'::bytea AS f, '\000\377'::bytea AS g, '\xAbCd'::bytea AS h, ''::bytea AS i;
SET bytea_output = 'escape';
SELECT '\x00610a5c7f80ff'::bytea AS a, 'a b'::bytea AS b, ''::bytea AS c;
RESET bytea_output;
SELECT '\x00610a5c7f80ff'::bytea AS a;
SELECT '\x6162'::bytea || '\x63'::bytea AS a, '\x'::bytea || '\x63'::bytea AS b, octet_length('\x616263'::bytea) AS c, length('\x'::bytea) AS d, reverse('\x010203'::bytea) AS e, reverse('\x'::bytea) AS f;
SELECT substring('\x0102030405'::bytea FROM 2) AS a, substring('\x0102030405'::bytea FROM 2 FOR 2) AS b, substring('\x0102030405'::bytea FROM -1 FOR 4) AS c, substring('\x0102030405'::bytea FROM 4 FOR 100) AS d, substring('\x0102030405'::bytea FROM 2 FOR 0) AS e, substring('\x0102030405'::bytea FROM 9) AS f, substring('\x0102030405'::bytea FROM 0 FOR 1) AS g, substring('\x0102030405'::bytea FROM 1 FOR 2147483647) AS h;
SELECT overlay('\x0102030405'::bytea PLACING '\xaabb'::bytea FROM 2 FOR 2) AS a, overlay('\x0102030405'::bytea PLACING '\xaabb'::bytea FROM 2) AS b, overlay('\x0102030405'::bytea PLACING '\xaa'::bytea FROM 5 FOR 10) AS c, overlay('\x0102030405'::bytea PLACING '\xaa'::bytea FROM 9) AS d, overlay('\x0102'::bytea PLACING '\xaa'::bytea FROM 1 FOR 0) AS e, overlay('\x0102'::bytea PLACING '\xaa'::bytea FROM 2 FOR -5) AS f;
SELECT position('\x0304'::bytea IN '\x01020304'::bytea) AS a, position('\x09'::bytea IN '\x01020304'::bytea) AS b, position('\x'::bytea IN '\x0102'::bytea) AS c, bit_count('\xff0f'::bytea) AS d, bit_count('\x'::bytea) AS e, get_byte('\x0102ff'::bytea, 2) AS f, get_bit('\x0102ff'::bytea, 0) AS g, get_bit('\x0102ff'::bytea, 9) AS h, set_byte('\x010203'::bytea, 1, 255) AS i, set_byte('\x010203'::bytea, 0, 256) AS j, set_bit('\x00'::bytea, 7, 1) AS k, set_bit('\xff'::bytea, 0, 0) AS l;
SELECT '\x01'::bytea < '\x0100'::bytea AS a, '\x0100'::bytea < '\x02'::bytea AS b, '\x'::bytea < '\x00'::bytea AS c, '\x01'::bytea = '\x01'::bytea AS d, byteacmp('\x02'::bytea, '\x0100'::bytea) > 0 AS e, byteacmp('\x01'::bytea, '\x0100'::bytea) < 0 AS f, byteacmp('\x01'::bytea, '\x01'::bytea) = 0 AS g;
SELECT b FROM (VALUES ('\x02'::bytea), ('\x0100'), ('\x'), ('\x01'), ('\xff'), ('\x00')) v(b) ORDER BY b;
SELECT max(b), min(b) FROM (VALUES ('\x02'::bytea), ('\x0100'), ('\x'), ('\x01'), ('\xff')) v(b);
SELECT '\x0102'::bytea::int2 AS a, '\xffff'::bytea::int2 AS b, '\xffff'::bytea::int4 AS c, '\x'::bytea::int4 AS d, '\x0000000000000100'::bytea::int8 AS e, '\xffffffffffffffff'::bytea::int8 AS f, 258::int2::bytea AS g, 258::int4::bytea AS h, (-2)::int8::bytea AS i, (-2)::int2::bytea AS j;
SELECT encode('\x00ff5c61'::bytea, 'hex') AS a, encode('\x00ff5c61'::bytea, 'escape') AS b, encode('\x00ff5c61'::bytea, 'base64') AS c, encode('\x'::bytea, 'base64') AS d, encode('\xff'::bytea, 'base64') AS e, encode('\xffff'::bytea, 'base64') AS f, encode('\xffffff'::bytea, 'base64') AS g, encode('\x61'::bytea, 'HEX') AS h;
SELECT encode(decode(repeat('a', 57), 'escape'), 'base64') AS a, encode(decode(repeat('a', 58), 'escape'), 'base64') AS b, length(encode(decode(repeat('a', 171), 'escape'), 'base64')) AS c;
SELECT decode('AP9cYQ==', 'base64') AS a, decode('AP9c YQ==
', 'base64') AS b, decode('00ff5c61', 'hex') AS c, decode(' 00 ff5c
61 ', 'hex') AS d, decode('a\\b\141\377', 'escape') AS e, decode('', 'base64') AS f;
SELECT decode(encode(b, 'base64'), 'base64') = b AS base64, decode(encode(b, 'hex'), 'hex') = b AS hex, decode(encode(b, 'escape'), 'escape') = b AS esc FROM (VALUES ('\x'::bytea), ('\x00'), ('\x5c'), ('\x00ff5c0a20'), (decode(repeat('abcd', 100), 'escape'))) v(b);
CREATE TEMP TABLE bytea_key (b bytea PRIMARY KEY, n int4);
INSERT INTO bytea_key VALUES ('\x01', 1), ('\x', 2), ('\xff00', 3), ('\x0100', 4);
SELECT n FROM bytea_key WHERE b = '\x0100';
SELECT n FROM bytea_key ORDER BY b;
SELECT n FROM bytea_key WHERE b > '\x01' ORDER BY b;
DROP TABLE bytea_key;
