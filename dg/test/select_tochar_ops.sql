-- Verify to_char of timestamp, timestamptz and interval, which ty writes
-- (ty_tochar.h): a format that changes from row to row, so the call site
-- compiles it again; TM's names under a collation; NULL and infinite
-- values; the session's zone; and each error. A refusal is what is tested,
-- so an error goes on to the next statement.
\set ON_ERROR_STOP off
SELECT f, to_char(timestamp '2024-02-29 13:05:09.123456', f) FROM (VALUES ('YYYY-MM-DD'), ('HH24:MI:SS.US'), ('YYYY-MM-DD'), ('Day, DD Month YYYY'), ('FMDay FMDDth'), (''), (NULL)) v(f);
SELECT to_char(timestamp '2024-02-29 13:05:09', 'TMDay TMMonth TMmon' COLLATE "unicode") AS icu, to_char(timestamp '2024-02-29 13:05:09', 'TMDAY TMMONTH') AS c;
SELECT to_char(timestamp 'infinity', 'YYYY') IS NULL AS inf, to_char(timestamptz '-infinity', 'YYYY') IS NULL AS ninf, to_char(interval 'infinity', 'HH24') IS NULL AS iinf, to_char(NULL::timestamp, 'YYYY') IS NULL AS n;
SET timezone = 'America/St_Johns';
SELECT to_char(timestamptz '2024-07-01 12:00:00+00', 'YYYY-MM-DD HH24:MI TZ OF TZH:TZM tz') AS stjohns;
SET timezone = 'Asia/Kathmandu';
SELECT to_char(timestamptz '2024-07-01 12:00:00+00', 'HH12:MI AM TZ OF FMOF') AS kathmandu;
RESET timezone;
SELECT to_char(interval '1 year 2 mons 3 days 04:05:06.789', 'YYYY MM DD HH24 MI SS MS US DDD WW J RM') AS iv, to_char(interval '-25 hours', 'HH HH12 HH24 SSSS AM') AS neg;
SELECT to_char(i, 'FMYYYY "years" FMMM "months"') FROM (VALUES (interval '12 mons'), (interval '-12 mons'), (interval '178000000 years')) v(i);
SELECT to_char(interval '1 day', 'Day');
SELECT to_char(interval '1 day', 'TZ');
SELECT to_char(interval '1 day', 'YYYY AD');
