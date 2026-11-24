-- Verify string_agg over text and bytea, whose state ty keeps
-- (ty_string_agg_add, _combine, _finish): NULL values and delimiters, the
-- first delimiter cut, ORDER BY and DISTINCT, groups, a window that calls the
-- final function again, and a state that grows. digres has no parallel query
-- and no partitioned table, so the combine, serialize and deserialize
-- functions are tested in ty alone (test_string_agg.cc).
SELECT string_agg(a, ',') AS c, string_agg(a, NULL) AS n FROM (VALUES ('aaaa'), (NULL), ('bbbb'), ('cccc')) g(a);
SELECT string_agg(a, 'AB') AS ab, string_agg(a, '') AS e FROM (VALUES (NULL), (NULL), ('bbbb'), ('cccc')) g(a);
SELECT string_agg(a, ',') IS NULL AS allnull FROM (VALUES (NULL::text), (NULL)) g(a);
SELECT string_agg(a, d) AS vary FROM (VALUES (1, '', NULL), (2, 'a', ','), (3, 'b', NULL), (4, 'c', '--')) g(i, a, d);
SELECT string_agg(a, d ORDER BY i) AS firstnull FROM (VALUES (1, 'x', NULL), (2, 'y', ';')) g(i, a, d);
SELECT string_agg(a, ',' ORDER BY a DESC) AS ord, string_agg(DISTINCT a, '|' ORDER BY a) AS dist FROM (VALUES ('b'), ('a'), ('c'), ('a')) g(a);
SELECT string_agg(v, '\x00'::bytea) AS b0, string_agg(v, NULL) AS bn, string_agg(v, '\xff'::bytea ORDER BY v DESC) AS bord FROM (VALUES ('\x01'::bytea), ('\x0203'), (NULL), (''::bytea)) g(v);
SELECT string_agg(v, ''::bytea) IS NULL AS ballnull FROM (VALUES (NULL::bytea)) g(v);
SELECT i % 3 AS k, string_agg(i::text, ',' ORDER BY i) AS t, string_agg(int4send(i), '\x2c'::bytea ORDER BY i) AS b FROM generate_series(1, 12) i GROUP BY k ORDER BY k;
SELECT i, string_agg(i::text, '-') OVER (ORDER BY i ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS w FROM generate_series(1, 5) i;
SELECT length(s) AS len, md5(s) AS md5, left(s, 20) AS head, right(s, 20) AS tail FROM (SELECT string_agg(i::text, ',' ORDER BY i) AS s FROM generate_series(1, 100000) i) t;
SELECT length(b) AS blen, md5(b) AS bmd5 FROM (SELECT string_agg(int8send(i), '\x00ff'::bytea ORDER BY i) AS b FROM generate_series(1, 50000) i) t;
