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
  - Placeholder for now: `elog(DEBUG1, ...)` only, so we can confirm a divide reaches analysis. To see it, run `SET client_min_messages = debug1;`.
  - No `markRelsAsNulledBy`: divide never pads rows with NULLs.
  - TODO: divisor columns, output columns (R minus divisor), hide S outside the divide.
