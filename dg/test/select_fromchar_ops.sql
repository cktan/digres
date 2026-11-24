-- Verify the reverse parse, which ty does (ty_dch_parse): to_timestamp and
-- to_date with zone abbreviations from the session's table, TM's names in a
-- German locale, and jsonpath's datetime() in the standard mode, with each
-- type it makes and each of its errors. A refusal is what is tested, so an
-- error goes on to the next statement.
\set ON_ERROR_STOP off
SET timezone = 'America/New_York';
SELECT to_timestamp('2011-12-18 11:38 EST', 'YYYY-MM-DD HH12:MI TZ') AS est, to_timestamp('2011-07-18 11:38 MSK', 'YYYY-MM-DD HH12:MI TZ') AS msk, to_timestamp('2011-12-18 11:38', 'YYYY-MM-DD HH12:MI') AS local, to_timestamp('2024-03-10 02:30', 'YYYY-MM-DD HH24:MI') AS gap;
SELECT to_timestamp('2011-12-18 11:38 PST', 'YYYY-MM-DD HH12:MI TZ') AS pst, to_timestamp('2011-12-18 11:38 +0530', 'YYYY-MM-DD HH12:MI OF') AS of;
SELECT to_timestamp('2011-12-18 11:38 XYZ', 'YYYY-MM-DD HH12:MI TZ');
SET lc_time = 'de_DE.utf8';
SELECT to_date('15 März 2024', 'DD TMMonth YYYY') AS de_month, to_date('15 mär 2024', 'DD TMMon YYYY') AS de_mon, to_date('Freitag 15 MÄRZ 2024', 'TMDay DD TMMONTH YYYY') AS de_day;
SELECT to_date('15 March 2024', 'DD TMMonth YYYY');
RESET lc_time;
RESET timezone;
SELECT jsonb_path_query_first('"2024-02-29"'::jsonb, '$.datetime("YYYY-MM-DD")'::jsonpath, '{}', false) AS d, jsonb_path_query_first('"12:34:56.789"'::jsonb, '$.datetime("HH24:MI:SS.FF3")'::jsonpath, '{}', false) AS t, jsonb_path_query_first('"12:34:56 +05:30"'::jsonb, '$.datetime("HH24:MI:SS TZH:TZM")'::jsonpath, '{}', false) AS ttz, jsonb_path_query_first('"2024-02-29 12:34:56"'::jsonb, '$.datetime("YYYY-MM-DD HH24:MI:SS")'::jsonpath, '{}', false) AS ts, jsonb_path_query_first('"2024-02-29 12:34:56 -08"'::jsonb, '$.datetime("YYYY-MM-DD HH24:MI:SS TZH")'::jsonpath, '{}', false) AS tstz;
SELECT jsonb_path_query_first('"2024-02-29T12:34:56"'::jsonb, '$.datetime()'::jsonpath, '{}', false) AS iso, jsonb_path_query_first('"2024-02-29"'::jsonb, '$.datetime("YYYY-MM-DD").type()'::jsonpath, '{}', false) AS typ;
SELECT jsonb_path_query_first('"2024/02/29"'::jsonb, '$.datetime("YYYY-MM-DD")'::jsonpath, '{}', false);
SELECT jsonb_path_query_first('"2024-02-29"'::jsonb, '$.datetime("YYYY-MM-DD HH24")'::jsonpath, '{}', false);
SELECT jsonb_path_query_first('"2024-02-29 x"'::jsonb, '$.datetime("YYYY-MM-DD")'::jsonpath, '{}', false);
SELECT jsonb_path_query_first('"2024-02-29"'::jsonb, '$.datetime("YYYY*MM*DD")'::jsonpath, '{}', false);
SELECT jsonb_path_query_first('"20240229"'::jsonb, '$.datetime("YYYY\"x\"MMDD")'::jsonpath, '{}', false);
SELECT jsonb_path_query_first('"2024-02-29 +05"'::jsonb, '$.datetime("YYYY-MM-DD TZH")'::jsonpath, '{}', false);
SELECT jsonb_path_query_first('"12:00"'::jsonb, '$.datetime("HH24:MI TZH")'::jsonpath, '{}', false);
SELECT jsonb_path_query_first('"2024"'::jsonb, '$.datetime("TZH")'::jsonpath, '{}', false);
SELECT jsonb_path_query_first('"2024-02-30"'::jsonb, '$.datetime("YYYY-MM-DD")'::jsonpath, '{}', false);
SELECT jsonb_path_query_first('"2024-02-30"'::jsonb, '$.datetime("YYYY-MM-DD")'::jsonpath, '{}', true) IS NULL AS silent;
SELECT jsonb_path_query_first_tz('"2024-02-29 12:00"'::jsonb, '$.datetime("YYYY-MM-DD HH24:MI").timestamp_tz()'::jsonpath, '{}', false) AS tz;
