-- Verify character(n), character varying(n) and name, which ty does
-- (ty/DESIGN.md section 16), with multibyte characters.
-- 1. Cast to character(n) and varchar(n): pad, cut, and spaces that are cut.
-- 2. length, octet_length, the compares and the hash of character(n):
--    trailing spaces do not count.
-- 3. name: cut to 63 bytes at a character, the order, the cast with text.
-- (Errors are server.sh's: a run stops at its first.)
SELECT s::char(5) AS c5, s::varchar(5) AS v5, s::char(2) AS c2x, length(s::char(8)) AS len8, octet_length(s::char(8)) AS oct8, s::char(8)::text AS txt8 FROM (VALUES ('ab'), ('héllo'), ('日本'), ('a😀b'), (''), ('abcde'), ('ab   ')) v(s);
SELECT 'abcdefgh'::varchar(5) AS a, 'abc   '::char(3) AS b, 'abc   '::varchar(3) AS c, 'héllo   '::char(5) AS d, '日本語   '::varchar(3) AS e, 'abc'::char(5) AS f, '日本'::char(4) AS g, ('abcdef'::text)::char(3) AS h, ('abcdef'::text)::varchar(3) AS i, 'xyz'::char AS j, 'xyz '::char(1) AS k;
SELECT 'ab'::char(5) = 'ab'::char(7) AS eq, 'ab'::char(5) < 'abc'::char(2) AS lt, 'ab  '::char(4) <> 'ab'::char(2) AS ne, 'b'::char(3) > 'abc'::char(3) AS gt, bpcharcmp('ab '::char(3), 'ab'::char(2)) AS c1, bpcharcmp('a'::char(3), 'b'::char(3)) AS c2, hashbpchar('ab'::char(5)) = hashbpchar('ab'::char(2)) AS hsame, hashbpchar('ab'::char(5)) AS h, hashbpcharextended('é '::char(3), 3) AS hx;
SELECT s FROM (VALUES ('b'::char(3)), ('a'), ('ab'), ('é'), ('a '), ('B'), ('')) v(s) ORDER BY s;
SELECT s, s ~<~ 'b'::char(2) AS pat_lt, bpchar_larger(s, 'b'::char(2)) AS larger FROM (VALUES ('a'::char(3)), ('b'), ('c ')) v(s);
SELECT count(*) FROM (SELECT (i % 100)::char(6) a FROM generate_series(1, 2000) i) x JOIN (SELECT (i % 50)::char(3) b FROM generate_series(1, 300) i) y ON a = b;
SELECT 'abc'::name AS a, repeat('x', 70)::name AS long, length(repeat('日', 30)::name::text) AS cjk, length(repeat('é', 40)::name::text) AS e40, 'b'::name < 'a'::name AS lt, 'abc'::name = 'abc'::name AS eq, 'abc'::name::text AS t, 'abc  '::char(5)::name AS cn, ''::name AS empty, repeat('😀', 20)::name::text AS emoji;
SELECT n FROM (VALUES ('b'::name), ('a'), ('ab'), ('é'), ('B'), ('')) v(n) ORDER BY n;
SELECT current_user::name = session_user::name AS same, 'x'::name || 'y' AS cat;
