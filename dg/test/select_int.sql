-- Verify the integer operations, which ty does (ty/DESIGN.md section 6),
-- at the edges of int2, int4 and int8, and what a client reads from them.
-- 1. Add and subtract up to the largest and smallest value of each width.
-- 2. Divide: the quotient cuts toward zero, the remainder has the sign of
--    the dividend, and x % -1 is 0, even for the smallest value.
-- 3. Negate and take the absolute value next to the smallest value.
-- 4. gcd and lcm are never negative; a zero makes an lcm 0.
-- 5. Bit operations, and shifts up to the width less one.
-- 6. Mixed widths give the wider type, and a cast narrows with a check.
-- 7. A float8 and a float4 round to the nearest int8, a tie to the even.
-- 8. max and min of each width, and sorted keys at the edges.
-- (Errors are server.sh's: a run stops at its first.)
SELECT 32767::int2 - 1::int2 AS a, (-32767)::int2 - 1::int2 AS b, 2147483646::int4 + 1 AS c, 2147483647::int4 - 1 AS d, 9223372036854775806::int8 + 1::int8 AS e, (-9223372036854775807)::int8 - 1::int8 AS f;
SELECT -7 / 2 AS a, -7 % 2 AS b, 7 / -2 AS c, 7 % -2 AS d, (-2147483648)::int4 % (-1)::int4 AS e, (-9223372036854775808)::int8 % (-1)::int8 AS f, (-32768)::int2 % (-1)::int2 AS g, 32767::int2 / 2::int2 AS h, 9223372036854775807::int8 / 10::int8 AS i;
SELECT -(32767::int2) AS a, -(2147483647::int4) AS b, -(9223372036854775807::int8) AS c, abs((-32767)::int2) AS d, abs((-2147483647)::int4) AS e, abs((-9223372036854775807)::int8) AS f, @ (-5) AS g;
SELECT gcd(12, -18) AS a, gcd(0, 0) AS b, gcd((-2147483648)::int4, (-1)::int4) AS c, gcd(9223372036854775807::int8, 3::int8) AS d, lcm(4, 6) AS e, lcm(0, 5) AS f, lcm((-4)::int8, 6::int8) AS g, lcm(0::int4, (-2147483648)::int4) AS h;
SELECT 12 & 10 AS a, 12 | 10 AS b, 12 # 10 AS c, ~12 AS d, 1 << 30 AS e, (-8) >> 1 AS f, 1::int8 << 62 AS g, 1::int2 << 15 AS h, (-32768)::int2 >> 15 AS i, ~(0::int2) AS j, 255::int8 & 15::int8 AS k;
SELECT 5::int2 + 7::int4 AS a, pg_typeof(5::int2 + 7::int4) AS b, pg_typeof(5::int4 + 7::int8) AS c, 2147483647::int4 + 1::int8 AS d, 5::int8 / 2::int2 AS e, 3::int2 * 4::int8 AS f, pg_typeof(3::int2 * 4::int2) AS g;
SELECT 32767::int4::int2 AS a, (-32768)::int8::int2 AS b, 2147483647::int8::int4 AS c, (-2147483648)::int8::int4 AS d, 5::int2::int8 AS e, 5::int4::int8 AS f;
SELECT 2.5::float8::int8 AS a, 3.5::float8::int8 AS b, (-2.5::float8)::int8 AS c, 2.5::float4::int8 AS d, 0.4::float8::int8 AS e, (-9223372036854775808::float8)::int8 AS f, 9223372036854775807::int8::float8 AS g;
SELECT max(x), min(x) FROM (VALUES (3::int2), (-5), (9)) v(x);
SELECT max(x), min(x) FROM (VALUES (3::int4), (-2147483648), (2147483647)) v(x);
SELECT max(x), min(x) FROM (VALUES (3::int8), (-9223372036854775807), (9223372036854775807)) v(x);
SELECT x FROM (VALUES (2147483647::int4), (-2147483648), (0), (-1), (1)) v(x) ORDER BY x;
