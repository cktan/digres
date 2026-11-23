-- Verify format(), whose format string ty reads and whose result ty makes
-- (ty_format_next, ty_format_put): text, %%, the arguments in order and by
-- place, VARIADIC, the widths and the flag, %I and %L, and each message. A
-- refusal is what is tested, so an error goes on to the next statement.
\set ON_ERROR_STOP off
SELECT format(NULL) AS n, format('Hello') AS h, format('Hello %s', 'World') AS w, format('Hello %%') AS p1, format('Hello %%%%') AS p2, format('') AS e, format('%%s', 1) AS ps;
SELECT format('INSERT INTO %I VALUES(%L,%L)', 'mytab', 10, 'Hello') AS ins, format('%s%s%s', 'Hello', NULL, 'World') AS nul, format('INSERT INTO %I VALUES(%L,%L)', 'my tab', NULL, 'it''s') AS q, format('%I', 'select') AS kw, format('%L', E'a\\b') AS bs;
SELECT format('%1$s %3$s', 1, 2, 3) AS a, format('%1$s %12$s', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12) AS b, format('Hello %s %1$s %s', 'World', 'Hello again') AS c, format('Hello %s %s, %2$s %2$s', 'World', 'Hello again') AS d;
SELECT format('%s, %s', VARIADIC array['Hello', 'World']) AS v1, format('%2$s, %1$s', VARIADIC array[1, 2]) AS v2, format('%s, %s', VARIADIC array[true, false]) AS v3, format('Hello', VARIADIC NULL::int[]) AS v4;
SELECT format('>>%10s<<', 'Hello') AS r, format('>>%-10s<<', 'Hello') AS l, format('>>%10s<<', NULL) AS rn, format('>>%-10s<<', '') AS le, format('>>%1$10s<<', 'Hello') AS rp, format('>>%1$-10I<<', 'Hello') AS li, format('>>%--10s<<', 'x') AS mm, format('>>%-s<<', 'Hello') AS lw, format('>>%2s<<', 'Hello') AS cut;
SELECT format('>>%2$*1$L<<', 10, 'Hello') AS wl, format('>>%2$*1$L<<', 10, NULL) AS wn, format('>>%2$*1$L<<', -10, NULL) AS neg, format('>>%*s<<', 10, 'Hello') AS ws, format('>>%*1$s<<', 10, 'Hello') AS wp, format('>>%10L<<', NULL) AS ln, format('>>%2$*1$L<<', NULL, 'Hello') AS wnull, format('>>%2$*1$L<<', 0, 'Hello') AS w0, format('>>%*s<<', 3::int2, 'a') AS w2, format('>>%*s<<', '4'::text, 'a') AS wt, format('>>%*s<<', 5::int8, 'a') AS w8;
SELECT format('%5s|%-5s|', '日本', 'a😀') AS mb, format('%3$s %1$s', 'a', 'b', 'c', 'd') AS skip, format('%s %I %L', 1.5, 'Ab', 2::int8) AS num;
SELECT format('Hello %s %s', 'World');
SELECT format('Hello %s');
SELECT format('Hello %x', 20);
SELECT format('%é', 1);
SELECT format('INSERT INTO %I VALUES(%L,%L)', NULL, 10, 'Hello');
SELECT format('%1$s %4$s', 1, 2, 3);
SELECT format('%1$s %13$s', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
SELECT format('%0$s', 'Hello');
SELECT format('%*0$s', 'Hello');
SELECT format('%1$', 1);
SELECT format('%1$1', 1);
SELECT format('%', 1);
SELECT format('abc%-', 1);
SELECT format('%*', 1);
SELECT format('%*1$', 1);
SELECT format('%*1s', 1, 'x');
SELECT format('%2147483648s', 'x');
SELECT format('%*s', -2147483648, 'x');
SELECT format('%*s', 'ten'::text, 'x');
SELECT format('%*s', 3);
SELECT format('%1073741823s', 'x');
SELECT format('%s %', 'x');
