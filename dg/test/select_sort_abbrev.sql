-- Verify that sorts by abbreviated keys, which ty makes and judges
-- (ty_abbrev.h), give the order of the values: text and bytea in the C
-- collation, char(n) without its trailing spaces, values longer than 128
-- bytes, uuid, text in an ICU collation, whose keys come from strxfrm, and
-- sorts that abort their keys. Each result is a digest of
-- the sorted values, which the digres before the move prints the same.
SELECT md5(string_agg(t, ',')) FROM (SELECT md5(i::text) AS t FROM generate_series(1, 200000) i ORDER BY 1) s;
SELECT md5(string_agg(t, ',')) FROM (SELECT 'same prefix ' || (i % 50000) AS t FROM generate_series(1, 200000) i ORDER BY 1) s;
SELECT md5(string_agg(u::text, ',')) FROM (SELECT md5(i::text)::uuid AS u FROM generate_series(1, 200000) i ORDER BY 1) s;
SELECT md5(string_agg(u::text, ',')) FROM (SELECT ('00000000-0000-0000-0000-' || lpad((i % 9)::text, 12, '0'))::uuid AS u FROM generate_series(1, 30000) i ORDER BY 1) s;
SELECT md5(string_agg(b, '\x2c'::bytea)) FROM (SELECT decode(md5(i::text), 'hex') AS b FROM generate_series(1, 200000) i ORDER BY 1) s;
SELECT md5(string_agg(b, '\x2c'::bytea)) FROM (SELECT '\x00000000'::bytea || int4send(i % 7) AS b FROM generate_series(1, 200000) i ORDER BY 1) s;
SELECT md5(string_agg(t, ',')) FROM (SELECT substr(md5(i::text), 1, (i % 12)) AS t FROM generate_series(1, 50000) i ORDER BY 1 DESC) s;
SELECT md5(string_agg(t::text, ',')) FROM (SELECT (substr(md5(i::text), 1, (i % 12)))::char(10) AS t FROM generate_series(1, 50000) i ORDER BY 1) s;
SELECT md5(string_agg(t, ',')) FROM (SELECT repeat('x', 130 + i % 3) || i AS t FROM generate_series(1, 20000) i ORDER BY 1) s;
SELECT md5(string_agg(t, ',')) FROM (SELECT md5((i / 3)::text) COLLATE "unicode" AS t FROM generate_series(1, 60000) i ORDER BY 1) s;
SELECT md5(string_agg(t, ',')) FROM (SELECT ('Same Prefix ' || (i % 500)) COLLATE "unicode" AS t FROM generate_series(1, 30000) i ORDER BY 1) s;
