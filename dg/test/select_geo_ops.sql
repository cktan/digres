-- Verify point and box, whose operations ty does (ty/DESIGN.md section 20),
-- with the fuzzy compares, NaN and infinity.
-- 1. point: the compares, + - * /, distance and slope.
-- 2. box: construction, the position operators, area, width, height, center.
-- 3. box: intersection, distance, + - * /, and the order by area.
-- (Errors are server.sh's: a run stops at its first.)
SELECT p, q, p << q AS l, p >> q AS r, p >^ q AS a, p <^ q AS b, p ?- q AS h, p ?| q AS v, p ~= q AS eq, p <-> q AS dist, p + q AS pl, p - q AS mi, p * q AS mu, p / q AS dv, slope(p, q) AS sl FROM (VALUES ('(1,2)'::point, '(3,4)'::point), ('(1,2)', '(1,2.0000005)'), ('(1,2)', '(1.0000005,2)'), ('(0.5,0)', '(0,0.5)'), ('(-1.5,2.5)', '(2,-3)'), ('(1e10,1e10)', '(1,1)'), ('(0,0)', '(3,4)'), ('(1,1)', '(1,5)'), ('(NaN,1)', '(NaN,1)'), ('(1,2)', '(Infinity,1)'), ('(1,2)', '(1,2)')) v(p, q);
SELECT b, area(b) AS ar, width(b) AS w, height(b) AS h, center(b) AS c, diagonal(b) AS dg FROM (VALUES ('(2,2),(0,0)'::box), ('(0,0),(2,2)'), ('(1,1),(1,1)'), ('(-1,-1),(1,3)'), ('(0.5,0.5),(10,20)'), ('(3,4),(1,2)')) v(b);
SELECT a, b, a && b AS ov, a << b AS l, a &< b AS ol, a >> b AS r, a &> b AS orr, a <<| b AS bl, a &<| b AS obl, a |>> b AS ab, a |&> b AS oab, a @> b AS c, a <@ b AS cd, a ~= b AS same, a <-> b AS dist, a # b AS isect FROM (VALUES ('(2,2),(0,0)'::box, '(3,3),(1,1)'::box), ('(2,2),(0,0)', '(5,5),(3,3)'), ('(5,5),(0,0)', '(2,2),(1,1)'), ('(1,1),(0,0)', '(1,1),(0,0)'), ('(2,2),(0,0)', '(2.0000005,2),(0,0)'), ('(1,1),(0,0)', '(2,2),(1,1)')) v(a, b);
SELECT b, b + '(1,2)'::point AS pl, b - '(1,2)'::point AS mi, b * '(2,0)'::point AS mu, b / '(2,0)'::point AS dv, b * '(1,1)'::point AS mu2 FROM (VALUES ('(2,2),(0,0)'::box), ('(1,1),(-1,-1)'), ('(3,4),(1,2)')) v(b);
SELECT a, b, a < b AS lt, a = b AS eq, a > b AS gt, a <= b AS le, a >= b AS ge FROM (VALUES ('(2,2),(0,0)'::box, '(1,4),(0,0)'::box), ('(2,2),(0,0)', '(2,2),(0,0)'), ('(1,1),(0,0)', '(3,3),(0,0)'), ('(2,2),(0,0)', '(2.0000005,2),(0,0)')) v(a, b);
SELECT point(1, 2) AS p, box(point(1, 2), point(3, 4)) AS b, box(point(3, 4), point(1, 2)) AS b2, '(3,4),(1,2)'::box AS b3, bound_box('(1,2),(3,4)'::box, '(0,0),(2,2)'::box) AS bb;
SELECT count(*) FROM (SELECT point(i % 10, i % 7) p FROM generate_series(1, 500) i) x WHERE p << '(5,5)'::point AND p ~= '(2,2)'::point;
