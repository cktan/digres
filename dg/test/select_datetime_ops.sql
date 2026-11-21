-- Verify the arithmetic of date, time and timestamp, which ty does
-- (ty/DESIGN.md section 19), in the ISO style and the default time zone.
-- 1. date + integer, date - integer, date - date, and the infinities.
-- 2. make_date and make_time.
-- 3. time + interval, time - interval, time - time.
-- 4. timestamp + interval and - interval: months with a short month, days,
--    microseconds, and the infinities.
-- 5. timestamp - timestamp, which justifies the hours into days.
-- (Errors are server.sh's: a run stops at its first.)
SELECT d, d + 1 AS p1, d - 1 AS m1, d + 366 AS p366, d - 400 AS m400, CASE WHEN d IN ('infinity', '-infinity') THEN NULL ELSE d - '2000-01-01'::date END AS diff, d + 0 AS p0 FROM (VALUES ('2000-01-01'::date), ('1999-12-31'), ('2000-02-28'), ('2024-02-29'), ('0001-01-01'), ('4700-01-01 BC'), ('5800000-01-01'), ('infinity'), ('-infinity')) v(d);
SELECT make_date(2024, 2, 29) AS a, make_date(1, 1, 1) AS b, make_date(-1, 12, 31) AS c, make_date(-4712, 1, 1) AS d, make_date(2000, 1, 31) AS e, make_date(294276, 12, 31) AS f, make_date(5874897, 12, 31) AS g;
SELECT make_time(0, 0, 0) AS a, make_time(23, 59, 59.999999) AS b, make_time(24, 0, 0) AS c, make_time(12, 30, 15.5) AS d, make_time(1, 2, 3.4999994) AS e, make_time(1, 2, 3.4999996) AS f;
SELECT t, t + i AS pl, t - i AS mi FROM (VALUES ('00:00'::time, '1 hour'::interval), ('23:00', '2 hours'), ('01:00', '2 hours'), ('12:00', '1 day'), ('12:00', '-36 hours'), ('12:34:56.789', '100 hours 30 minutes'), ('00:00', '1 month 2 days'), ('00:00:00.000001', '-1 microsecond')) v(t, i);
SELECT '12:00'::time - '10:30'::time AS a, '10:30'::time - '12:00'::time AS b, '00:00'::time - '23:59:59.999999'::time AS c, '24:00'::time - '00:00'::time AS d;
SELECT t, t + i AS pl, t - i AS mi FROM (VALUES ('2000-01-31 12:00'::timestamp, '1 month'::interval), ('2000-01-31', '1 month 1 day'), ('2000-03-31', '-1 month'), ('2024-02-29', '1 year'), ('2024-02-29', '-1 year'), ('2000-01-01', '13 months 40 days 5 hours'), ('2000-01-01 00:00:00.5', '-1 microsecond'), ('0001-01-01', '-1 day'), ('0001-01-01', '1 year'), ('4713-12-31 BC', '1 day'), ('infinity', '1 day'), ('-infinity', '1 day'), ('2000-01-01', 'infinity'), ('2000-01-01', '-infinity'), ('2000-01-31', '12 months'), ('1999-12-31 23:59:59.999999', '1 microsecond'), ('2000-01-01', '-100 years'), ('2000-01-01', '5000 years')) v(t, i);
SELECT a, b, a - b AS diff FROM (VALUES ('2000-01-02'::timestamp, '2000-01-01'::timestamp), ('2000-01-01', '2000-01-02'), ('2000-01-01 12:00', '2000-01-01 00:00'), ('2024-03-01', '2024-02-01'), ('2024-01-01', '2000-01-01'), ('0001-01-01', '4713-01-01 BC'), ('infinity', '2000-01-01'), ('2000-01-01', 'infinity'), ('-infinity', '2000-01-01'), ('2000-01-01', '-infinity'), ('2000-01-01 00:00:00.000001', '2000-01-01'), ('2000-01-01', '2000-01-01 00:00:00.000001')) v(a, b);
SELECT d, extract(dow FROM d) AS dow, extract(doy FROM d) AS doy, d::timestamp + interval '1 day' AS next, (d + 7)::text AS week FROM (VALUES ('2000-01-01'::date), ('2024-12-31'), ('1900-03-01'), ('1582-10-04')) v(d);
SELECT g::date AS d, g + interval '1 month' AS plus FROM generate_series('2024-01-31'::timestamp, '2024-05-31', interval '1 month') g;
SELECT count(*), min(d), max(d) FROM (SELECT '2000-01-01'::date + (i * 37 % 10000) d FROM generate_series(1, 2000) i) x;
