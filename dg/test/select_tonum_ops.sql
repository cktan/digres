-- Verify to_char of numbers, which ty writes (ty_tochar.h): the locale's
-- symbols for D, G, L and S, and TM's month names, by a German locale; a
-- format that changes from row to row; and each error of a format. A
-- refusal is what is tested, so an error goes on to the next statement.
\set ON_ERROR_STOP off
SET lc_numeric = 'de_DE.utf8';
SET lc_monetary = 'de_DE.utf8';
SET lc_time = 'de_DE.utf8';
SELECT to_char(1234567.891::float8, 'L9G999G999D999S') AS de, to_char(-1234.5::numeric, 'FM9G999D99S') AS num, to_char(-42, 'S9999') AS i4, to_char(1234567::int8, '9,999,999.99') AS comma;
SELECT to_char(timestamp '2024-03-15 10:00:00', 'TMMonth TMMon TMDay TMDy TMMONTH') AS tm;
RESET lc_numeric;
RESET lc_monetary;
RESET lc_time;
SELECT f, to_char(1234.5::float8, f) AS f8, to_char(-1234, f) AS i4, to_char(1234.5::numeric, f) AS num FROM (VALUES ('9999.99'), ('FM9999.99'), ('9999.99'), ('S9,999.9'), ('RN'), ('9.9EEEE'), (''), (NULL)) v(f);
SELECT to_char(1, '9.9.9');
SELECT to_char(1, 'S9S');
SELECT to_char(1, 'RNMI');
SELECT to_char(1, 'FM9EEEE');
SELECT to_char(1, '9V9999999999');
SELECT to_char(1::int8, '9V9999999999999999999');
SELECT to_char('NaN'::float8, '999TH');
SELECT to_char(5, 'TH');
