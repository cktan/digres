-- Verify LIKE, NOT LIKE, ILIKE and the ESCAPE clause, which ty does
-- (ty/DESIGN.md section 16), on text with multibyte characters.
-- 1. % and _ with 1-, 2-, 3- and 4-byte characters.
-- 2. Backslash as the default escape, and another escape character.
-- 3. A backslash at the end of the pattern: an error only if the match
--    gets to it.
-- 4. ILIKE, and LIKE on name and bytea.
-- (Errors are server.sh's: a run stops at its first.)
SELECT s, p, s LIKE p AS m, s NOT LIKE p AS nm FROM (VALUES ('abc', 'a%'), ('abc', '_b_'), ('abc', 'a_'), ('héllo', 'h_llo'), ('héllo', 'h__llo'), ('日本語', '_本_'), ('日本語', '%語'), ('日本語', '日__'), ('a😀b', 'a_b'), ('a😀b', 'a__b'), ('a😀b', '%😀%'), ('', '%'), ('', '_'), ('', ''), ('abc', '%%%'), ('abcabc', '%bc%bc'), ('aaaa', '%a%a%a%a'), ('aaaa', '%a%a%a%a%a'), ('a%c', 'a\%c'), ('abc', 'a\%c'), ('a_c', 'a\_c'), ('a\c', 'a\\c'), ('ABC', 'abc')) v(s, p);
SELECT s LIKE 'a#%c' ESCAPE '#' AS m1, s LIKE 'a#_c' ESCAPE '#' AS m2, s LIKE 'a##c' ESCAPE '#' AS m3, s LIKE 'a%c' ESCAPE '' AS m4, 'a\c' LIKE 'a\c' ESCAPE '' AS m5, 'aéc' LIKE 'aéé%' ESCAPE 'é' AS m6, 'a%c' LIKE 'aé%c' ESCAPE 'é' AS m7, 'a%c' LIKE 'a\%c' ESCAPE '\' AS m8 FROM (VALUES ('a%c'), ('a_c'), ('a#c'), ('abc')) v(s);
SELECT 'abc' LIKE 'x\' AS a, 'abc' LIKE 'abc\' AS b, '' LIKE '\' AS c, 'a' LIKE 'a\' AS d;
SELECT 'ABC' ILIKE 'a%' AS a, 'ABC' ILIKE '_b_' AS b, 'HÉLLO' ILIKE 'h_llo' AS c, 'abc' NOT ILIKE 'A%' AS d, 'Straße' ILIKE 'STRA%' AS e, 'ÄÖÜ' ILIKE 'äöü' AS f;
SELECT 'abc'::name LIKE 'a%' AS a, 'abc'::name NOT LIKE '_b_' AS b, 'abc'::bytea LIKE 'a%' AS c, 'abc'::bytea LIKE '_b_' AS d, '\xc3a9'::bytea LIKE '_' AS e, '\xc3a9'::bytea LIKE '__' AS f, 'abc'::bytea NOT LIKE 'x%' AS g, 'a%c'::bytea LIKE 'a#%c' ESCAPE '#'::bytea AS h;
SELECT s FROM (VALUES ('apple'), ('banana'), ('cherry'), ('日本語'), ('apricot'), ('Äpfel')) v(s) WHERE s LIKE 'ap%' OR s LIKE '%er%' OR s LIKE '日%' ORDER BY s;
SELECT count(*) FROM (SELECT 'item' || i s FROM generate_series(1, 2000) i) x WHERE s LIKE 'item1_%' AND s NOT LIKE '%9';
SELECT like_escape('a#%c', '#') AS a, like_escape('a\c', '') AS b, like_escape('a\c', '\') AS c, like_escape('aé%é', 'é') AS d, like_escape('##a#', '#') AS e;
