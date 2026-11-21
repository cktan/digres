-- Verify lower, upper and initcap in the C ctype, which ty does
-- (ty/DESIGN.md section 16): only ASCII letters change.
SELECT s, lower(s) AS lo, upper(s) AS up, initcap(s) AS ic FROM (VALUES (''), ('Hello World'), ('hELLO wORLD'), ('héllo wörld'), ('ÀÉÎ ñ'), ('日本語 abc'), ('a😀B'), ('o''neil mc-donald'), ('foo_bar baz1qux 2nd'), ('ALL CAPS'), ('already Title Case'), ('x') ) v(s);
