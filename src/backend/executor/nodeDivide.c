/*-------------------------------------------------------------------------
 *
 * nodeDivide.c
 *	  Routines to handle relational division (R DIVIDE S).
 *
 * R (the outer child) is the dividend and S (the inner child) the divisor.
 * The divisor columns are R's columns matched to S by the match quals (the
 * generated USING equalities); every other column of R is a quotient
 * column.  The result is each distinct quotient value that is paired in R
 * with every row of S.
 *
 * Algorithm (all the work happens on the first call):
 *	1. Read all of S into memory, numbering its rows 0 .. |S|-1, and index
 *	   them by a hash of their divisor values.
 *	2. Read all of R and group its rows by quotient value in a
 *	   TupleHashTable.  Each group carries a bitmap of the S rows it has
 *	   matched, plus a count.  For each R row, probe the S index with the
 *	   hash of the row's divisor values and evaluate the match quals against
 *	   each candidate S row; mark each match in the group's bitmap.
 *	3. Return each group whose count equals |S|, projected from the group's
 *	   first R row.
 *
 * Cost is roughly |R| + |S| hash operations plus one qual evaluation per
 * (R row, hash-matching S row) pair.  If the match quals can't be hashed
 * (an operator without hash support), every S row goes into a single
 * bucket, which degrades to |R| * |S| qual evaluations but stays correct.
 *
 * Semantics:
 *	- Empty S: every distinct quotient value of R qualifies.
 *	- NULLs in divisor columns never match (SQL "=").  An S row with a NULL
 *	  divisor value can never be matched, so then no group qualifies; this
 *	  is the same answer as the double NOT EXISTS formulation of division.
 *	- NULLs in quotient columns group together, as in GROUP BY.
 *	- Duplicate rows in R or S don't change the result.
 *	- Everything is kept in memory; there is no spilling to disk yet, so
 *	  work_mem is not enforced.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeDivide.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecDivide		- return the next qualifying quotient value
 *		ExecInitDivide	- initialize the node and its children
 *		ExecEndDivide	- shut down the node and its children
 *		ExecReScanDivide - forget all results and start over
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeDivide.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"

/*
 * Data stored alongside each quotient group in the groups hash table.
 * The bitmap has one bit per S row and is allocated in tablecxt.
 */
typedef struct DivideGroupData
{
	int			nmatched;		/* number of S rows matched so far */
	bits8	   *matched;		/* bitmap over S rows */
} DivideGroupData;

#define DIVIDE_BIT_IS_SET(bits, i)	(((bits)[(i) / 8] & (1 << ((i) % 8))) != 0)
#define DIVIDE_SET_BIT(bits, i)		((bits)[(i) / 8] |= (1 << ((i) % 8)))

/* Which child's columns an expression references */
typedef struct DivideSideContext
{
	bool		has_outer;
	bool		has_inner;
	bool		has_other;
} DivideSideContext;

static bool
divide_side_walker(Node *node, DivideSideContext *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == OUTER_VAR)
			context->has_outer = true;
		else if (var->varno == INNER_VAR)
			context->has_inner = true;
		else
			context->has_other = true;
		return false;
	}
	return expression_tree_walker(node, divide_side_walker, context);
}

/* Is expr computed from only the given child (OUTER_VAR or INNER_VAR)? */
static bool
divide_expr_uses_only(Node *expr, int varno)
{
	DivideSideContext context = {false, false, false};

	divide_side_walker(expr, &context);
	if (context.has_other)
		return false;
	if (varno == OUTER_VAR)
		return context.has_outer && !context.has_inner;
	return context.has_inner && !context.has_outer;
}

/*
 * Split each match qual "x op y" into an R-side and an S-side key, and look
 * up hash functions compatible with op for each side.  If any qual doesn't
 * have that shape or op can't hash, set hashable = false: every S row then
 * goes into one bucket and the quals alone decide matches.
 */
static void
divide_init_keys(DivideState *state, Divide *node)
{
	List	   *quals = node->join.joinqual;
	int			nkeys = list_length(quals);
	int			i = 0;
	ListCell   *lc;

	state->nkeys = nkeys;
	state->hashable = (nkeys > 0);
	state->outerkeys = palloc0(sizeof(ExprState *) * Max(nkeys, 1));
	state->innerkeys = palloc0(sizeof(ExprState *) * Max(nkeys, 1));
	state->outerhashfns = palloc0(sizeof(FmgrInfo) * Max(nkeys, 1));
	state->innerhashfns = palloc0(sizeof(FmgrInfo) * Max(nkeys, 1));
	state->keycollations = palloc0(sizeof(Oid) * Max(nkeys, 1));

	foreach(lc, quals)
	{
		OpExpr	   *op;
		Expr	   *larg;
		Expr	   *rarg;
		Expr	   *outerexpr;
		Expr	   *innerexpr;
		Oid			lhashfn;
		Oid			rhashfn;
		Oid			outerhashfn;
		Oid			innerhashfn;

		if (!IsA(lfirst(lc), OpExpr) ||
			list_length(((OpExpr *) lfirst(lc))->args) != 2)
		{
			state->hashable = false;
			return;
		}
		op = (OpExpr *) lfirst(lc);
		larg = (Expr *) linitial(op->args);
		rarg = (Expr *) lsecond(op->args);

		if (!get_op_hash_functions(op->opno, &lhashfn, &rhashfn))
		{
			state->hashable = false;
			return;
		}

		/* The planner normally gives R on the left, but allow either way */
		if (divide_expr_uses_only((Node *) larg, OUTER_VAR) &&
			divide_expr_uses_only((Node *) rarg, INNER_VAR))
		{
			outerexpr = larg;
			innerexpr = rarg;
			outerhashfn = lhashfn;
			innerhashfn = rhashfn;
		}
		else if (divide_expr_uses_only((Node *) larg, INNER_VAR) &&
				 divide_expr_uses_only((Node *) rarg, OUTER_VAR))
		{
			outerexpr = rarg;
			innerexpr = larg;
			outerhashfn = rhashfn;
			innerhashfn = lhashfn;
		}
		else
		{
			state->hashable = false;
			return;
		}

		state->outerkeys[i] = ExecInitExpr(outerexpr, (PlanState *) state);
		state->innerkeys[i] = ExecInitExpr(innerexpr, (PlanState *) state);
		fmgr_info(outerhashfn, &state->outerhashfns[i]);
		fmgr_info(innerhashfn, &state->innerhashfns[i]);
		state->keycollations[i] = op->inputcollid;
		i++;
	}
}

/*
 * Hash the R-side (outer = true) or S-side key values of the row(s) set in
 * econtext.  Returns false if any key is NULL: under a strict "=" such a
 * row can't match anything.
 */
static bool
divide_hash_keys(DivideState *state, ExprContext *econtext, bool outer,
				 uint32 *hashvalue)
{
	uint32		hash = 0;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(state->tempcxt);
	for (int i = 0; i < state->nkeys; i++)
	{
		ExprState  *keyexpr = outer ? state->outerkeys[i] : state->innerkeys[i];
		FmgrInfo   *hashfn = outer ? &state->outerhashfns[i] : &state->innerhashfns[i];
		Datum		value;
		bool		isnull;

		value = ExecEvalExpr(keyexpr, econtext, &isnull);
		if (isnull)
		{
			MemoryContextSwitchTo(oldcxt);
			return false;
		}
		hash = pg_rotate_left32(hash, 1);
		hash ^= DatumGetUInt32(FunctionCall1Coll(hashfn,
												 state->keycollations[i],
												 value));
	}
	MemoryContextSwitchTo(oldcxt);

	*hashvalue = hash;
	return true;
}

/*
 * Step 1: read all of S, keep a copy of each row, and index the rows by the
 * hash of their divisor values.
 */
static void
divide_load_divisor(DivideState *state)
{
	PlanState  *innerPlan = innerPlanState(state);
	ExprContext *econtext = state->js.ps.ps_ExprContext;
	int			nalloc = 64;
	int			n = 0;
	bool	   *hashok;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(state->tablecxt);
	state->innertuples = palloc(sizeof(MinimalTuple) * nalloc);
	state->innerhashes = palloc(sizeof(uint32) * nalloc);
	hashok = palloc(sizeof(bool) * nalloc);
	MemoryContextSwitchTo(oldcxt);

	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(innerPlan);

		if (TupIsNull(slot))
			break;

		if (n >= nalloc)
		{
			nalloc *= 2;
			state->innertuples = repalloc(state->innertuples,
										  sizeof(MinimalTuple) * nalloc);
			state->innerhashes = repalloc(state->innerhashes,
										  sizeof(uint32) * nalloc);
			hashok = repalloc(hashok, sizeof(bool) * nalloc);
		}

		oldcxt = MemoryContextSwitchTo(state->tablecxt);
		state->innertuples[n] = ExecCopySlotMinimalTuple(slot);
		MemoryContextSwitchTo(oldcxt);

		if (state->hashable)
		{
			econtext->ecxt_innertuple = slot;
			hashok[n] = divide_hash_keys(state, econtext, false,
										 &state->innerhashes[n]);
			MemoryContextReset(state->tempcxt);
		}
		else
		{
			state->innerhashes[n] = 0;
			hashok[n] = true;
		}
		n++;
	}
	state->ninner = n;

	/*
	 * Chain the S rows into buckets.  A row with a NULL key (hashok false)
	 * is left out of every bucket: it can never be matched, but it still
	 * counts toward |S|, so no group can qualify.
	 */
	state->nbuckets = state->hashable ? (int) pg_nextpower2_32(Max(n, 1)) : 1;
	oldcxt = MemoryContextSwitchTo(state->tablecxt);
	state->bucketheads = palloc(sizeof(int) * state->nbuckets);
	state->bucketnext = palloc(sizeof(int) * Max(n, 1));
	MemoryContextSwitchTo(oldcxt);
	for (int b = 0; b < state->nbuckets; b++)
		state->bucketheads[b] = -1;
	for (int i = n - 1; i >= 0; i--)
	{
		int			b;

		state->bucketnext[i] = -1;
		if (!hashok[i])
			continue;
		b = state->innerhashes[i] & (state->nbuckets - 1);
		state->bucketnext[i] = state->bucketheads[b];
		state->bucketheads[b] = i;
	}
	pfree(hashok);
}

/*
 * Step 2: read all of R, grouping by quotient value and recording which S
 * rows each group matches.
 */
static void
divide_load_dividend(DivideState *state)
{
	PlanState  *outerPlan = outerPlanState(state);
	ExprContext *econtext = state->js.ps.ps_ExprContext;
	int			nbytes = Max((state->ninner + 7) / 8, 1);

	for (;;)
	{
		TupleTableSlot *outerslot = ExecProcNode(outerPlan);
		TupleHashEntry entry;
		DivideGroupData *group;
		bool		isnew;
		uint32		hash = 0;
		int			i;

		if (TupIsNull(outerslot))
			break;

		/* Find or create this row's quotient group */
		entry = LookupTupleHashEntry(state->groups, outerslot, &isnew, NULL);
		group = (DivideGroupData *) TupleHashEntryGetAdditional(state->groups,
																 entry);
		if (isnew)
		{
			group->nmatched = 0;
			group->matched = MemoryContextAllocZero(state->tablecxt, nbytes);
		}

		/* Nothing to learn if this group already matched every S row */
		if (group->nmatched < state->ninner)
		{
			econtext->ecxt_outertuple = outerslot;
			if (!state->hashable ||
				divide_hash_keys(state, econtext, true, &hash))
			{
				i = state->bucketheads[hash & (state->nbuckets - 1)];
				for (; i >= 0; i = state->bucketnext[i])
				{
					if (DIVIDE_BIT_IS_SET(group->matched, i))
						continue;
					if (state->hashable && state->innerhashes[i] != hash)
						continue;

					ExecStoreMinimalTuple(state->innertuples[i],
										  state->innerslot, false);
					econtext->ecxt_innertuple = state->innerslot;
					if (ExecQual(state->js.joinqual, econtext))
					{
						DIVIDE_SET_BIT(group->matched, i);
						group->nmatched++;
					}
					ResetExprContext(econtext);
				}
			}
		}

		ResetExprContext(econtext);
		MemoryContextReset(state->tempcxt);
	}
}

/* ----------------------------------------------------------------
 *		ExecDivide
 *
 *		Returns the next quotient value that matched every S row, or NULL
 *		when there are no more.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecDivide(PlanState *pstate)
{
	DivideState *node = castNode(DivideState, pstate);
	ExprContext *econtext = node->js.ps.ps_ExprContext;

	CHECK_FOR_INTERRUPTS();

	if (!node->built)
	{
		divide_load_divisor(node);
		divide_load_dividend(node);
		node->built = true;
		InitTupleHashIterator(node->groups, &node->groupiter);
	}

	for (;;)
	{
		TupleHashEntry entry;
		DivideGroupData *group;

		entry = ScanTupleHashTable(node->groups, &node->groupiter);
		if (entry == NULL)
			return NULL;

		group = (DivideGroupData *) TupleHashEntryGetAdditional(node->groups,
																 entry);
		if (group->nmatched < node->ninner)
			continue;			/* missed some S row */

		ResetExprContext(econtext);
		ExecStoreMinimalTuple(TupleHashEntryGetTuple(entry),
							  node->groupslot, false);
		econtext->ecxt_outertuple = node->groupslot;

		if (node->js.ps.qual == NULL || ExecQual(node->js.ps.qual, econtext))
			return ExecProject(node->js.ps.ps_ProjInfo);
	}
}

/* ----------------------------------------------------------------
 *		ExecInitDivide
 * ----------------------------------------------------------------
 */
DivideState *
ExecInitDivide(Divide *node, EState *estate, int eflags)
{
	DivideState *state;
	TupleDesc	outerDesc;
	TupleDesc	innerDesc;
	int			numQuotCols = node->numQuotCols;
	Oid		   *eqOperators;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	state = makeNode(DivideState);
	state->js.ps.plan = (Plan *) node;
	state->js.ps.state = estate;
	state->js.ps.ExecProcNode = ExecDivide;

	ExecAssignExprContext(estate, &state->js.ps);

	/* Each input is read once per (re)scan, so no rewind needed */
	outerPlanState(state) = ExecInitNode(outerPlan(node), estate,
										 eflags & ~EXEC_FLAG_REWIND);
	innerPlanState(state) = ExecInitNode(innerPlan(node), estate,
										 eflags & ~EXEC_FLAG_REWIND);
	outerDesc = ExecGetResultType(outerPlanState(state));
	innerDesc = ExecGetResultType(innerPlanState(state));

	/*
	 * Expressions here see rows of more than one slot type: R rows come
	 * straight from the outer child or, for output, from a stored
	 * MinimalTuple; S rows likewise.  So don't let expression compilation
	 * assume the children's slot types.
	 */
	state->js.ps.outeropsset = true;
	state->js.ps.outeropsfixed = false;
	state->js.ps.outerops = NULL;
	state->js.ps.inneropsset = true;
	state->js.ps.inneropsfixed = false;
	state->js.ps.innerops = NULL;

	/* Result slot and projection: output comes from a group's R row */
	ExecInitResultTupleSlotTL(&state->js.ps, &TTSOpsVirtual);
	ExecAssignProjectionInfo(&state->js.ps, NULL);

	state->js.ps.qual = ExecInitQual(node->join.plan.qual, (PlanState *) state);
	state->js.jointype = node->join.jointype;
	state->js.single_match = false;
	state->js.joinqual = ExecInitQual(node->join.joinqual, (PlanState *) state);

	state->groupslot = ExecInitExtraTupleSlot(estate, outerDesc,
											  &TTSOpsMinimalTuple);
	state->innerslot = ExecInitExtraTupleSlot(estate, innerDesc,
											  &TTSOpsMinimalTuple);

	divide_init_keys(state, node);

	/*
	 * Grouping by quotient value uses each column type's default hashable
	 * equality, as GROUP BY does (so NULLs group together).
	 */
	eqOperators = palloc(sizeof(Oid) * Max(numQuotCols, 1));
	state->quotCollations = palloc(sizeof(Oid) * Max(numQuotCols, 1));
	for (int i = 0; i < numQuotCols; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(outerDesc,
											   node->quotColIdx[i] - 1);
		TypeCacheEntry *typentry = lookup_type_cache(attr->atttypid,
													 TYPECACHE_EQ_OPR);

		if (!OidIsValid(typentry->eq_opr) ||
			!op_hashjoinable(typentry->eq_opr, attr->atttypid))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DIVIDE cannot group by quotient column of type %s",
							format_type_be(attr->atttypid)),
					 errdetail("Quotient column types must support hashing.")));
		eqOperators[i] = typentry->eq_opr;
		state->quotCollations[i] = attr->attcollation;
	}
	execTuplesHashPrepare(numQuotCols, eqOperators,
						  &state->quotEqFuncOids, &state->quotHashFunctions);

	state->tablecxt = AllocSetContextCreate(CurrentMemoryContext,
											"Divide hash table",
											ALLOCSET_DEFAULT_SIZES);
	state->tempcxt = AllocSetContextCreate(CurrentMemoryContext,
										   "Divide temporary context",
										   ALLOCSET_SMALL_SIZES);

	/*
	 * The groups table's metadata lives in the query context; its entries
	 * live in tablecxt, so a rescan just resets the table and tablecxt.
	 */
	state->groups = BuildTupleHashTable(&state->js.ps,
										outerDesc,
										NULL,
										numQuotCols,
										node->quotColIdx,
										state->quotEqFuncOids,
										state->quotHashFunctions,
										state->quotCollations,
										Max(node->join.plan.plan_rows, 16),
										sizeof(DivideGroupData),
										estate->es_query_cxt,
										state->tablecxt,
										state->tempcxt,
										false);

	state->built = false;

	return state;
}

/* ----------------------------------------------------------------
 *		ExecEndDivide
 * ----------------------------------------------------------------
 */
void
ExecEndDivide(DivideState *node)
{
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));

	MemoryContextDelete(node->tablecxt);
	MemoryContextDelete(node->tempcxt);
}

/* ----------------------------------------------------------------
 *		ExecReScanDivide
 *
 *		Forget everything and re-read both inputs on the next call.  The
 *		answer can change if a child depends on a changed parameter, so
 *		there is no reuse of earlier results.
 * ----------------------------------------------------------------
 */
void
ExecReScanDivide(DivideState *node)
{
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);

	if (node->built)
	{
		ResetTupleHashTable(node->groups);
		MemoryContextReset(node->tablecxt);
		node->built = false;
	}

	/* Children with changed params are rescanned by ExecProcNode */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
	if (innerPlan->chgParam == NULL)
		ExecReScan(innerPlan);
}
