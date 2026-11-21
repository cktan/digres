-- Verify money, which ty does (ty/DESIGN.md section 17), in the C locale.
-- 1. Text in and out: symbols, signs, parentheses, separators, rounding.
-- 2. The order, + and -, and * and / with integers and floats.
-- 3. cash_words, and the casts with integers .
-- (Errors are server.sh's: a run stops at its first.)
SELECT '$1.23'::money AS a, '1.23'::money AS b, '-$1.23'::money AS c, '($1.23)'::money AS d, '$1,234.56'::money AS e, '1234567.891'::money AS f, '0.005'::money AS g, '0.004'::money AS h, '  $ 5 '::money AS i, '5-'::money AS j, '92233720368547758.07'::money AS k, '-92233720368547758.08'::money AS l, '1.455'::money AS m;
SELECT m, m < '$2'::money AS lt, m = '$1.50'::money AS eq, m + '$0.75'::money AS pl, m - '$2'::money AS mi, m * 3 AS mul_i, m * 2.5::float8 AS mul_f, m / 4 AS div_i, m / 0.5::float8 AS div_f, m / '$0.25'::money AS ratio FROM (VALUES ('$1.50'::money), ('$100.00'), ('-$3.33'), ('$0.00'), ('$12345678.90')) v(m);
SELECT 3 * '$1.10'::money AS a, 2.5::float8 * '$1.10'::money AS b, 3::int2 * '$1.10'::money AS c, 3::int8 * '$1.10'::money AS d, '$7.00'::money / 2::int2 AS e, '$7.00'::money / 2::int8 AS f, '$7.00'::money / 2.0::float4 AS g, '$7.00'::money * 1.5::float4 AS h, 1.5::float4 * '$7.00'::money AS i;
SELECT m FROM (VALUES ('$5'::money), ('$-1'), ('$0.01'), ('$100'), ('$-100.50')) v(m) ORDER BY m;
SELECT max(m) AS mx, min(m) AS mn, sum(m) AS sm FROM (VALUES ('$5'::money), ('$-1'), ('$0.01'), ('$100')) v(m);
SELECT cash_words(m) FROM (VALUES ('$0'::money), ('$0.01'), ('$1'), ('$1.01'), ('$21.15'), ('$100'), ('$101.10'), ('$1,000'), ('$1,234.56'), ('$12,345.67'), ('$1,000,000'), ('$999,999,999.99'), ('-$5.25'), ('$1,234,567,890,123.45'), ('$92,233,720,368,547,758.07')) v(m);
SELECT 5::int4::money AS a, 5::int8::money AS b, (-3)::int4::money AS e;
SELECT count(*), sum(m) FROM (SELECT (i * 7919 % 10007)::int4::money m FROM generate_series(1, 5000) i) x WHERE m > '$50'::money;
