-- Verify circle, whose operations ty does (ty/DESIGN.md section 22).
-- 1. circle: area, diameter, radius, center, the conversions to and from box.
-- 2. circle against circle: position, contain, overlap, order, distance.
-- 3. circle against point: + - * /, contain, distance.
-- (Errors are server.sh's: a run stops at its first.)

SELECT a, area(a) AS ar, diameter(a) AS dia, radius(a) AS rad, @@ a AS ctr, box(a) AS bx
FROM (VALUES
  (0, '<(0,0),1>'::circle),
  (1, '<(1,1),2>'),
  (2, '<(0,0),0>'),
  (3, '<(5,5),1>'),
  (4, '<(2,0),3>'),
  (5, '<(-1,-1),0.5>'),
  (6, '<(0,0),1e-7>'),
  (7, '<(NaN,0),1>'),
  (8, '<(0,0),NaN>'),
  (9, '<(3,4),5>'),
  (10, '<(10,10),10>'),
  (11, '<(0,3),1>'),
  (12, '<(2.5,2.5),0.5>')) a(ia, a) ORDER BY ia;

SELECT a, circle(a) AS cr
FROM (VALUES
  (0, '(1,1),(0,0)'::box),
  (1, '(2,2),(1,1)'),
  (2, '(4,4),(-1,-1)'),
  (3, '(10,10),(5,5)'),
  (4, '(3,2),(0,0)'),
  (5, '(0,0),(0,0)'),
  (6, '(1,3),(1,3)'),
  (7, '(NaN,NaN),(NaN,NaN)')) a(ia, a) ORDER BY ia;

SELECT a, b, a ~= b AS same, a && b AS ov, a &< b AS ol, a << b AS l, a >> b AS r, a &> b AS or, a <@ b AS cd, a @> b AS c, a <<| b AS bl, a |>> b AS ab, a &<| b AS obl, a |&> b AS oab, a = b AS eq, a <> b AS ne, a < b AS lt, a > b AS gt, a <= b AS le, a >= b AS ge, a <-> b AS dist
FROM (VALUES
  (0, '<(0,0),1>'::circle),
  (1, '<(1,1),2>'),
  (2, '<(0,0),0>'),
  (3, '<(5,5),1>'),
  (4, '<(2,0),3>'),
  (5, '<(-1,-1),0.5>'),
  (6, '<(0,0),1e-7>'),
  (7, '<(NaN,0),1>'),
  (8, '<(0,0),NaN>'),
  (9, '<(3,4),5>'),
  (10, '<(10,10),10>'),
  (11, '<(0,3),1>'),
  (12, '<(2.5,2.5),0.5>')) a(ia, a) CROSS JOIN (VALUES
  (0, '<(0,0),1>'::circle),
  (1, '<(1,1),2>'),
  (2, '<(0,0),0>'),
  (3, '<(5,5),1>'),
  (4, '<(2,0),3>'),
  (5, '<(-1,-1),0.5>'),
  (6, '<(0,0),1e-7>'),
  (7, '<(NaN,0),1>'),
  (8, '<(0,0),NaN>'),
  (9, '<(3,4),5>'),
  (10, '<(10,10),10>'),
  (11, '<(0,3),1>'),
  (12, '<(2.5,2.5),0.5>')) b(ib, b) ORDER BY ia, ib;

SELECT a, b, a + b AS pl, a - b AS mi, a * b AS mu, a @> b AS cont, a <-> b AS dist, b <-> a AS dist2, b <@ a AS cd
FROM (VALUES
  (0, '<(0,0),1>'::circle),
  (1, '<(1,1),2>'),
  (2, '<(0,0),0>'),
  (3, '<(5,5),1>'),
  (4, '<(2,0),3>'),
  (5, '<(-1,-1),0.5>'),
  (6, '<(0,0),1e-7>'),
  (7, '<(NaN,0),1>'),
  (8, '<(0,0),NaN>'),
  (9, '<(3,4),5>'),
  (10, '<(10,10),10>'),
  (11, '<(0,3),1>'),
  (12, '<(2.5,2.5),0.5>')) a(ia, a) CROSS JOIN (VALUES
  (0, '(0,0)'::point),
  (1, '(1,1)'),
  (2, '(1,2)'),
  (3, '(-3,4)'),
  (4, '(2.5,-1.5)'),
  (5, '(10,10)'),
  (6, '(0,5)'),
  (7, '(5,0)'),
  (8, '(1e-7,0)'),
  (9, '(NaN,1)'),
  (10, '(3,3)'),
  (11, '(2,0)')) b(ib, b) ORDER BY ia, ib;

SELECT a, b, a / b AS dv
FROM (VALUES
  (0, '<(0,0),1>'::circle),
  (1, '<(1,1),2>'),
  (2, '<(0,0),0>'),
  (3, '<(5,5),1>'),
  (4, '<(2,0),3>'),
  (5, '<(-1,-1),0.5>'),
  (6, '<(0,0),1e-7>'),
  (7, '<(NaN,0),1>'),
  (8, '<(0,0),NaN>'),
  (9, '<(3,4),5>'),
  (10, '<(10,10),10>'),
  (11, '<(0,3),1>'),
  (12, '<(2.5,2.5),0.5>')) a(ia, a) CROSS JOIN (VALUES
  (0, '(0,0)'::point),
  (1, '(1,1)'),
  (2, '(1,2)'),
  (3, '(-3,4)'),
  (4, '(2.5,-1.5)'),
  (5, '(10,10)'),
  (6, '(0,5)'),
  (7, '(5,0)'),
  (8, '(1e-7,0)'),
  (9, '(NaN,1)'),
  (10, '(3,3)'),
  (11, '(2,0)')) b(ib, b)
WHERE NOT b ~= '(0,0)'::point ORDER BY ia, ib;
