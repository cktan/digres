-- Verify numeric's text in and out, which ty reads and writes
-- (ty_decimal.h): each form, a typmod's rounding, the limits of 36
-- digits, each error with its detail, and the casts with the floats.
-- The binary form's typmod is in server.sh, which can COPY a file.
-- A refusal is tested, so an error goes on to the next statement.
\set ON_ERROR_STOP off
CREATE TEMP TABLE dec_in (i int primary key, x text);
INSERT INTO dec_in VALUES (1, '  -12.50  '), (2, '.5'), (3, '5.'), (4, '1_000.5'), (5, '1.5e2'), (6, '1.5e-2'), (7, '0x1F'), (8, '-0o17'), (9, '0b101'), (11, '0.000000000000000000000000000000000001'), (12, '-0');
SELECT i, x, numeric_in(x::cstring, 0, -1) AS n, numeric_in(x::cstring, 0, ((30 << 16) | 6) + 4) AS n30_6 FROM dec_in ORDER BY i;
SELECT numeric_in('123456789012345678901234567890123456.0000', 0, -1) AS n36;
SELECT '1.25'::numeric(3,1) AS a, '-1.25'::numeric(3,1) AS b, '99.94'::numeric(3,1) AS c, '0.5'::numeric(2,2) AS d, '9.995'::numeric(20,2) AS e;
SELECT '99.95'::numeric(3,1);
SELECT '1.5'::numeric(2,2);
SELECT '1234567890123456789012345678901234567'::numeric(36,0);
SELECT numeric_in('1.0000000000000000000000000000000000005', 0, -1);
SELECT 'abc'::numeric;
SELECT '1 2'::numeric;
SELECT 'NaN'::numeric;
SELECT '-Infinity'::numeric;
SELECT numeric_in('1e1001', 0, -1);
SELECT numeric_in(('0x' || repeat('F', 31))::cstring, 0, -1);
SELECT '0x'::numeric;
SELECT x, pg_input_is_valid(x, 'numeric(5,2)') AS ok, (pg_input_error_info(x, 'numeric(5,2)')).message FROM (VALUES ('123.456'), ('1234.5'), ('nan'), ('1e1001')) v(x);
SELECT min_scale(1.2500::numeric(10,4)) AS a, trim_scale(1.2500::numeric(10,4)) AS b, min_scale(0.000::numeric(10,3)) AS c, trim_scale(0.000::numeric(10,3)) AS d, trim_scale(123456789012345678901234.500000::numeric(30,6)) AS e;
SELECT 123456789012345678901234.567891::numeric(30,6)::float8 AS f8, 123456789012345678901234.567891::numeric(30,6)::float4 AS f4, 1.2345::numeric(10,4)::float8 AS d64f8, 1.2345::numeric(10,4)::float4 AS d64f4;
SELECT 0.1::float8::numeric(30,20) AS a, 1234567.891::float4::numeric(30,10) AS b, 1e30::float8::numeric(36,0) AS c, (-1e-30)::float8::numeric(36,36) AS d;
SELECT 'NaN'::float8::numeric(30,6);
SELECT 'Infinity'::float4::numeric(30,6);
SELECT 1e40::float8::numeric(36,0);
DROP TABLE dec_in;
