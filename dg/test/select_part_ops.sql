-- Verify extract and date_part of timestamp, date, time and interval, which
-- ty does (ty/DESIGN.md section 19).
-- (Errors are server.sh's: a run stops at its first.)
SELECT * FROM (
SELECT 'microseconds' AS u, t::text AS v, extract('microseconds' FROM t)::text AS e, date_part('microseconds', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'milliseconds' AS u, t::text AS v, extract('milliseconds' FROM t)::text AS e, date_part('milliseconds', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'second' AS u, t::text AS v, extract('second' FROM t)::text AS e, date_part('second', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'minute' AS u, t::text AS v, extract('minute' FROM t)::text AS e, date_part('minute', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'hour' AS u, t::text AS v, extract('hour' FROM t)::text AS e, date_part('hour', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'day' AS u, t::text AS v, extract('day' FROM t)::text AS e, date_part('day', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'week' AS u, t::text AS v, extract('week' FROM t)::text AS e, date_part('week', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'month' AS u, t::text AS v, extract('month' FROM t)::text AS e, date_part('month', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'quarter' AS u, t::text AS v, extract('quarter' FROM t)::text AS e, date_part('quarter', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'year' AS u, t::text AS v, extract('year' FROM t)::text AS e, date_part('year', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'decade' AS u, t::text AS v, extract('decade' FROM t)::text AS e, date_part('decade', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'century' AS u, t::text AS v, extract('century' FROM t)::text AS e, date_part('century', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'millennium' AS u, t::text AS v, extract('millennium' FROM t)::text AS e, date_part('millennium', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'julian' AS u, t::text AS v, extract('julian' FROM t)::text AS e, date_part('julian', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'dow' AS u, t::text AS v, extract('dow' FROM t)::text AS e, date_part('dow', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'doy' AS u, t::text AS v, extract('doy' FROM t)::text AS e, date_part('doy', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'isoyear' AS u, t::text AS v, extract('isoyear' FROM t)::text AS e, date_part('isoyear', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'isodow' AS u, t::text AS v, extract('isodow' FROM t)::text AS e, date_part('isodow', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'epoch' AS u, t::text AS v, extract('epoch' FROM t)::text AS e, date_part('epoch', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'YEAR' AS u, t::text AS v, extract('YEAR' FROM t)::text AS e, date_part('YEAR', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
UNION ALL
SELECT 'Epoch' AS u, t::text AS v, extract('Epoch' FROM t)::text AS e, date_part('Epoch', t)::text AS dp FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('2021-01-03 10:00:00'), ('2020-12-31 23:59:59.999999'), ('0044-03-15 12:00:00.5 BC'), ('0001-01-01 00:00:00'), ('1999-12-27 08:30:00'), ('1970-01-01 00:00:00'), ('1969-12-31 23:59:59.999999'), ('4713-11-24 00:00:00 BC'), ('9999-12-31 23:59:59.999999')) b(t)
) x ORDER BY 1, 2, 3, 4;
SELECT * FROM (
SELECT 'day' AS u, d::text AS v, extract('day' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'week' AS u, d::text AS v, extract('week' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'month' AS u, d::text AS v, extract('month' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'quarter' AS u, d::text AS v, extract('quarter' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'year' AS u, d::text AS v, extract('year' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'decade' AS u, d::text AS v, extract('decade' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'century' AS u, d::text AS v, extract('century' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'millennium' AS u, d::text AS v, extract('millennium' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'julian' AS u, d::text AS v, extract('julian' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'dow' AS u, d::text AS v, extract('dow' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'doy' AS u, d::text AS v, extract('doy' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'isoyear' AS u, d::text AS v, extract('isoyear' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'isodow' AS u, d::text AS v, extract('isodow' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
UNION ALL
SELECT 'epoch' AS u, d::text AS v, extract('epoch' FROM d)::text AS e FROM (VALUES ('2024-02-29'::date), ('2021-01-03'), ('0044-03-15 BC'), ('0001-01-01'), ('1999-12-27'), ('1970-01-01'), ('1969-12-31'), ('2000-01-01')) b(d)
) x ORDER BY 1, 2, 3;
SELECT * FROM (
SELECT 'microseconds' AS u, t::text AS v, extract('microseconds' FROM t)::text AS e, date_part('microseconds', t)::text AS dp FROM (VALUES ('13:45:56.789012'::time), ('00:00:00'), ('23:59:59.999999'), ('24:00:00'), ('00:00:00.000001'), ('12:00:00.5')) b(t)
UNION ALL
SELECT 'milliseconds' AS u, t::text AS v, extract('milliseconds' FROM t)::text AS e, date_part('milliseconds', t)::text AS dp FROM (VALUES ('13:45:56.789012'::time), ('00:00:00'), ('23:59:59.999999'), ('24:00:00'), ('00:00:00.000001'), ('12:00:00.5')) b(t)
UNION ALL
SELECT 'second' AS u, t::text AS v, extract('second' FROM t)::text AS e, date_part('second', t)::text AS dp FROM (VALUES ('13:45:56.789012'::time), ('00:00:00'), ('23:59:59.999999'), ('24:00:00'), ('00:00:00.000001'), ('12:00:00.5')) b(t)
UNION ALL
SELECT 'minute' AS u, t::text AS v, extract('minute' FROM t)::text AS e, date_part('minute', t)::text AS dp FROM (VALUES ('13:45:56.789012'::time), ('00:00:00'), ('23:59:59.999999'), ('24:00:00'), ('00:00:00.000001'), ('12:00:00.5')) b(t)
UNION ALL
SELECT 'hour' AS u, t::text AS v, extract('hour' FROM t)::text AS e, date_part('hour', t)::text AS dp FROM (VALUES ('13:45:56.789012'::time), ('00:00:00'), ('23:59:59.999999'), ('24:00:00'), ('00:00:00.000001'), ('12:00:00.5')) b(t)
UNION ALL
SELECT 'epoch' AS u, t::text AS v, extract('epoch' FROM t)::text AS e, date_part('epoch', t)::text AS dp FROM (VALUES ('13:45:56.789012'::time), ('00:00:00'), ('23:59:59.999999'), ('24:00:00'), ('00:00:00.000001'), ('12:00:00.5')) b(t)
) x ORDER BY 1, 2, 3, 4;
SELECT * FROM (
SELECT 'microseconds' AS u, i::text AS v, extract('microseconds' FROM i)::text AS e, date_part('microseconds', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'milliseconds' AS u, i::text AS v, extract('milliseconds' FROM i)::text AS e, date_part('milliseconds', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'second' AS u, i::text AS v, extract('second' FROM i)::text AS e, date_part('second', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'minute' AS u, i::text AS v, extract('minute' FROM i)::text AS e, date_part('minute', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'hour' AS u, i::text AS v, extract('hour' FROM i)::text AS e, date_part('hour', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'day' AS u, i::text AS v, extract('day' FROM i)::text AS e, date_part('day', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'week' AS u, i::text AS v, extract('week' FROM i)::text AS e, date_part('week', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'month' AS u, i::text AS v, extract('month' FROM i)::text AS e, date_part('month', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'quarter' AS u, i::text AS v, extract('quarter' FROM i)::text AS e, date_part('quarter', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'year' AS u, i::text AS v, extract('year' FROM i)::text AS e, date_part('year', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'decade' AS u, i::text AS v, extract('decade' FROM i)::text AS e, date_part('decade', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'century' AS u, i::text AS v, extract('century' FROM i)::text AS e, date_part('century', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'millennium' AS u, i::text AS v, extract('millennium' FROM i)::text AS e, date_part('millennium', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
UNION ALL
SELECT 'epoch' AS u, i::text AS v, extract('epoch' FROM i)::text AS e, date_part('epoch', i)::text AS dp FROM (VALUES ('1 year 2 months 3 days 04:05:06.789'::interval), ('-1 year -2 months -3 days -04:05:06.789'), ('0'), ('25 years 7 months 400 days'), ('-25 years -7 months'), ('1500 years'), ('-5 months'), ('-4 months'), ('-1 month'), ('11 months 99999 hours')) b(i)
) x ORDER BY 1, 2, 3, 4;
