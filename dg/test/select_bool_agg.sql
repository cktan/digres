-- Verify the aggregates of bool, which ty's state serves (ty_bool_agg_t): EVERY,
-- bool_and and bool_or, with nulls, with no row, and over a moving frame, which
-- takes a value out of the state (bool_accum_inv).
SELECT g, bool_and(b) AS band, bool_or(b) AS bor, every(b) AS every, count(b) AS n FROM (VALUES (1, true), (1, true), (2, true), (2, false), (3, false), (3, false), (4, NULL::bool), (4, true), (5, NULL::bool), (5, NULL::bool)) v(g, b) GROUP BY g ORDER BY g;
SELECT bool_and(b) AS band, bool_or(b) AS bor, every(b) AS every FROM (VALUES (true)) v(b) WHERE false;
SELECT i, b, bool_and(b) OVER w AS band, bool_or(b) OVER w AS bor, every(b) OVER w AS every FROM (VALUES (1, true), (2, true), (3, false), (4, true), (5, NULL::bool), (6, false), (7, false), (8, true)) v(i, b) WINDOW w AS (ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) ORDER BY i;
SELECT i, b, bool_and(b) OVER w AS band, bool_or(b) OVER w AS bor FROM (VALUES (1, true), (2, false), (3, true), (4, true), (5, true), (6, false)) v(i, b) WINDOW w AS (ORDER BY i ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING) ORDER BY i;
SELECT i, bool_and(b) OVER (ORDER BY i ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_and, bool_or(b) OVER (ORDER BY i ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_or FROM (VALUES (1, true), (2, true), (3, false), (4, true)) v(i, b) ORDER BY i;
SELECT bool_and(b) FILTER (WHERE i > 2) AS f_and, bool_or(b) FILTER (WHERE i > 2) AS f_or, bool_and(b) FILTER (WHERE i > 9) AS f_empty FROM (VALUES (1, false), (2, false), (3, true), (4, true)) v(i, b);
