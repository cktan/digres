-- Verify the float operations, which ty does (ty/DESIGN.md section 11),
-- and what a client reads from them: arithmetic, NaN's place in the order,
-- rounding, casts, text in and out at each extra_float_digits, powers and
-- logarithms, width_bucket and the hash.
-- 1. Add, subtract, multiply and divide; the sign of zero.
-- 2. NaN equals NaN and sorts last; max and min; every compare.
-- 3. round, ceil, floor, trunc and sign; a tie rounds to the even.
-- 4. Casts between the floats and the integers.
-- 5. Text out at extra_float_digits 3, 1, 0 and -3, and text in.
-- 6. sqrt, cbrt, exp, ln, log and power, with the cases POSIX leaves open.
-- 7. width_bucket, in both directions.
-- 8. The hash: zero and -0 alike, every NaN alike, float4 as float8.
-- 9. Trigonometry in radians and in degrees, exact where the angle is,
--    the hyperbolic functions, erf and gamma, and pi. As in
--    PostgreSQL's float8 test, the hyperbolic functions print at
--    extra_float_digits 0, and erf and gamma at -1: the libm of each
--    platform can differ in the last place.
-- 10. avg, variance, stddev, covariance and regression of float8 and
--     float4: their results, their NULLs, and the combine of two states.
-- (Errors are server.sh's: a run stops at its first.)
SELECT 1.5::float8 + 2.25 AS a, 1.5::float8 - 2.25 AS b, 1.5::float8 * 2.25 AS c, 1.5::float8 / 2.25 AS d, (-0.0::float8)::text AS e, (0.0::float8 * -1)::text AS f, 1.5::float4 + 2.25::float4 AS g, 1::float4 / 3::float4 AS h, 'Infinity'::float8 + 1 AS i, 'NaN'::float8 * 0 AS j;
SELECT x FROM (VALUES ('NaN'::float8), (1), ('Infinity'), (-1), ('-Infinity'), (0), ('NaN')) v(x) ORDER BY x;
SELECT max(x), min(x), 'NaN'::float8 = 'NaN'::float8 AS eq, 'NaN'::float8 > 'Infinity'::float8 AS gt, 0.0::float8 = -0.0::float8 AS zero, 'NaN'::float4 < 1::float4 AS lt FROM (VALUES ('NaN'::float8), (1), ('Infinity'), (-1)) v(x);
SELECT round(2.5::float8) AS a, round(3.5::float8) AS b, round(-2.5::float8) AS c, ceil(-0.5::float8)::text AS d, floor(2.5::float8) AS e, trunc(-2.7::float8) AS f, trunc(2.7::float8) AS g, sign(-3.2::float8) AS h, sign(0::float8) AS i, ceil(1.2::float8) AS j;
SELECT 2.5::float8::int4 AS a, 3.5::float8::int8 AS b, 2.5::float4::int2 AS c, (-0.5::float8)::int4 AS d, 1.5::float4::float8 AS e, 0.1::float8::float4 AS f, 7::int4::float8 AS g, 16777217::int4::float4 AS h, 9007199254740993::int8::float8 AS i;
SET extra_float_digits = 3;
SELECT 0.1::float8 AS a, (0.1::float8 + 0.2) AS b, 1e23::float8 AS c, 123456789012345678::float8 AS d, 5e-324::float8 AS e, 0.1::float4 AS f, 122908496::float4 AS g, 3.4028235e38::float4 AS h;
SET extra_float_digits = 1;
SELECT 0.1::float8 AS a, (0.1::float8 + 0.2) AS b, 1e-5::float8 AS c, 100000000000000000000::float8 AS d, (1::float4 / 3::float4) AS e;
SET extra_float_digits = 0;
SELECT 0.1::float8 AS a, (0.1::float8 + 0.2) AS b, (1::float8 / 3) AS c, (1::float4 / 3::float4) AS d, 1e23::float8 AS e, 'NaN'::float8 AS f, '-Infinity'::float8 AS g;
SET extra_float_digits = -3;
SELECT (1::float8 / 3) AS a, (1::float4 / 3::float4) AS b, 123456789.125::float8 AS c;
RESET extra_float_digits;
SELECT ' 1.5 '::float8 AS a, '+Infinity'::float8 AS b, '-inf'::float8 AS c, 'nan'::float8 AS d, '1e-320'::float8 AS e, '.5'::float8 AS f, '5.'::float8 AS g, '1E+3'::float4 AS h, E'\t-2\n'::float8 AS i, '0x10'::float8 AS j;
SELECT sqrt(2::float8) AS a, cbrt(27::float8) AS b, exp(1::float8) AS c, ln(10::float8) AS d, log(100::float8) AS e, power(2::float8, 0.5) AS f, power(0::float8, 0) AS g, power('NaN'::float8, 0) AS h, power(1, 'NaN'::float8) AS i, exp('-Infinity'::float8) AS j, power(-2::float8, 3) AS k, power(2::float8, '-Infinity') AS l;
SELECT width_bucket(5.35::float8, 0.024, 10.06, 5) AS a, width_bucket(-1::float8, 0, 10, 5) AS b, width_bucket(11::float8, 0, 10, 5) AS c, width_bucket(5::float8, 10, 0, 5) AS d, width_bucket(10::float8, 0, 10, 5) AS e, width_bucket('Infinity'::float8, 0, 10, 5) AS f;
SELECT hashfloat8(0.0::float8) = hashfloat8(-0.0::float8) AS zero, hashfloat8('NaN'::float8) = hashfloat8('-NaN'::float8) AS nan, hashfloat4(1.5::float4) = hashfloat8(1.5::float8) AS widened, hashfloat8(1.5::float8) <> hashfloat8(2.5::float8) AS differs, hashfloat8extended(0.0::float8, 7) = 7 AS seed, (hashfloat8extended(1.5::float8, 0) & 4294967295) = (hashfloat8(1.5::float8)::int8 & 4294967295) AS low32;
SELECT sind(30::float8) AS a, sind(90::float8) AS b, cosd(60::float8) AS c, cosd(90::float8) AS d, tand(45::float8) AS e, cotd(45::float8) AS f, sind(-30::float8) AS g, cosd(180::float8) AS h, sind(210::float8) AS i, tand(135::float8) AS j, cosd(120::float8) AS k, sind(360::float8) AS l, sind(1e5::float8) AS m;
SELECT asind(0.5::float8) AS a, acosd(0.5::float8) AS b, atand(1::float8) AS c, atan2d(1::float8, 1) AS d, acosd(-1::float8) AS e, asind(-1::float8) AS f, acosd(0::float8) AS g, atan2d(0::float8, -1) AS h;
SELECT sin(0::float8) AS a, cos(0::float8) AS b, tan(0::float8) AS c, cot(1::float8) AS d, asin(1::float8) AS e, acos(1::float8) AS f, atan(1::float8) AS g, atan2(1::float8, 1) AS h, sin(pi() / 2) AS i, cos(pi()) AS j, pi() AS k, degrees(pi()) AS l, radians(180::float8) AS m;
SET extra_float_digits = 0;
SELECT sinh(1::float8) AS a, cosh(1::float8) AS b, tanh(1::float8) AS c, asinh(1::float8) AS d, acosh(2::float8) AS e, atanh(0.5::float8) AS f, atanh(1::float8) AS g, sinh('Infinity'::float8) AS h, tanh('-Infinity'::float8) AS i, cosh(0::float8) AS j;
SET extra_float_digits = -1;
SELECT erf(1::float8) AS a, erfc(1::float8) AS b, erf('Infinity'::float8) AS c, gamma(5::float8) AS d, gamma(0.5::float8) AS e, lgamma(1::float8) AS f, gamma(1::float8) AS g, cot(0::float8) AS h, sin('NaN'::float8) AS i, cosd('NaN'::float8) AS j;
RESET extra_float_digits;
SELECT avg(x), var_pop(x), var_samp(x), stddev_pop(x), stddev_samp(x) FROM (VALUES (1::float8), (2), (4), (8), (16)) v(x);
SELECT avg(x), var_samp(x), stddev_samp(x), var_pop(x) FROM (VALUES (5::float8)) v(x);
SELECT avg(x), var_pop(x), var_samp(x), stddev_pop(x) FROM (SELECT 1::float8 AS x WHERE false) v;
SELECT avg(x), var_pop(x), stddev_pop(x) FROM (VALUES ('Infinity'::float8), (1)) v(x);
SELECT avg(x), var_samp(x), stddev_pop(x) FROM (VALUES (1.5::float4), (2.5), (4.5)) v(x);
SELECT covar_pop(y, x), covar_samp(y, x), corr(y, x), regr_slope(y, x), regr_intercept(y, x), regr_r2(y, x), regr_avgx(y, x), regr_avgy(y, x), regr_sxx(y, x), regr_syy(y, x), regr_sxy(y, x), regr_count(y, x) FROM (VALUES (1::float8, 1::float8), (3, 2), (2, 3), (5, 4), (4, 5)) v(y, x);
SELECT regr_slope(y, x), regr_r2(y, x), corr(y, x), covar_samp(y, x) FROM (VALUES (1::float8, 2::float8), (5, 2)) v(y, x);
SELECT regr_r2(y, x), corr(y, x) FROM (VALUES (3::float8, 1::float8), (3, 2), (3, 5)) v(y, x);
SELECT x, avg(x) OVER (ORDER BY x ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS m, var_pop(x) OVER (ORDER BY x ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS v FROM (VALUES (1::float8), (2), (4), (8)) w(x) ORDER BY x;
