--
-- DIVIDE (relational division)
--

CREATE TABLE div_r (a int, b int);
CREATE TABLE div_s (b int);
INSERT INTO div_r VALUES (1,10), (1,20), (1,20), (2,10), (3,10), (3,20), (3,30),
  (NULL,10), (NULL,20), (4,NULL), (4,10), (4,20);
INSERT INTO div_s VALUES (10), (20), (20);

-- 1. Basic division, both syntax forms, and the plan shape
SELECT * FROM div_r DIVIDE div_s ORDER BY a;
SELECT * FROM div_r DIVIDE div_s USING (b) ORDER BY a;
EXPLAIN (COSTS OFF) SELECT * FROM div_r DIVIDE div_s;

-- 2. Same answer as the double NOT EXISTS formulation
WITH actual AS (SELECT * FROM div_r DIVIDE div_s),
expected AS (SELECT DISTINCT a FROM div_r x WHERE NOT EXISTS (
  SELECT 1 FROM div_s WHERE NOT EXISTS (
    SELECT 1 FROM div_r y WHERE y.a IS NOT DISTINCT FROM x.a AND y.b = div_s.b)))
SELECT NOT EXISTS ((TABLE actual EXCEPT TABLE expected)
                   UNION ALL (TABLE expected EXCEPT TABLE actual)) AS matches;

-- 3. Empty divisor: every distinct quotient value qualifies
SELECT * FROM div_r DIVIDE (SELECT b FROM div_s WHERE false) t ORDER BY a;

-- 4. Empty dividend: no rows
SELECT * FROM (SELECT a, b FROM div_r WHERE false) t DIVIDE div_s;

-- 5. NULL in the divisor can never be matched: no rows
SELECT * FROM div_r DIVIDE (SELECT b FROM div_s UNION ALL SELECT NULL) t;

-- 6. Several divisor and quotient columns; unused quotient columns still group
CREATE TABLE div_r2 (a int, c text, b1 int, b2 int);
CREATE TABLE div_s2 (b1 int, b2 int);
INSERT INTO div_r2 VALUES (1,'x',1,1), (1,'x',2,2), (1,'y',1,1),
  (2,'y',1,1), (2,'y',2,2), (2,'z',2,2);
INSERT INTO div_s2 VALUES (1,1), (2,2);
SELECT * FROM div_r2 DIVIDE div_s2 ORDER BY a, c;
SELECT a FROM div_r2 DIVIDE div_s2 USING (b1, b2) ORDER BY a;

-- 7. Cross-type divisor column (int4 vs int8), and a computed quotient
-- column: groups by q, not by the underlying a (a=1 and a=3 both give q=1)
SELECT * FROM div_r DIVIDE (SELECT b::int8 AS b FROM div_s) t ORDER BY a;
SELECT * FROM (SELECT a % 2 AS q, b FROM (VALUES (1,10), (3,20)) v(a, b)) t
  DIVIDE (VALUES (10), (20)) s(b);
-- Non-hashable divisor operator (box =): all S rows go in one bucket,
-- and the match condition alone decides each match
SELECT * FROM (VALUES (1, box(point(0,0), point(1,1))),
                      (2, box(point(0,0), point(2,2)))) r(a, b)
  DIVIDE (VALUES (box(point(0,0), point(1,1)))) s(b) ORDER BY a;

-- 8. Errors
SELECT * FROM div_r DIVIDE (SELECT 1 AS z) t;           -- no divisor column
SELECT div_s.b FROM div_r DIVIDE div_s;                 -- inputs are hidden
SELECT * FROM div_r DIVIDE div_s USING (zzz);           -- bad USING column
-- non-hashable quotient column
SELECT * FROM (SELECT point(a, a)::box AS q, b FROM div_r) t DIVIDE div_s;

-- 9. Rescan: the divisor depends on an outer value
SELECT m, ARRAY(SELECT a FROM div_r DIVIDE (SELECT b FROM div_s WHERE b <= m) t
                ORDER BY a) AS quotient
FROM (VALUES (5), (10), (20)) v(m) ORDER BY m;

-- 10. Views deparse to valid SQL
CREATE VIEW div_v AS SELECT * FROM div_r DIVIDE div_s;
CREATE VIEW div_v2 AS SELECT q.a FROM (div_r2 DIVIDE div_s2) AS q;
SELECT pg_get_viewdef('div_v');
SELECT pg_get_viewdef('div_v2');
SELECT * FROM div_v ORDER BY a;

DROP VIEW div_v, div_v2;
DROP TABLE div_r, div_s, div_r2, div_s2;
