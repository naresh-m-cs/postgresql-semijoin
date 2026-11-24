# DIVIDE operator — change log & reasoning

Syntax: `R DIVIDE S [USING (cols)]` → native executor node (`JOIN_DIVIDE`).

## 1. Parsing

### `src/include/parser/kwlist.h`
- Added `PG_KEYWORD("divide", DIVIDE, TYPE_FUNC_NAME_KEYWORD, BARE_LABEL)` so the lexer recognizes `divide` as a keyword.
- Category `TYPE_FUNC_NAME_KEYWORD` (same as `SEMI`, `LEFT`, `NATURAL`):
  - `divide` can still be used as a function/type name, and as an output label (`AS divide`).
  - It cannot be an unquoted column/table name or alias. Otherwise `FROM r divide s` would be ambiguous (alias vs. operator). Use `"divide"` to quote it.
- `BARE_LABEL`: allows `SELECT x divide` (label without `AS`).
- The list must stay alphabetically sorted.

### `src/backend/parser/gram.y`
- Added `DIVIDE` to the `%token <keyword>` list (alphabetical).
  - Declares `DIVIDE` as a token (terminal symbol) the grammar can use in rules.
  - The lexer returns this token when it finds `divide` in `kwlist.h`.
  - `<keyword>` = its value is the keyword's text (`const char *`), so rules like `ColLabel` can use the word itself as a name.
- Added `DIVIDE` to the `type_func_name_keyword` list (alphabetical).
  - Must match the category in `kwlist.h`; `check_keywords.pl` fails the build otherwise.
  - Puts `divide` into the grammar rules that accept function/type names (`type_function_name`) and labels after `AS` (`ColLabel`).
  - Keeps it out of `ColId` (column/table names, aliases), avoiding the `FROM r divide s` ambiguity.
- Added `DIVIDE` to the `bare_label_keyword` list (alphabetical).
  - Matches `BARE_LABEL` in `kwlist.h`; `check_keywords.pl` checks this too.
  - Allows `SELECT x divide FROM ...` (output label without `AS`).
- Added `DIVIDE` to the `%left JOIN CROSS LEFT FULL RIGHT INNER_P NATURAL SEMI` line.
  - Same precedence as the other join operators, and left-associative like all SQL joins.
  - `a JOIN b DIVIDE c` = `(a JOIN b) DIVIDE c`.
  - Division isn't associative, so `R ÷ S ÷ T` = `(R ÷ S) ÷ T`. Parentheses override.
  - Without this, Bison reports shift/reduce conflicts.
- Added two `joined_table` rules:
  - `table_ref DIVIDE table_ref`: divisor columns matched by name (like `NATURAL`).
  - `table_ref DIVIDE table_ref USING (cols)`: divisor columns listed explicitly.
  - No `ON` form: an arbitrary condition has no clear meaning for division.
  - Both build a `JoinExpr` (`larg` = R, `rarg` = S), reusing the existing join node.
    - By-name form: `isNatural = true`, like `NATURAL JOIN`.
    - `USING` form: the column list goes in `usingClause`.
  - Both set `jointype = JOIN_DIVIDE` (see `nodes.h`), which marks the node as a divide for every later stage.
- Added `%nonassoc USING` after the join precedence line.
  - Fixes one shift/reduce conflict: in `a JOIN b DIVIDE c USING (x)`, `USING` could belong to `JOIN` or to `DIVIDE`.
  - Higher precedence makes Bison shift, binding `USING` to the nearest `DIVIDE`. To give `USING` to the `JOIN` instead, write `a JOIN (b DIVIDE c) USING (x)`.
  - Safe: the grammar had 0 conflicts before, so this precedence only affects the new one.

### `src/include/nodes/nodes.h`
- Added `JOIN_DIVIDE` to the `JoinType` enum.
  - Reusing `JoinExpr` means no new parse node: `larg`, `rarg`, `isNatural`, and `usingClause` already fit.
  - Every later stage (analysis, planner, executor) identifies a divide by this value in `switch (jointype)`.
  - Added at the end so existing values keep their numbers (stored view definitions save them).
  - Not added to `IS_OUTER_JOIN`: divide never pads rows with NULLs.
  - Risk: existing `switch (jointype)` statements with `default: elog(ERROR, ...)` need a `JOIN_DIVIDE` case wherever a divide reaches them.

## 2. Analysis

### `src/backend/parser/parse_clause.c` (`transformFromClauseItem`)
- Added `case JOIN_DIVIDE` to the join-type switch, which previously hit `default: elog(ERROR, "unrecognized join type")`.
  - Fetches left (R) and right (S) column names from `l_colnames` / `r_colnames`.
    - These are filled in earlier in the function for every join type, from each side's namespace item (`p_names->colnames`).
    - They work for any input (table, subquery, function, nested join), unlike digging into `larg`/`rarg` directly.
    - Dropped columns appear as empty strings, so they're skipped.
  - For now each name is printed with `elog(DEBUG1, ...)` to confirm the columns are right. Run `SET client_min_messages = debug1;` to see them.
  - No `markRelsAsNulledBy`: divide never pads rows with NULLs.
  - TODO: divisor columns, output columns (R minus divisor), hide S outside the divide.
- No separate natural vs. `USING` handling: `j->usingClause` holds the divisor columns for both forms.
  - Earlier in the function, the `isNatural` block already converts `R DIVIDE S` into a `usingClause` of shared column names.
  - Existing code after it also checks that each `USING` column exists in both inputs, for both forms.
- Error if `j->usingClause` is empty: "DIVIDE requires at least one divisor column".
  - Only the natural form can hit this (no shared column names); the grammar requires at least one name in `USING (...)`.
  - Dividing by zero columns has no meaning.
- No new code needed to check that `USING` columns exist: the shared `USING` loop (runs before the join-type switch) already raises errors for a column missing from the left or right input, listed twice, or ambiguous in one input.
- Count the quotient columns (R's columns minus dropped and divisor ones).
  - "Divisor" is checked by position in `l_colnos` (filled by the `USING` loop), not by "does S have this name". In the `USING` form, R may share an unlisted column with S, and that column still belongs to the quotient.
  - Only a count is kept. The quotient columns themselves end up in the join's output-column list (see below), so separate `q_colnames` / `q_colnos` lists were redundant and removed.
  - Error if zero: dividing when every R column is a divisor leaves nothing to return.
- Hide S outside the divide, same as SEMI JOIN, in two places:
  - Output columns: S's remaining (non-`USING`) columns are not added to the join's result columns (`extractRemainingColumns` is skipped for the right side). Hides them from `SELECT *` and unqualified names.
  - Namespace: `my_namespace = l_namespace`, so `s.col` gives "missing FROM-clause entry". Hides qualified names.
  - Why: an output row is one quotient value, not paired with any single S row, so S's columns have no value to return.
  - Still to do: the merged `USING` (divisor) columns remain in the output, and R's own divisor columns (`r.b`) are still visible. The output-column step must reduce the output to the quotient columns only.
- Output = quotient columns only. The divisor (`USING`) columns are kept out of the output in two places:
  - `USING` loop: the column name is not added to `res_colnames` for DIVIDE.
  - Merged-column block (`if (j->usingClause ...)`): skipped for DIVIDE, so no output value is built for divisor columns. `buildMergedJoinVar` would also raise "unrecognized join type" for DIVIDE.
  - What remains: `extractRemainingColumns` on the left side adds R's non-`USING`, non-dropped columns, which are exactly the quotient columns.
  - The generated `USING` condition (`r.b = s.b`) is still built and kept in `j->quals`, so the executor knows how R's and S's divisor columns match.
- Hide R's name too: `my_namespace = NIL` for DIVIDE (replaces the SEMI-style `my_namespace = l_namespace`).
  - Why: `r.b` (a divisor column) has no single value per quotient row. Left visible, `SELECT r.b ...` would fail later with an internal planner error or return an arbitrary value.
  - PostgreSQL can hide a whole table name but not a single column, so `r.a` goes too. Per-column hiding would need changes to shared lookup code; not worth it.
  - Quotient columns stay reachable: unqualified (`a`) through the join's own entry, or `q.a` with `(r DIVIDE s) AS q`. Same rule as an aliased join.

## 3. Planner

Key idea: the divide's quals (`r.b = s.b`, from `USING`) are **matching rules**, not filters. They must stay at the divide: never pushed into R or S, never merged into EquivalenceClasses (ECs, the planner's sets of values known to be equal). The divide is also a fixed unit: nothing may be reordered into or out of it, and R/S never swap.

### Part 1: `src/backend/optimizer/prep/prepjointree.c` (query-tree simplification)
- `pull_up_sublinks_jointree_recurse`: `JOIN_DIVIDE` does nothing. Its quals are only the generated equalities, so there are no sublinks to pull up.
- `pull_up_subqueries_recurse`: does **not** recurse into a divide's inputs; subqueries there stay subquery scans.
  - Why (bug found by the regression test): the quotient is grouped by R's output column values. Flattening `(SELECT a % 2 AS q, b FROM t)` made the divide group by `a` instead of `q`, which gave wrong answers.
- `reduce_outer_joins_pass2`: no simplification (a divide is never outer). It passes **no** constraints to its children, like FULL. Removing rows from either input changes which quotient values qualify: fewer divisor rows means an easier test, and with an empty divisor every R value qualifies.
- `remove_useless_results_recurse`: no special handling (grouped with FULL/ANTI).
- `get_relids_in_jointree`: counts as inner (like SEMI), because a divide nulls nothing.
- `get_nullingrels_recurse`: grouped with INNER/SEMI, because a divide nulls nothing.

### Part 2: `src/backend/optimizer/plan/initsplan.c` (registering the divide)
- `deconstruct_recurse`, new `case JOIN_DIVIDE`:
  - R stays in the parent join domain. S gets its own child domain, like a LEFT JOIN's right side, and the divide's quals belong to that child domain.
  - Why (bug found by the regression test): with S in the parent domain, a constant-false qual inside S (`WHERE false`) was lifted above the divide and emptied the whole result. An empty S should instead make every quotient value qualify.
  - `j->rtindex = 0`, like explicit SEMI: no NULL padding, and alias Vars are already flattened, so it needs no outer-join identity.
  - `nonnullable_rels = qualscope` (both sides). Effect in `distribute_qual_to_rels`: the quals are `is_pushed_down = false`, not ECs, and evaluated exactly at the divide's level. They also skip the left/right/full outer-join clause lists, so no constant gets derived into S.
- Joinlist: DIVIDE forces its own subproblem, like FULL, so no outside rel is joined into R or S before dividing.
- `make_outerjoininfo`:
  - Same as FULL: `min_lefthand` / `min_righthand` = whole sides, so nothing is reordered inside the divide.
  - Joins above a divide treat it as a barrier, like FULL, but without `ojrelid` (it's 0).
  - `FOR UPDATE/SHARE` on either input is an error: a quotient row stands for many R rows.
- `deconstruct_distribute` needed no change: a non-SEMI join already gets `ojscope = min_lefthand ∪ min_righthand`.

### Keeping all quotient columns (`planner.c`, `initsplan.c`, `pathnodes.h`)
- Problem: the planner normally drops columns nothing above uses. For `r(a, c, b) DIVIDE s(b)` with `SELECT a`, it would drop `c`. But the quotient is grouped by **all** non-divisor columns (`a`, `c`), so dropping `c` gives wrong answers.
- `subquery_planner` (planner.c) discards each join's column-to-Var mapping (`joinaliasvars`). Just before that, for each DIVIDE join it saves the base-rel Vars behind the quotient columns in two new `PlannerInfo` fields: `divide_rtindexes` and `divide_quotvars`.
- `deconstruct_recurse` (DIVIDE case) marks those Vars as needed at the divide (`add_vars_to_targetlist`), so every quotient column reaches it. The divide's own output still contains only the columns used above.

### Part 3: paths and cost
- `pathnodes.h`: new `DividePath` (just a `JoinPath`: outer = R, inner = S, `joinrestrictinfo` = match quals).
- `joinrels.c` `populate_joinrel_with_paths`: `JOIN_DIVIDE` case.
  - Only exact R (rel1) ÷ S (rel2), never swapped.
  - Empty R gives an empty result. Empty S doesn't (every R value qualifies), so only R is checked.
- `joinpath.c` `add_paths_to_joinrel`: for DIVIDE, build one path from R's and S's cheapest unparameterized paths and return. Nestloop/merge/hash don't apply.
- `pathnode.c` `create_divide_path`: fills the path, which is unordered, not parallel, and unparameterized.
- `costsize.c`:
  - Row estimate: `|R| / |S|`. An R group needs at least |S| rows to qualify. Join selectivity is skipped because the quals aren't filters (and `eqjoinsel` would raise "unrecognized join type").
  - `cost_divide`: both inputs are startup cost (must read all of S and R before any quotient is known). Plus one step per input row, the quals once per R row, and a per-output-row charge.
- `relnode.c`: a divide is never partitionwise.

### Part 4: plan node
- `plannodes.h`: new `Divide` node = `Join` plus `numQuotCols` / `quotColIdx` (positions of quotient columns in R's output). The executor groups by these.
- `createplan.c` `create_divide_plan`:
  - R is built with an exact column list.
  - Quotient columns = R's output columns not used by the match quals.
  - `joinqual` = match quals.
- `setrefs.c`: `T_Divide` uses `set_join_references`, which rewrites Vars to OUTER (R) / INNER (S).
- `subselect.c` `finalize_plan`: `T_Divide` handles params in `joinqual`.
- `explain.c`: shows `Divide` with `Divide Cond`. It only works once the executor exists, because EXPLAIN also initializes the executor.
- `gen_node_support.pl`: the stable-branch ABI check's last nodetag number goes 479 → 481 (two new node types). Extensions built for stock 18 must be rebuilt.

### Verified (2026-11-24)
- `SELECT * FROM r DIVIDE s` reaches the executor: `ERROR: unrecognized node type: 358` (358 = `T_Divide`, no executor node yet).
- Plan (via `debug_print_plan`): `Divide` → outer SeqScan r (a, b), inner SeqScan s (b), `joinqual` r.b = s.b, output `a`, `quotColIdx (1)`.
- `r3(a, c, b) DIVIDE s`, `SELECT a`: `quotColIdx (1 2)`, so `c` is kept.
- `s.b` / `r.a` after the divide: "invalid reference to FROM-clause entry". `q.a` via an alias works.

## 4. Executor

### Decisions
| Question | Decision | Why |
|---|---|---|
| Empty S | Every distinct quotient value of R qualifies | Textbook definition (vacuous "for all") |
| NULL in a divisor column | Never matches (SQL `=`). A NULL in S means no group qualifies; NULL `b` rows in R are ignored | Same answer as the standard double `NOT EXISTS` formulation |
| NULL in a quotient column | NULLs group together | Same as `GROUP BY` |
| Duplicates in R or S | Don't change the result; output is distinct quotient values | Relational (set) semantics |
| Memory | Everything in memory; no spill to disk, `work_mem` not enforced | First version; spilling is future work |
| Non-hashable quotient type | Error: "DIVIDE cannot group by quotient column of type …" | Grouping uses a hash table |
| Non-hashable divisor operator (e.g. `box =`) | Still works, slower: all S rows in one bucket, `|R|·|S|` qual checks | Correctness over speed |

### Algorithm (`src/backend/executor/nodeDivide.c`), all done on the first call
1. **Read all of S.** Copy each row, number them 0…|S|−1, and index them in a chained hash table on their divisor values.
   - Rows with a NULL divisor value go in no bucket. They can never match, but they still count toward |S|.
2. **Read all of R.** Group rows by quotient value in a `TupleHashTable` (the same machinery `HashAgg`/`SetOp` use).
   - Each group stores `nmatched` and a bitmap with one bit per S row.
   - For each R row: hash its divisor values, walk that bucket, evaluate the real match quals on each candidate S row, and set the bit if it matches.
   - Skip the probing once a group has matched every S row.
3. **Return** each group with `nmatched == |S|`, projected from the group's first R row.
- Cost: about |R| + |S| hash steps plus one qual check per hash-matching pair.
- The match quals decide every match, so cross-type `USING` (int4 vs int8) works. Hash functions come from the operator's hash opfamily (`get_op_hash_functions`), as in hash join.

### Files
- `nodeDivide.c` / `nodeDivide.h`: `ExecInitDivide`, `ExecDivide`, `ExecEndDivide`, `ExecReScanDivide`.
- `execnodes.h`: `DivideState` (key expressions and hash functions, S rows and buckets, groups table, iterator, two memory contexts).
- `execProcnode.c` (init/end), `execAmi.c` (rescan), `meson.build` / `Makefile`.
- `gen_node_support.pl`: nodetag ABI check 481 → 482 (`DivideState`).

### Implementation notes
- **Slot types:** expressions see R/S rows both straight from the children and as stored `MinimalTuple` copies. So `outeropsfixed` / `inneropsfixed` = false, otherwise an assertion fails (`op->d.fetch.kind == slot->tts_ops`). This was the first crash during testing.
- **Memory:**
  - `tablecxt` holds the S rows, group tuples and bitmaps; it's reset on rescan.
  - `tempcxt` is per-R-row scratch for hashing.
  - The groups table's metadata lives in the query context, so rescan is `ResetTupleHashTable` plus a reset of `tablecxt`.
- **Rescan:** always forget everything and re-read both inputs. Children whose parameters changed are rescanned automatically.
- **Children** are started without `EXEC_FLAG_REWIND`, because each is read once per scan.

## 5. Deparse (views, `pg_get_viewdef`, `pg_dump`)
- **Crash found during testing:** `pg_get_viewdef` on a DIVIDE view hit `Assert(jcolno == numjoincols)` in `ruleutils.c`.
  - Cause: the join RTE described the divisor columns as merged output columns (`joinmergedcols = n`), but the real output is only the quotient columns.
- **Fix in analysis (`parse_clause.c`):**
  - The DIVIDE join RTE now has `joinmergedcols = 0`.
  - `joinleftcols` = quotient positions (one per output column) followed by the divisor positions in R.
  - `joinrightcols` = the divisor positions in S.
- **Second problem:** the deparser printed `SELECT dr.a ...`, but `dr` is hidden after a divide, so the text couldn't be reparsed.
  - Fix in analysis: the quotient columns' syntactic source (`varnosyn`) is the divide join, as is already done for aliased joins.
  - Fix in `ruleutils.c` `get_variable`: an unnamed DIVIDE's columns print unqualified (`a`).
- **`ruleutils.c`:**
  - `deparse_columns` gets `ndivcols` / `divleftattnos` / `divrightattnos`.
  - `identify_join_columns` splits `joinleftcols` for DIVIDE.
  - `set_using_names` picks the `USING` names and pushes them to both children (no output column).
  - `set_join_column_names` skips R's divisor columns and all of S.
  - `get_from_clause_item` prints `DIVIDE`.
- **Always printed with `USING (...)`,** even when written as `R DIVIDE S`. The column list is fixed when the view is created, as with `NATURAL JOIN`.
- **Known limitation:** an unqualified quotient column could become ambiguous if a column with the same name is later added to another table in the same `FROM`. Aliasing the divide (`(r DIVIDE s) AS q`) avoids this.

## Test results (2026-11-24, after executor)
- `SELECT * FROM r DIVIDE s` → `a = 1`.
  - `EXPLAIN` shows `Divide` / `Divide Cond: (r.b = s.b)`.
  - `EXPLAIN ANALYZE` shows actual rows (1 output; 3 from r, 2 from s).
- Edge cases (all as expected):

| Case | Result |
|---|---|
| Duplicates + NULL quotient group | `1, 3, 4, NULL`; matches double `NOT EXISTS` |
| Empty S | all distinct `a` incl. NULL |
| Empty R | no rows |
| NULL in S | no rows |
| Two divisor + two quotient columns, select subset | `(1,x), (2,y)`; `SELECT a` → `1, 2` |
| `USING` with unlisted shared column | `c` kept as quotient column |
| Cross-type `USING` (int4 vs int8) | correct |
| Non-hashable divisor (`box =`) | correct via fallback |
| Non-hashable quotient (`box`) | clear error |
| Quotient with no Vars (constant from pulled-up subquery) | correct |
| Rescan (divisor depends on outer value) | correct per row |
| Inside join + aggregate | correct |

- **Randomized:** 40 trials (2000 R rows, 1–4 S rows, ~3% NULLs) compared with double `NOT EXISTS`: 0 mismatches.
- **Views:**
  - `pg_get_viewdef` gives valid SQL.
  - Renaming a divisor column in both tables still deparses correctly.
  - Recreating 4 views from their deparsed text gives identical results.
  - `pg_dump` → restore into a new database → same results.
- **Regression suite** (`meson test --suite setup --suite regress`): all pass.

## 6. Regression test
- `src/test/regress/sql/divide.sql` + `expected/divide.out`, run as `test: divide` in `parallel_schedule` (before `tablespace`, which must stay last).
- 10 tests:
  1. Both syntax forms + `EXPLAIN`
  2. Matches double `NOT EXISTS`
  3. Empty S
  4. Empty R
  5. NULL in S
  6. Several divisor/quotient columns, unused quotient column still groups
  7. Cross-type divisor + computed quotient column (`a % 2`)
  8. Errors: no divisor, hidden inputs, bad `USING` column, non-hashable quotient
  9. Rescan with correlated divisor
  10. View deparse
- Writing it found two planner bugs, both fixed (see Planner parts 1 and 2):
  - Empty S via `WHERE false` returned no rows.
  - A computed quotient column grouped by the wrong value.
- After the fixes: the whole suite passes, and 40 more randomized trials with computed quotient columns and subquery inputs give 0 mismatches.

## Still to do
- Docs: `DIVIDE` in `doc/src/sgml/ref/select.sgml`.
- Spill to disk / honor `work_mem` for large inputs.
