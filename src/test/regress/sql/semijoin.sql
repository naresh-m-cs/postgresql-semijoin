-- Explicit semijoins preserve left duplicates, not right matches.
CREATE TABLE semi_l (id int, label text);
CREATE TABLE semi_r (id int, active bool);
INSERT INTO semi_l VALUES (1, 'one'), (1, 'one'), (2, 'two'), (3, 'three'), (NULL, 'null');
INSERT INTO semi_r VALUES (1, true), (1, true), (2, false), (NULL, true);
SELECT * FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id ORDER BY l.id;
SELECT * FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id AND r.active ORDER BY l.id;

WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id = r.id))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id < r.id),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id < r.id))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id IS NOT DISTINCT FROM r.id),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id IS NOT DISTINCT FROM r.id))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id AND EXISTS (SELECT 1 FROM semi_r x WHERE x.id = r.id AND x.active)),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id = r.id AND EXISTS (SELECT 1 FROM semi_r x WHERE x.id = r.id AND x.active)))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id AND r.id IN (SELECT x.id FROM semi_r x WHERE x.active)),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id = r.id AND r.id IN (SELECT x.id FROM semi_r x WHERE x.active)))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
SET enable_hashjoin = off; SET enable_mergejoin = off;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id = r.id))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id < r.id),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id < r.id))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id IS NOT DISTINCT FROM r.id),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id IS NOT DISTINCT FROM r.id))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id AND EXISTS (SELECT 1 FROM semi_r x WHERE x.id = r.id AND x.active)),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id = r.id AND EXISTS (SELECT 1 FROM semi_r x WHERE x.id = r.id AND x.active)))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id AND r.id IN (SELECT x.id FROM semi_r x WHERE x.active)),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id = r.id AND r.id IN (SELECT x.id FROM semi_r x WHERE x.active)))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
SET enable_hashjoin = off; SET enable_mergejoin = on; SET enable_nestloop = off;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id = r.id))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id < r.id),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id < r.id))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id IS NOT DISTINCT FROM r.id),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id IS NOT DISTINCT FROM r.id))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id AND EXISTS (SELECT 1 FROM semi_r x WHERE x.id = r.id AND x.active)),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id = r.id AND EXISTS (SELECT 1 FROM semi_r x WHERE x.id = r.id AND x.active)))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id AND r.id IN (SELECT x.id FROM semi_r x WHERE x.active)),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id = r.id AND r.id IN (SELECT x.id FROM semi_r x WHERE x.active)))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
SET enable_hashjoin = on; SET enable_mergejoin = off; SET enable_nestloop = off;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id = r.id))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id < r.id),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id < r.id))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id IS NOT DISTINCT FROM r.id),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id IS NOT DISTINCT FROM r.id))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id AND EXISTS (SELECT 1 FROM semi_r x WHERE x.id = r.id AND x.active)),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id = r.id AND EXISTS (SELECT 1 FROM semi_r x WHERE x.id = r.id AND x.active)))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
WITH actual AS (SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id AND r.id IN (SELECT x.id FROM semi_r x WHERE x.active)),
expected AS (SELECT l.* FROM semi_l l WHERE EXISTS (SELECT 1 FROM semi_r r WHERE l.id = r.id AND r.id IN (SELECT x.id FROM semi_r x WHERE x.active)))
SELECT NOT EXISTS ((TABLE actual EXCEPT ALL TABLE expected)
                   UNION ALL (TABLE expected EXCEPT ALL TABLE actual)) AS matches_exists;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
SELECT * FROM semi_l l SEMI JOIN semi_r r ON false;
SELECT * FROM semi_l l SEMI JOIN (SELECT * FROM semi_r WHERE false) r ON l.id = r.id;
SELECT j.* FROM (semi_l l SEMI JOIN semi_r r ON l.id = r.id) j ORDER BY j.id;
SELECT l.* FROM semi_l l SEMI JOIN (semi_r r JOIN semi_r x ON r.id = x.id) ON l.id = r.id ORDER BY l.id;
SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id SEMI JOIN semi_r x ON l.id = x.id ORDER BY l.id;
SELECT l.* FROM semi_l l LEFT JOIN semi_r r ON l.id = r.id SEMI JOIN semi_r x ON l.id = x.id ORDER BY l.id;
SELECT l.* FROM semi_l l SEMI JOIN (VALUES (1), (2)) r(id) ON l.id = r.id ORDER BY l.id;
SELECT l.* FROM semi_l l SEMI JOIN semi_r r ON true ORDER BY l.id NULLS LAST;
-- Right-side names must not escape the join.
SELECT r.id FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id;
SELECT * FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id WHERE r.active;
SELECT r.* FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id;
-- Unsupported syntax must fail.
SELECT * FROM semi_l SEMI JOIN semi_r USING (id);
SELECT * FROM semi_l NATURAL SEMI JOIN semi_r;
-- Views must deparse and reparse, including after schema changes.
CREATE VIEW semi_v AS SELECT * FROM semi_l l SEMI JOIN semi_r r ON l.id = r.id;
CREATE VIEW semi_named AS SELECT j.* FROM (semi_l l SEMI JOIN semi_r r ON l.id = r.id) j;
SELECT pg_get_viewdef('semi_v'::regclass);
SELECT pg_get_viewdef('semi_named'::regclass);
ALTER TABLE semi_l ADD COLUMN extra int;
ALTER TABLE semi_r ADD COLUMN extra int;
SELECT pg_get_viewdef('semi_v'::regclass);
SELECT pg_get_viewdef('semi_named'::regclass);
DO $$ BEGIN
 EXECUTE 'CREATE VIEW semi_copy AS ' || pg_get_viewdef('semi_v'::regclass);
 EXECUTE 'CREATE VIEW semi_named_copy AS ' || pg_get_viewdef('semi_named'::regclass);
END $$;
SELECT * FROM semi_copy ORDER BY id;
SELECT * FROM semi_named_copy ORDER BY id;
DROP VIEW semi_copy, semi_named_copy, semi_v, semi_named;
DROP TABLE semi_l, semi_r;
