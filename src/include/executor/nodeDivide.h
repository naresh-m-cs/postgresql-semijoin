/*-------------------------------------------------------------------------
 *
 * nodeDivide.h
 *	  prototypes for nodeDivide.c
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeDivide.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEDIVIDE_H
#define NODEDIVIDE_H

#include "nodes/execnodes.h"

extern DivideState *ExecInitDivide(Divide *node, EState *estate, int eflags);
extern void ExecEndDivide(DivideState *node);
extern void ExecReScanDivide(DivideState *node);

#endif							/* NODEDIVIDE_H */
