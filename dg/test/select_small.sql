-- Verify tid, pg_lsn and "char", which ty does (ty/DESIGN.md section 15).
-- 1. tid: text in and out, the order, min and max, the hash, a sort.
-- 2. pg_lsn: text in and out, the order, + and - with numbers, the hash.
-- 3. "char": text in and out with octal escapes, the order as unsigned, the
--    casts with int4 and text.
-- (Errors are server.sh's: a run stops at its first.)
SELECT '(0,1)'::tid AS a, '(4294967295,65535)'::tid AS b, '(-1,1)'::tid AS c, '(12,34)'::tid AS d, '(1,2)'::tid < '(1,3)'::tid AS lt, '(2,1)'::tid > '(1,9)'::tid AS gt, '(1,2)'::tid = '(1,2)'::tid AS eq, '(1,2)'::tid <> '(1,2)'::tid AS ne, bttidcmp('(1,2)', '(1,3)') AS cmp, bttidcmp('(2,0)', '(1,9)') AS cmp2;
SELECT t FROM (VALUES ('(1,2)'::tid), ('(0,5)'), ('(1,1)'), ('(4294967295,1)'), ('(10,0)'), ('(2,65535)')) v(t) ORDER BY t;
SELECT max(t) AS mx, min(t) AS mn FROM (VALUES ('(1,2)'::tid), ('(0,5)'), ('(10,0)'), ('(2,65535)')) v(t);
SELECT hashtid('(1,2)') = hashtid('(1,2)') AS same, hashtid('(1,2)') = hashtid('(2,1)') AS diff, hashtid('(1,2)') AS h, hashtidextended('(1,2)', 5) AS h5;
SELECT count(*) FROM (SELECT ('(' || (i % 100) || ',' || (i % 7) || ')')::tid a FROM generate_series(1, 2000) i) x JOIN (SELECT ('(' || (i % 50) || ',' || (i % 7) || ')')::tid b FROM generate_series(1, 300) i) y ON a = b;
SELECT '0/0'::pg_lsn AS a, 'FFFFFFFF/FFFFFFFF'::pg_lsn AS b, '1/2'::pg_lsn AS c, 'a/B'::pg_lsn AS d, '16/B374D848'::pg_lsn AS e, '0/ff'::pg_lsn AS f, 'AB/CD'::pg_lsn < 'AB/CE'::pg_lsn AS lt, '1/0'::pg_lsn > '0/FFFFFFFF'::pg_lsn AS gt, '1/2'::pg_lsn = '1/2'::pg_lsn AS eq;
SELECT l FROM (VALUES ('1/2'::pg_lsn), ('0/5'), ('10/0'), ('FFFFFFFF/0'), ('0/FFFFFFFF'), ('1/1')) v(l) ORDER BY l;
SELECT '1/10'::pg_lsn + 16 AS a, '1/10'::pg_lsn - 16 AS b, '1/10'::pg_lsn - '0/10'::pg_lsn AS c, '0/10'::pg_lsn - '1/10'::pg_lsn AS d, '1/10'::pg_lsn + 0.0 AS e, '0/10'::pg_lsn - 16 AS f, max(l) AS mx, min(l) AS mn FROM (VALUES ('1/2'::pg_lsn), ('0/5'), ('10/0')) v(l);
SELECT 'a'::"char" AS a, ''::"char" AS b, 'ab'::"char" AS c, '\101'::"char" AS d, '\377'::"char" AS e, '\200'::"char" AS f, '\000'::"char" AS g, 'é'::"char" AS h, '7'::"char" AS i, '\134'::"char" AS j;
SELECT 'a'::"char" < 'b'::"char" AS lt, '\377'::"char" > 'a'::"char" AS gt, '\200'::"char" > '\177'::"char" AS gt2, 'a'::"char" = 'a'::"char" AS eq, 'a'::"char" <= 'a'::"char" AS le, 'a'::"char" >= 'b'::"char" AS ge, 'a'::"char" <> 'b'::"char" AS ne;
SELECT c FROM (VALUES ('a'::"char"), ('\377'), ('A'), ('\200'), ('z'), ('0'), ('\001')) v(c) ORDER BY c;
SELECT 'a'::"char"::int4 AS a, '\377'::"char"::int4 AS b, '\200'::"char"::int4 AS c, 65::"char" AS d, (-1)::"char" AS e, (-128)::"char" AS f, 127::"char" AS g, 'abc'::text::"char" AS h, ''::text::"char" AS i, '\101'::text::"char" AS j, 'a'::"char"::text AS k, '\377'::"char"::text AS l, length('\000'::"char"::text) AS m;
SELECT count(*), count(DISTINCT c) FROM (SELECT (i % 200)::"char" c FROM generate_series(1, 1000) i WHERE i % 200 < 128) x;
