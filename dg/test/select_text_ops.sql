-- Verify the text functions that ty does (ty/DESIGN.md section 16), on text
-- with 1-, 2-, 3- and 4-byte characters.
-- 1. length, octet_length, the order and the compares.
-- 2. substring, left, right, reverse, overlay.
-- 3. position, replace, split_part, starts_with.
-- 4. repeat, lpad, rpad, btrim, ltrim, rtrim, translate.
-- 5. ascii and chr.
-- (Errors are server.sh's: a run stops at its first.)
SELECT s, length(s) AS len, octet_length(s) AS octets, s < 'b' AS lt, s = 'abc' AS eq, s || 'x' AS cat FROM (VALUES (''), ('abc'), ('héllo'), ('日本語'), ('a😀b'), ('€uro'), ('Zz')) v(s);
SELECT s FROM (VALUES ('b'), ('a'), ('é'), ('z'), ('日'), (''), ('ab'), ('a😀'), ('€'), ('B')) v(s) ORDER BY s;
SELECT s, substring(s FROM 2) AS f2, substring(s FROM 2 FOR 2) AS f22, substring(s FROM 0 FOR 2) AS f02, substring(s FROM -1 FOR 3) AS fm13, substring(s FROM 10) AS f10, substr(s, 3, 1) AS s31, substring(s FROM 2 FOR 2147483647) AS fbig FROM (VALUES ('abcdef'), ('héllo wörld'), ('日本語テキスト'), ('a😀b😀c'), ('')) v(s);
SELECT s, left(s, 2) AS l2, left(s, -2) AS lm2, left(s, 0) AS l0, left(s, 99) AS l99, right(s, 2) AS r2, right(s, -2) AS rm2, right(s, 0) AS r0, right(s, 99) AS r99, reverse(s) AS rev FROM (VALUES ('abcdef'), ('héllo'), ('日本語'), ('a😀b'), ('')) v(s);
SELECT overlay(s PLACING 'XY' FROM 2 FOR 2) AS a, overlay(s PLACING 'XY' FROM 2) AS b, overlay(s PLACING 'é' FROM 1 FOR 0) AS c, overlay(s PLACING '😀' FROM 9 FOR 1) AS d, overlay(s PLACING 'Q' FROM 3 FOR -1) AS e FROM (VALUES ('abcdef'), ('héllo'), ('日本語'), ('')) v(s);
SELECT s, n, position(n IN s) AS pos, strpos(s, n) AS strp, s ^@ n AS sw, starts_with(s, n) AS sw2, replace(s, n, '--') AS repl, replace(s, n, '') AS del FROM (VALUES ('abcabc', 'bc'), ('héllo wörld', 'ö'), ('日本語日本語', '本語'), ('a😀b😀c', '😀'), ('abc', ''), ('', 'a'), ('aaaa', 'aa'), ('abc', 'abcd'), ('xxabcxx', 'abc'), ('abababab', 'abab')) v(s, n);
SELECT s, sep, split_part(s, sep, 1) AS p1, split_part(s, sep, 2) AS p2, split_part(s, sep, 3) AS p3, split_part(s, sep, -1) AS m1, split_part(s, sep, -2) AS m2, split_part(s, sep, -9) AS m9 FROM (VALUES ('a,b,c', ','), ('a,,c', ','), (',a,', ','), ('日本;語;テ', ';'), ('abc', ','), ('', ','), ('abc', ''), ('a😀b😀c', '😀'), ('aXXbXXc', 'XX')) v(s, sep);
SELECT repeat('ab', 3) AS a, repeat('é', 2) AS b, repeat('', 5) AS c, repeat('x', 0) AS d, repeat('日', -1) AS e, repeat('😀b', 2) AS f;
SELECT lpad(s, 7, '*') AS lp, rpad(s, 7, '*') AS rp, lpad(s, 3, 'xy') AS lp3, rpad(s, 3, 'xy') AS rp3, lpad(s, 9, 'é日') AS lpm, rpad(s, 9, 'é日') AS rpm, lpad(s, 9, '') AS lpe, rpad(s, 0, 'x') AS rp0, lpad(s, -1, 'x') AS lpn FROM (VALUES ('abc'), ('héllo'), ('日本語'), ('a😀b'), ('')) v(s);
SELECT btrim('  xx  ') AS a, ltrim('  xx  ') AS b, rtrim('  xx  ') AS c, btrim('xyxhixyx', 'xy') AS d, ltrim('xyxhixyx', 'xy') AS e, rtrim('xyxhixyx', 'xy') AS f, btrim('éé日hi日é', 'é日') AS g, rtrim('hi😀😀', '😀') AS h, ltrim('😀😀hi', '😀x') AS i, btrim('abc', '') AS j, btrim('', 'a') AS k, btrim('aaa', 'a') AS l;
SELECT translate('12345', '143', 'ax') AS a, translate('héllo', 'é', 'e') AS b, translate('日本語', '本', '😀') AS c, translate('abc', 'abc', '') AS d, translate('a😀b', '😀', 'xy') AS e, translate('', 'a', 'b') AS f, translate('hello', 'lo', '0') AS g;
SELECT ascii('') AS a, ascii('A') AS b, ascii('é') AS c, ascii('日') AS d, ascii('😀') AS e, ascii('abc') AS f, chr(65) AS g, chr(233) AS h, chr(26085) AS i, chr(128512) AS j, chr(127) AS k, chr(128) AS l, chr(2047) AS m, chr(2048) AS n, chr(65535) AS o, chr(65536) AS p, chr(1114111) AS q;
SELECT s, hashtext(s) AS h, hashtextextended(s, 1) AS h1 FROM (VALUES (''), ('abc'), ('héllo'), ('日本語')) v(s);
SELECT count(*), count(DISTINCT s) FROM (SELECT substr(repeat('日本語a', 3), (i % 10) + 1, 4) s FROM generate_series(1, 1000) i) x;
