-- Verify the order, min/max and interval arithmetic of
-- timetz and time, which ty does (ty/DESIGN.md section 19). timetz cannot be
-- stored yet, so the values come from VALUES.
-- (Errors are server.sh's: a run stops at its first.)
WITH tz(id, t, tm) AS (VALUES (1, '12:00+00'::timetz, '12:00'::time), (2, '12:00+01', '12:00:01'), (3, '12:00-05', '11:59:59.999999'), (4, '23:59:59.999999+14', '00:00'), (5, '00:00-12', '23:59:59'), (6, '07:00+00', '07:00:00.5'), (7, '12:00+00', '12:00'), (8, '12:00+01:30:15', '06:30'))
SELECT a.id, b.id, a.t < b.t AS lt, a.t = b.t AS eq, a.t > b.t AS gt, a.t <= b.t AS le, a.t >= b.t AS ge, a.t <> b.t AS ne FROM tz a, tz b ORDER BY 1, 2;
WITH tz(id, t, tm) AS (VALUES (1, '12:00+00'::timetz, '12:00'::time), (2, '12:00+01', '12:00:01'), (3, '12:00-05', '11:59:59.999999'), (4, '23:59:59.999999+14', '00:00'), (5, '00:00-12', '23:59:59'), (6, '07:00+00', '07:00:00.5'), (7, '12:00+00', '12:00'), (8, '12:00+01:30:15', '06:30'))
SELECT id, t::text FROM tz ORDER BY t, id;
WITH tz(id, t, tm) AS (VALUES (1, '12:00+00'::timetz, '12:00'::time), (2, '12:00+01', '12:00:01'), (3, '12:00-05', '11:59:59.999999'), (4, '23:59:59.999999+14', '00:00'), (5, '00:00-12', '23:59:59'), (6, '07:00+00', '07:00:00.5'), (7, '12:00+00', '12:00'), (8, '12:00+01:30:15', '06:30'))
SELECT min(t)::text, max(t)::text, count(DISTINCT t), count(DISTINCT tm) FROM tz;
WITH tz(id, t, tm) AS (VALUES (1, '12:00+00'::timetz, '12:00'::time), (2, '12:00+01', '12:00:01'), (3, '12:00-05', '11:59:59.999999'), (4, '23:59:59.999999+14', '00:00'), (5, '00:00-12', '23:59:59'), (6, '07:00+00', '07:00:00.5'), (7, '12:00+00', '12:00'), (8, '12:00+01:30:15', '06:30'))
SELECT t::text, count(*) FROM tz GROUP BY t ORDER BY 1;
WITH tz(id, t, tm) AS (VALUES (1, '12:00+00'::timetz, '12:00'::time), (2, '12:00+01', '12:00:01'), (3, '12:00-05', '11:59:59.999999'), (4, '23:59:59.999999+14', '00:00'), (5, '00:00-12', '23:59:59'), (6, '07:00+00', '07:00:00.5'), (7, '12:00+00', '12:00'), (8, '12:00+01:30:15', '06:30'))
SELECT id, (t + interval '5 hours 30 minutes')::text AS p, (t - interval '13 hours')::text AS m, (t + interval '1 day 36 hours')::text AS d, (tm + interval '5 hours')::text AS tp, (tm - interval '13 hours')::text AS tm_ FROM tz ORDER BY id;
SELECT t::text, t::timetz(0)::text AS p0, t::timetz(1)::text AS p1 FROM (VALUES ('12:00:00.56+01'::timetz), ('23:59:59.96-05'), ('00:00:00.04+00')) x(t);
