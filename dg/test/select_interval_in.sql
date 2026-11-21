-- Verify the text in of interval, which ty does (ty/DESIGN.md section 18):
-- the postgres and sql forms, units, signs, fractions, ago, ISO 8601, and
-- the typmod. (Errors are server.sh's: a run stops at its first.)
SELECT i, i::interval AS v FROM (VALUES ('1 year 2 months 3 days 04:05:06'), ('@ 1 minute'), ('-1 days +02:03:04'), ('1:30'), ('1:30:45.5'), ('P1Y2M3DT4H5M6S'), ('P0001-02-03T04:05:06'), ('1-2'), ('-1-2'), ('1 day ago'), ('infinity'), ('-infinity'), ('1.5 years'), ('0.5 months'), ('1.2 weeks'), ('1 us'), ('2 ms'), ('1 decade 1 century 1 millennium'), ('1 year -2 months'), ('0.000001 seconds'), ('9223372036854775807 microseconds'), ('2147483647 months'), ('178956970 years'), ('25:00:00'), ('1 day 25:00:00'), ('PT1H30M'), ('P1W'), ('P1.5Y'), ('PT0.5S'), ('@ 1 day ago')) v(i);
SELECT '1 day 2 hours 3 minutes 4.5 seconds'::interval(0) AS a, '1 day 2 hours 3 minutes 4.5 seconds'::interval(1) AS b, '1 year 2 months 3 days'::interval year AS c, '1 year 2 months 3 days'::interval year to month AS d, '1 day 2 hours 3 minutes 4 seconds'::interval day AS e, '1 day 2 hours 3 minutes 4 seconds'::interval hour to minute AS f, '1 day 2 hours 3 minutes 4.123456 seconds'::interval second(3) AS g, '3 minutes 4 seconds'::interval minute to second AS h;
SET intervalstyle = 'sql_standard';
SELECT i::interval AS v FROM (VALUES ('-1 day 02:03:04'), ('1-2 3 4:5:6'), ('-1-2'), ('1 day -02:03:04'), ('+1 day -1 hour')) v(i);
SET intervalstyle = 'iso_8601';
SELECT '1 year 2 months 3 days 04:05:06.7'::interval AS v, 'P1Y2M3DT4H5M6.7S'::interval AS w, '-1 day 02:03:04'::interval AS x;
