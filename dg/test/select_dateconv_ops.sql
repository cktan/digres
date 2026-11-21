-- Verify the conversions between date, time, timestamp and interval, which ty
-- does (ty/DESIGN.md section 19).
-- (Errors are server.sh's: a run stops at its first.)
SELECT d::text, d::timestamp::text AS ts, (d + time '13:45:56.789012')::text AS dt, (d + time '24:00')::text AS dt24, d::timestamp::date::text AS back FROM (VALUES ('2024-02-29'::date), ('0001-01-01'), ('4714-11-24 BC'), ('1999-12-31'), ('2000-01-01'), ('infinity'), ('-infinity')) x(d) ORDER BY d;
SELECT t::text, t::date::text AS d, t::time::text AS tm FROM (VALUES ('2024-02-29 13:45:56.789012'::timestamp), ('0044-03-15 12:00:00.5 BC'), ('1999-12-31 23:59:59.999999'), ('2000-01-01 00:00:00'), ('1969-12-31 00:00:00.000001'), ('infinity'), ('-infinity')) x(t) ORDER BY t;
SELECT i::text, i::time::text AS tm, (i::time)::interval::text AS back FROM (VALUES ('1 day 02:03:04.5'::interval), ('-2 hours'), ('0'), ('25 hours'), ('-25 hours -1 minute'), ('3 months 4 days 05:06:07'), ('-1 microsecond'), ('24:00:00')) x(i) ORDER BY i;
SELECT d::text, d < t AS lt, d = t AS eq, d > t AS gt, d <= t AS le, d >= t AS ge, d <> t AS ne FROM (VALUES ('2024-02-29'::date), ('2000-01-01'), ('infinity'), ('-infinity')) a(d), (VALUES ('2024-02-29 00:00:00'::timestamp), ('2024-02-29 00:00:01'), ('2023-12-31 23:59:59.999999'), ('infinity'), ('-infinity')) b(t) ORDER BY 1, 2, 3, 4;
