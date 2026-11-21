-- Verify the arithmetic of interval, which ty does (ty/DESIGN.md section 18):
-- the order, the sum, the product, the quotient and the justify functions,
-- including the infinities. (The text is DateStyle's, and stays in digres.)
-- 1. The order: a month is 30 days, a day 24 hours.
-- 2. + - * / and unary minus, and the infinities.
-- 3. justify_hours, justify_days and justify_interval.
-- 4. min, max, sum and avg, a sort, the hash and a group by.
-- (Errors are server.sh's: a run stops at its first.)
SELECT a, b, a < b AS lt, a = b AS eq, a > b AS gt, a <> b AS ne, interval_cmp(a, b) AS cmp FROM (VALUES ('1 month'::interval, '30 days'::interval), ('1 day', '24 hours'), ('1 month', '31 days'), ('-1 day', '1 hour'), ('0', '0 days'), ('1 year', '12 months'), ('1 year', '365 days'), ('infinity', '1000 years'), ('-infinity', '-1000 years'), ('infinity', 'infinity'), ('00:00:00.000001', '0')) v(a, b);
SELECT a, b, a + b AS pl, a - b AS mi, -a AS neg FROM (VALUES ('1 month 2 days 3 hours'::interval, '1 day 90 minutes'::interval), ('1 month', '-1 month'), ('10 days', '-20 days 5 hours'), ('infinity', '1 day'), ('1 day', 'infinity'), ('-infinity', '1 day'), ('0', '0'), ('2 years 3 months', '-1 year -2 months')) v(a, b);
SELECT a, a * 2 AS m2, a * 0.5 AS mh, a * 1.5 AS m15, a * -1 AS mn, a / 2 AS d2, a / 3 AS d3, a / 0.5 AS dh, a / -4 AS dn, 2.5 * a AS lm FROM (VALUES ('1 month'::interval), ('1 day'), ('1 hour'), ('1 month 1 day 1 hour'), ('-3 days 7 hours'), ('1.5 months'), ('0'), ('100 years'), ('15 days'), ('00:00:00.000001')) v(a);
SELECT a, a * 'infinity'::float8 AS mi, a * '-infinity'::float8 AS mni, 'infinity'::interval * 2 AS i2, '-infinity'::interval * -2 AS i3, 'infinity'::interval / 2 AS id FROM (VALUES ('1 day'::interval), ('-1 day')) v(a);
SELECT a, justify_hours(a) AS jh, justify_days(a) AS jd, justify_interval(a) AS ji FROM (VALUES ('1 month 40 days 50 hours'::interval), ('-1 month 40 days 50 hours'), ('29 days 25 hours'), ('1 day -1 hour'), ('-1 day 1 hour'), ('1 month -1 day'), ('-1 month 1 day'), ('35 days'), ('-35 days'), ('100 hours'), ('-100 hours'), ('1 month 2 days -3 hours'), ('infinity'), ('0')) v(a);
SELECT min(a) AS mn, max(a) AS mx, sum(a) AS sm, avg(a) AS av, count(DISTINCT a) AS dc FROM (VALUES ('1 month'::interval), ('2 days'), ('3 hours'), ('30 days'), ('-1 day')) v(a);
SELECT a FROM (VALUES ('1 month'::interval), ('2 days'), ('3 hours'), ('30 days'), ('-1 day'), ('0'), ('infinity'), ('-infinity'), ('720 hours')) v(a) ORDER BY a;
SELECT hashint8(1) IS NOT NULL AS ok, interval_hash('1 month') = interval_hash('30 days') AS h1, interval_hash('1 day') = interval_hash('24 hours') AS h2, interval_hash('1 day') = interval_hash('2 days') AS h3, interval_hash('1 month') AS h, interval_hash_extended('1 day', 5) AS hx;
SELECT a, count(*) FROM (SELECT ((i % 5) || ' days')::interval a FROM generate_series(1, 100) i) x GROUP BY a ORDER BY a;
SELECT count(*) FROM (SELECT (i || ' hours')::interval a FROM generate_series(1, 500) i) x JOIN (SELECT (i * 2 || ' hours')::interval b FROM generate_series(1, 500) i) y ON a = b;
