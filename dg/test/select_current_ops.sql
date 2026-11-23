-- Verify CURRENT_DATE, CURRENT_TIME, LOCALTIME and their typmods, which ty
-- (ty/DESIGN.md section 19) serves from the time of the transaction in the
-- session's zone. Each is compared with now() in the same zone, so that the
-- answers do not depend on the day the test runs.
-- 1. In each zone, the date, the time and the offset are now()'s.
-- 2. A typmod rounds the time to that many digits, and not below 0.
-- 3. The values do not change in a transaction, however long it lasts, and
--    follow the zone when it is set inside the transaction.
-- 4. A time cast to timetz has the offset of the session's zone at that time
--    today, as a timestamp of today and that time has.
-- 5. A date and a timetz make the timestamptz that their offset says.
SET datestyle = 'ISO, MDY';
SET timezone = 'UTC';
SELECT current_date = now()::date AS d, current_time = now()::timetz AS t, localtime = now()::time AS lt, localtimestamp = now()::timestamp AS lts, extract(timezone FROM current_time) AS tz;
SET timezone = 'America/New_York';
SELECT current_date = now()::date AS d, current_time = now()::timetz AS t, localtime = now()::time AS lt, localtimestamp = now()::timestamp AS lts, extract(timezone FROM current_time) = extract(timezone FROM now()) AS tz;
SET timezone = 'Asia/Kolkata';
SELECT current_date = now()::date AS d, current_time = now()::timetz AS t, localtime = now()::time AS lt, extract(timezone FROM current_time) AS tz;
SET timezone = 'Asia/Kathmandu';
SELECT current_date = now()::date AS d, current_time = now()::timetz AS t, localtime = now()::time AS lt, extract(timezone FROM current_time) AS tz;
SET timezone = 'Pacific/Chatham';
SELECT current_date = now()::date AS d, current_time = now()::timetz AS t, localtime = now()::time AS lt, extract(timezone FROM current_time) = extract(timezone FROM now()) AS tz;
SET timezone = 'Pacific/Kiritimati';
SELECT current_date = now()::date AS d, current_time = now()::timetz AS t, localtime = now()::time AS lt, extract(timezone FROM current_time) AS tz;
SET timezone = 'UTC';
SELECT length(localtime(3)::text) <= 12 AS l3, length(localtime(0)::text) = 8 AS l0, length(current_time(0)::text) = 11 AS c0, length(current_time(1)::text) <= 13 AS c1, localtime(6) = localtime AS l6, current_time(6) = current_time AS c6;
BEGIN;
CREATE TEMP TABLE cur AS SELECT current_date AS d, current_time::text AS t, localtime AS lt, localtimestamp AS lts, now() AS n;
SELECT pg_sleep(0.05);
SELECT d = current_date AS d, t = current_time::text AS t, lt = localtime AS lt, lts = localtimestamp AS lts, n = now() AS n FROM cur;
SET LOCAL timezone = 'Asia/Tokyo';
SELECT extract(timezone FROM current_time) AS tz, current_time = now()::timetz AS t, current_date = now()::date AS d, extract(timezone FROM now()) AS now_tz;
SET LOCAL timezone = 'America/Los_Angeles';
SELECT extract(timezone FROM current_time) = extract(timezone FROM now()) AS tz, current_time = now()::timetz AS t, (SELECT d FROM cur) = (SELECT d FROM cur) AS same;
COMMIT;
-- A time cast to timetz takes the offset of the zone at that time today.
SELECT z, t, extract(timezone FROM t::timetz) = extract(timezone FROM (current_date + t)::timestamptz) AS ok FROM (VALUES ('UTC'), ('America/New_York'), ('Europe/London'), ('Australia/Lord_Howe'), ('Australia/Sydney'), ('Asia/Kolkata')) zs(z), (VALUES (time '00:30'), (time '01:30'), (time '02:30'), (time '03:30'), (time '12:00'), (time '23:59:59'), (time '24:00')) ts(t) WHERE set_config('timezone', z, false) = z ORDER BY z, t;
SET timezone = 'UTC';
-- A date and a timetz make the timestamptz that their offset says.
SELECT d + tz AS ts, (d + tz)::text AS txt FROM (VALUES (date '2024-07-04'), (date '1999-12-31'), (date '2100-02-28')) ds(d), (VALUES (timetz '12:34:56.789012+02'), (timetz '00:00:00-05:30'), (timetz '23:59:59+14')) tzs(tz) ORDER BY d, tz;
SELECT date 'infinity' + timetz '12:00:00+00' AS inf, date '-infinity' + timetz '12:00:00+00' AS ninf;
SELECT date '294277-01-01' + timetz '00:00:00+00';
