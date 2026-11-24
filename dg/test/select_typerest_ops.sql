-- Verify the operations that ty took last: date_bin of timestamptz,
-- in_range of floats, to_timestamp of a number, the parts of a timetz,
-- a zone written as a number, text's pattern operators, quote_literal,
-- the normal forms, unistr, and the scales of decimal's results.
\set ON_ERROR_STOP off
SET timezone = 'UTC';
SELECT date_bin('15 minutes', t, '2001-01-01 00:07:00+00') AS b FROM (VALUES ('2024-02-29 12:34:56+00'::timestamptz), ('1999-12-31 23:59:59+00'), ('infinity')) v(t);
SELECT date_bin('1 month', '2024-02-29 12:34:56+00'::timestamptz, '2001-01-01 00:00:00+00');
SELECT date_bin('0 days', '2024-02-29 12:34:56+00'::timestamptz, '2001-01-01 00:00:00+00');
SELECT v, b, in_range(v, b, 1.5::float8, true, false) AS pre8, in_range(v, b, 'inf'::float8, false, true) AS fol8, in_range(v::float4, b::float4, 1.5::float8, true, false) AS pre4 FROM (VALUES ('1'::float8, '2.5'::float8), ('0.5', '2.5'), ('inf', '-inf'), ('nan', 'nan'), ('nan', '1'), ('1', 'nan')) x(v, b);
SELECT in_range(1::float8, 1::float8, -1::float8, true, true);
SELECT in_range(1::float4, 1::float4, 'nan'::float8, true, true);
SELECT to_timestamp(0) AS a, to_timestamp(1700000000.123456) AS b, to_timestamp('-infinity') AS c;
SELECT to_timestamp('nan');
SELECT to_timestamp(1e300);
SELECT extract(epoch FROM t) AS e, extract(timezone FROM t) AS z, extract(timezone_h FROM t) AS h, date_part('timezone_m', t) AS m, extract(millisecond FROM t) AS ms FROM (VALUES ('12:34:56.789-05:30'::timetz), ('00:00+14')) v(t);
SELECT extract(day FROM '12:34:56-05'::timetz);
SELECT extract(fred FROM '12:34:56-05'::timetz);
SELECT make_timestamptz(2024, 1, 1, 0, 0, 0, '+05:30') AS a, make_timestamptz(2024, 1, 1, 0, 0, 0, '-0800') AS b;
SELECT make_timestamptz(2024, 1, 1, 0, 0, 0, '+99');
SELECT a, b, a ~<~ b AS lt, a ~<=~ b AS le, a ~>=~ b AS ge, a ~>~ b AS gt FROM (VALUES ('a', 'ab'), ('é', 'z'), ('x', 'x'), ('', 'a')) v(a, b);
SELECT quote_literal(x) AS q FROM (VALUES (''), ('it''s'), (E'a\\b'), (E'\\''')) v(x);
SELECT normalize(U&'e\0301', NFC) = U&'\00e9' AS c, normalize(U&'\00e9', NFD) = U&'e\0301' AS d, normalize(U&'\fb01', NFKC) AS kc, U&'e\0301' IS NFC NORMALIZED AS is_c, U&'e\0301' IS NFD NORMALIZED AS is_d, unicode_assigned(U&'a\0378') AS assigned;
SELECT pg_catalog.normalize('a', 'nfx');
SELECT unistr('a\\b') AS a, unistr('\0061\u0062\+000063\U00000064') AS b, unistr('\d83d\de00') AS c;
SELECT unistr('\d83d');
SELECT unistr('\x');
SELECT unistr('\0000');
SELECT 1::numeric(30,0) / 3 AS div, 1::numeric(5,0) / 3 AS div64, sqrt(2::numeric(30,5)) AS sqrt, ln(10::numeric(30,0)) AS ln, log(2::numeric(30,0), 8) AS log, exp(1::numeric(30,0)) AS exp;
SELECT 2::numeric(30,0) ^ 10 AS p, 2::numeric(30,0) ^ 0.5 AS r, 0::numeric(30,0) ^ 0 AS z, (-2)::numeric(30,0) ^ 3 AS n;
SELECT 0::numeric(30,0) ^ (-1);
SELECT (-2)::numeric(30,0) ^ 0.5;
SELECT 10::numeric(30,0) ^ 40;
SELECT 2.5::numeric(30,2)::int2 AS i2, (-2.5)::numeric(30,2)::int4 AS i4, 9223372036854775807.4::numeric(30,2)::int8 AS i8, 9.994::numeric(30,5)::numeric(3,2) AS t;
SELECT 32767.5::numeric(30,2)::int2;
SELECT 9.995::numeric(30,5)::numeric(3,2);
SELECT var_samp(x) AS v, stddev_pop(x) AS s, avg(x) AS a FROM (VALUES (1.5::numeric(30,10)), (2.25), (7)) t(x);
