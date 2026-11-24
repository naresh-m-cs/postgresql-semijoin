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
