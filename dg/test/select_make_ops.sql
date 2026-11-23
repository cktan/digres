-- Verify make_timestamp, make_timestamptz and make_timestamptz with a time zone,
-- which ty serves (ty_timestamp_make): the values, the edges of the range, and
-- the messages, which word the year as it was given. A refusal is what is
-- tested, so an error goes on to the next statement.
\set ON_ERROR_STOP off
SET datestyle = 'ISO, MDY';
SET timezone = 'UTC';
SELECT make_timestamp(2024, 7, 4, 12, 34, 56.789012) AS ts, make_timestamp(2000, 1, 1, 0, 0, 0) AS y2k, make_timestamp(-1, 1, 1, 0, 0, 0) AS bc1, make_timestamp(-4714, 11, 24, 0, 0, 0) AS first, make_timestamp(294276, 12, 31, 23, 59, 59.999999) AS last, make_timestamp(2024, 2, 29, 24, 0, 0) AS h24, make_timestamp(2024, 1, 1, 0, 0, 0.0000005) AS round_up, make_timestamp(2024, 1, 1, 0, 0, 0.0000015) AS round_even;
SELECT make_timestamptz(2024, 7, 4, 12, 34, 56.789012) AS utc;
SET timezone = 'America/New_York';
SELECT make_timestamptz(2024, 7, 4, 12, 34, 56.789012) AS ny, make_timestamptz(2024, 3, 10, 2, 30, 0) AS gap, make_timestamptz(2024, 11, 3, 1, 30, 0) AS fold, make_timestamptz(-4714, 11, 24, 0, 0, 0) AS first;
SELECT make_timestamptz(2024, 7, 4, 12, 0, 0, 'UTC') AS utc, make_timestamptz(2024, 7, 4, 12, 0, 0, 'Europe/London') AS lon, make_timestamptz(2024, 1, 4, 12, 0, 0, 'Europe/London') AS lon_w, make_timestamptz(2024, 3, 31, 1, 30, 0, 'Europe/London') AS lon_gap, make_timestamptz(2024, 10, 27, 1, 30, 0, 'Europe/London') AS lon_fold, make_timestamptz(2024, 7, 4, 12, 0, 0, 'EST') AS est, make_timestamptz(2024, 7, 4, 12, 0, 0, 'EDT') AS edt, make_timestamptz(2024, 7, 4, 12, 0, 0, 'MSK') AS msk, make_timestamptz(2024, 7, 4, 12, 0, 0, '+05:30') AS num, make_timestamptz(2024, 7, 4, 12, 0, 0, '-03') AS neg, make_timestamptz(2024, 7, 4, 12, 0, 0, 'Asia/Kolkata') AS kol;
SELECT make_timestamp(0, 1, 1, 0, 0, 0);
SELECT make_timestamp(-5, 13, 1, 0, 0, 0);
SELECT make_timestamp(2023, 2, 29, 0, 0, 0);
SELECT make_timestamp(2024, 1, 1, 25, 0, 0);
SELECT make_timestamp(2024, 1, 1, 0, 60, 0);
SELECT make_timestamp(2024, 1, 1, 0, 0, 60.5);
SELECT make_timestamp(2024, 1, 1, 0, 0, 'NaN');
SELECT make_timestamp(2024, 1, 1, 0, 0, 'Infinity');
SELECT make_timestamp(-4714, 11, 23, 0, 0, 0);
SELECT make_timestamp(-4714, 1, 1, 0, 0, 0);
SELECT make_timestamp(5874897, 12, 31, 0, 0, 0);
SELECT make_timestamp(5874898, 1, 1, 0, 0, 0);
SELECT make_timestamp(5874899, 1, 1, 0, 0, 0);
SELECT make_timestamp(294276, 12, 31, 24, 0, 0);
SELECT make_timestamp(294277, 1, 1, 0, 0, 0);
SELECT make_timestamp(-2147483648, 1, 1, 0, 0, 0);
SELECT make_timestamptz(2024, 13, 1, 0, 0, 0);
SELECT make_timestamptz(294277, 1, 1, 0, 0, 0, 'UTC');
SELECT make_timestamptz(2024, 7, 4, 12, 0, 0, 'Nowhere/Land');
SELECT make_timestamptz(2024, 7, 4, 12, 0, 0, '5');
SELECT make_timestamptz(294276, 12, 31, 23, 59, 59.999999, '-01');
