/*-------------------------------------------------------------------------
 *
 * heapam_handler.c
 *	  heap table access method code
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam_handler.c
 *
 *
 * NOTES
 *	  This files wires up the lower level heapam.c et al routines with the
 *	  tableam abstraction.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/multixact.h"
#include "access/rewriteheap.h"
#include "access/syncscan.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#ifdef DIVA
#include "storage/pleaf.h"
#include "storage/ebi_tree_buf.h"
#include "storage/ebi_sub_buf.h"
#include "storage/pleaf_bufpage.h"
#include "access/ebiam.h"
#include "storage/itemid.h"
#endif /* DIVA */

#ifdef LOCATOR
#include "pg_refcnt.h"
#include "locator/locator_executor.h"
#include "locator/locator_external_catalog.h"
#endif /* LOCATOR */

static void reform_and_rewrite_tuple(HeapTuple tuple,
									 Relation OldHeap, Relation NewHeap,
									 Datum *values, bool *isnull, RewriteState rwstate);

static bool SampleHeapTupleVisible(TableScanDesc scan, Buffer buffer,
								   HeapTuple tuple,
								   OffsetNumber tupoffset);

static BlockNumber heapam_scan_get_blocks_done(HeapScanDesc hscan);

static const TableAmRoutine heapam_methods;


/* ------------------------------------------------------------------------
 * Slot related callbacks for heap AM
 * ------------------------------------------------------------------------
 */

static const TupleTableSlotOps *
heapam_slot_callbacks(Relation relation)
{
	return &TTSOpsBufferHeapTuple;
}


/* ------------------------------------------------------------------------
 * Index Scan Callbacks for heap AM
 * ------------------------------------------------------------------------
 */

static IndexFetchTableData *
heapam_index_fetch_begin(Relation rel)
{
	IndexFetchHeapData *hscan = palloc0(sizeof(IndexFetchHeapData));

	hscan->xs_base.rel = rel;
	hscan->xs_cbuf = InvalidBuffer;
#ifdef DIVA
	hscan->xs_c_ebi_buf_id = InvalidEbiSubBuf;
	hscan->xs_vistuple_free = NULL;
	hscan->xs_vistuple_size = 0;
#endif /* DIVA */

	return &hscan->xs_base;
}

static void
heapam_index_fetch_reset(IndexFetchTableData *scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;

	if (BufferIsValid(hscan->xs_cbuf))
	{
		ReleaseBuffer(hscan->xs_cbuf);
		hscan->xs_cbuf = InvalidBuffer;
	}

#ifdef DIVA
	if (hscan->xs_c_ebi_buf_id != InvalidEbiSubBuf)
	{
		UnpinEbiSubBuffer(hscan->xs_c_ebi_buf_id);
		hscan->xs_c_ebi_buf_id = InvalidEbiSubBuf;
	}
#endif /* DIVA */
}

static void
heapam_index_fetch_end(IndexFetchTableData *scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;

	heapam_index_fetch_reset(scan);

#ifdef DIVA
	if (hscan->xs_vistuple_free)
		pfree(hscan->xs_vistuple_free);
#endif /* DIVA */
	pfree(hscan);
}

static bool
heapam_index_fetch_tuple(struct IndexFetchTableData *scan,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot,
						 bool *call_again, bool *all_dead)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	bool		got_heap_tuple;

#ifdef DIVA
	bool		siro = IsSiro(hscan->xs_base.rel);
#ifdef LOCATOR
	DualRefDescData	drefDesc;

	Assert(!IsLocator(hscan->xs_base.rel));
#endif /* LOCATOR */

	if (hscan->xs_c_ebi_buf_id != InvalidEbiSubBuf)
	{
		UnpinEbiSubBuffer(hscan->xs_c_ebi_buf_id);
		hscan->xs_c_ebi_buf_id = InvalidEbiSubBuf;
	}
#endif

	Assert(TTS_IS_BUFFERTUPLE(slot));

	/* We can skip the buffer-switching logic if we're in mid-HOT chain. */
	if (!*call_again)
	{
		/* Switch to correct buffer if we don't have it already */
		Buffer		prev_buf = hscan->xs_cbuf;

		hscan->xs_cbuf = ReleaseAndReadBuffer(hscan->xs_cbuf,
											  hscan->xs_base.rel,
											  ItemPointerGetBlockNumber(tid));

		/*
		 * Prune page, but only if we weren't already on this page
		 */
		if (prev_buf != hscan->xs_cbuf)
			heap_page_prune_opt(hscan->xs_base.rel, hscan->xs_cbuf);
	}

#ifdef LOCATOR
#ifdef USING_LOCK
	drefDesc.dual_ref = NULL;
#else
	if (siro)
	{
		/* Get dual_ref for avoiding race with heap_insert() */
		drefDesc.dual_ref = (pg_atomic_uint64*)GetBufferDualRef(hscan->xs_cbuf);
		SetPageRefUnit(&drefDesc);
	}
	else
#endif
		LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_SHARE);
#else
	/* Obtain share-lock on the buffer so we can examine visibility */
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_SHARE);
#endif /* LOCATOR */

#ifdef DIVA
	if (siro)
	{

#ifdef LOCATOR
		got_heap_tuple =
				 heap_hot_search_buffer_with_vc(tid,
												hscan->xs_base.rel,
												hscan->xs_cbuf,
												snapshot,
												&bslot->base.tupdata,
												&bslot->base.copied_tuple,
												&drefDesc,
												all_dead,
												!*call_again,
												hscan);
#else /* !LOCATOR */
		got_heap_tuple =
				 heap_hot_search_buffer_with_vc(tid,
												hscan->xs_base.rel,
												hscan->xs_cbuf,
												snapshot,
												&bslot->base.tupdata,
												&bslot->base.copied_tuple,
												all_dead,
												!*call_again,
												hscan);
#endif /* !LOCATOR*/
	}
	else
	{
		got_heap_tuple = heap_hot_search_buffer(tid,
												hscan->xs_base.rel,
												hscan->xs_cbuf,
												snapshot,
												&bslot->base.tupdata,
												all_dead,
												!*call_again);
	}
#else
	got_heap_tuple = heap_hot_search_buffer(tid,
											hscan->xs_base.rel,
											hscan->xs_cbuf,
											snapshot,
											&bslot->base.tupdata,
											all_dead,
											!*call_again);
#endif

	bslot->base.tupdata.t_self = *tid;
#ifdef LOCATOR
#ifndef USING_LOCK
	if (siro)
	{
		/* Decrease ref_cnt */
		DRefDecrRefCnt(drefDesc.dual_ref, drefDesc.page_ref_unit);
	}
	else
#endif
		LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);
#else
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);
#endif /* LOCATOR */

	if (got_heap_tuple)
	{
		/*
		 * Only in a non-MVCC snapshot can more than one member of the HOT
		 * chain be visible.
		 */
		*call_again = !IsMVCCSnapshot(snapshot);

		slot->tts_tableOid = RelationGetRelid(scan->rel);

#ifdef DIVA
		if (siro)
		{
			ExecStoreBufferHeapTuple(bslot->base.copied_tuple, slot, InvalidBuffer);
			bslot->base.copied_tuple = NULL;
		}
		else
			ExecStoreBufferHeapTuple(
					&bslot->base.tupdata, slot, hscan->xs_cbuf);
#else
		ExecStoreBufferHeapTuple(&bslot->base.tupdata, slot, hscan->xs_cbuf);
#endif
	}
	else
	{
		/* We've reached the end of the HOT chain. */
		*call_again = false;
	}

	return got_heap_tuple;
}

#ifdef LOCATOR
static bool
locator_index_fetch_tuple(IndexFetchHeapData *hscan,
						  ItemPointer tid,
						  LocatorTuplePosition tuplePosition,
						  Snapshot snapshot,
						  TupleTableSlot *slot,
						  bool *call_again)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	LocatorExecutor *executor = slot->tts_locator_executor;
	LocatorExecutorLevelDesc *level_desc
		= locator_get_level_columnar_desc(executor,
										  tuplePosition->partitionLevel);
	bool is_modification = (curr_cmdtype == CMD_UPDATE ||
							curr_cmdtype == CMD_DELETE);
	bool found = false;

#ifdef LOCATOR_DEBUG
	Assert(level_desc != NULL);
#endif /* LOCATOR_DEBUG */

	/*
	 * Set t_self to p-locator.
	 */
	bslot->base.tupdata.t_self = *tid;
	
	found = locator_search_and_deform_version(executor->rel,
											  level_desc,
											  slot,
											  snapshot,
											  tuplePosition,
											  hscan,
											  is_modification);

	if (found)
	{
		/*
		 * Only in a non-MVCC snapshot can more than one member of the HOT
		 * chain be visible.
		 */
		*call_again = !IsMVCCSnapshot(snapshot);
	}
	else
	{
		/* We've reached the end of the version */
		*call_again = false;
	}

	return found;
}
#endif /* LOCATOR */

#ifdef LOCATOR
/*
 * locatoram_index_fetch_tuple
 */
static bool
locatoram_index_fetch_tuple(struct IndexFetchTableData *scan,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot,
						 bool *call_again, bool *all_dead)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	LocatorRouteSynopsis route_synopsis = slot->tts_locator_route_synopsis;
	LocatorExternalCatalog *exCatalog;
	bool got_partition_tuple;
	DualRefDescData	drefDesc;
	LocatorTuplePositionData tuplePosition;
	Relation relation;
	LocatorExecutor *locator_executor = slot->tts_locator_executor;

	Assert(route_synopsis);
	Assert(IsSiro(hscan->xs_base.rel) && IsLocator(hscan->xs_base.rel));

	/* Release opened ebi buffer */
	if (hscan->xs_c_ebi_buf_id != InvalidEbiSubBuf)
	{
		UnpinEbiSubBuffer(hscan->xs_c_ebi_buf_id);
		hscan->xs_c_ebi_buf_id = InvalidEbiSubBuf;
	}

	Assert(TTS_IS_BUFFERTUPLE(slot));

	relation = hscan->xs_base.rel;

	/* Get external catalog */
	exCatalog = LocatorGetExternalCatalog(relation->rd_node.relNode);

	/* Get a tuple position information from route synopsis */
	LocatorRouteSynopsisGetTuplePositionForIndexscan(exCatalog, hscan->xs_base.rel, 
													 route_synopsis, &tuplePosition);

	if (locator_is_columnar_layout(locator_executor,
								   tuplePosition.partitionLevel))
		return locator_index_fetch_tuple(hscan, tid, &tuplePosition,
										 snapshot, slot, call_again);

	/* We can skip the buffer-switching logic if we're in mid-HOT chain. */
	if (!*call_again)
	{
		/* Switch to correct buffer if we don't have it already */
		//Buffer		prev_buf = hscan->xs_cbuf;
		BlockNumber	targetBlockNumber = 
			tuplePosition.partitionTuplePosition / relation->records_per_block;

		hscan->xs_cbuf = LocatorReleaseAndReadBuffer(hscan->xs_cbuf,
													 hscan->xs_base.rel,
													 tuplePosition.partitionLevel,
													 tuplePosition.partitionNumber,
													 tuplePosition.partitionGenerationNumber,
													 targetBlockNumber);

		/*
		 * Prune page, but only if we weren't already on this page
		 */
		/*
		if (prev_buf != hscan->xs_cbuf)
			heap_page_prune_opt(hscan->xs_base.rel, hscan->xs_cbuf);
		*/
	}

#ifdef USING_LOCK
	drefDesc.dual_ref = NULL;
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_SHARE);
#else
	/* Get dual_ref for avoiding race with heap_insert() */
	drefDesc.dual_ref = (pg_atomic_uint64*)GetBufferDualRef(hscan->xs_cbuf);
	SetPageRefUnit(&drefDesc);
#endif

	/* 
	 * Retrieve a tuple using the tuple position obtained through the route
	 * synopsis. 
	 */
	got_partition_tuple = locator_search_version(&tuplePosition,
												 hscan->xs_base.rel,
												 hscan->xs_cbuf,
												 snapshot,
												 &bslot->base.tupdata,
												 &bslot->base.copied_tuple,
												 &drefDesc,
												 all_dead,
												 !*call_again,
												 hscan,
												 InvalidBlockNumber,
												 InvalidOffsetNumber);

	bslot->base.tupdata.t_self = *tid;

#ifdef USING_LOCK
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);
#else
	/* Decrease ref_cnt */
	DRefDecrRefCnt(drefDesc.dual_ref, drefDesc.page_ref_unit);
#endif

	if (got_partition_tuple)
	{
		/*
		 * Only in a non-MVCC snapshot can more than one member of the HOT
		 * chain be visible.
		 */
		*call_again = !IsMVCCSnapshot(snapshot);

		slot->tts_tableOid = RelationGetRelid(scan->rel);

		ExecStoreBufferHeapTuple(bslot->base.copied_tuple, slot, InvalidBuffer);
		bslot->base.copied_tuple = NULL;
	}
	else
	{
		/* We've reached the end of the HOT chain. */
		*call_again = false;
	}

	return got_partition_tuple;
}
#endif


/* ------------------------------------------------------------------------
 * Callbacks for non-modifying operations on individual tuples for heap AM
 * ------------------------------------------------------------------------
 */

static bool
heapam_fetch_row_version(Relation relation,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	Buffer		buffer;

	Assert(TTS_IS_BUFFERTUPLE(slot));
#ifdef DIVA
	Assert(!IsSiro(relation));
#endif /* DIVA */

	bslot->base.tupdata.t_self = *tid;
	if (heap_fetch(relation, snapshot, &bslot->base.tupdata, &buffer, false))
	{
		/* store in slot, transferring existing pin */
		ExecStorePinnedBufferHeapTuple(&bslot->base.tupdata, slot, buffer);
		slot->tts_tableOid = RelationGetRelid(relation);

		return true;
	}

	return false;
}

#ifdef DIVA
/*
 * Search the visible version with tid of p-locator. (similar with index search)
 * Referenced heapam_index_fetch_tuple() & heapam_fetch_fow_version
 */
static bool
heapam_search_row_version(Relation relation,
						  ItemPointer tid,
						  Snapshot snapshot,
						  TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	bool		got_heap_tuple;
	bool		all_dead;

	BlockNumber block;
	Buffer		buffer;

	CmdType old_cmdtype = curr_cmdtype;

#ifdef LOCATOR
	DualRefDescData drefDesc;
#endif /* LOCATOR */

	Assert(TTS_IS_BUFFERTUPLE(slot));
	Assert(IsSiro(relation));
	Assert(snapshot->snapshot_type == SNAPSHOT_MVCC);

	/* Get buffer by using tid */
	block = ItemPointerGetBlockNumber(tid);
	buffer = ReadBuffer(relation, block);

#ifdef LOCATOR
#ifdef USING_LOCK
    drefDesc.dual_ref = NULL;
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
#else
	/* Get dual_ref for avoiding race with heap_insert() */
	drefDesc.dual_ref = (pg_atomic_uint64*)GetBufferDualRef(buffer);
	SetPageRefUnit(&drefDesc);
#endif
#else /* LOCATOR */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
#endif /* LOCATOR */

	/* Set t_self to p-locator */
	bslot->base.tupdata.t_self = *tid;

	/* To allow the current transaction to read the version modified by itself */
	if (old_cmdtype != CMD_UPDATE && old_cmdtype != CMD_DELETE)
	{
		snapshot->curcid += 1;
		curr_cmdtype = CMD_SELECT;
	}

	got_heap_tuple =
				 heap_hot_search_buffer_with_vc(tid,
												relation,
												buffer,
												snapshot,
												&bslot->base.tupdata,
												&bslot->base.copied_tuple,
#ifdef LOCATOR
												&drefDesc,
#endif /* LOCATOR */
												&all_dead,
												false,
												NULL);

	/* Restore the curcid */
	if (old_cmdtype != CMD_UPDATE && old_cmdtype != CMD_DELETE)
	{
		snapshot->curcid -= 1;
		curr_cmdtype = old_cmdtype;
	}

#if defined(LOCATOR) && !defined(USING_LOCK)
	/* Decrease ref_cnt */
	DRefDecrRefCnt(drefDesc.dual_ref, drefDesc.page_ref_unit);
#else /* !LOCATOR */
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
#endif /* LOCATOR */

	if (got_heap_tuple)
	{
		ExecStoreBufferHeapTuple(bslot->base.copied_tuple, slot, buffer);
		slot->tts_tableOid = RelationGetRelid(relation);

		ReleaseBuffer(buffer);
		return true;
	}

	ReleaseBuffer(buffer);
	return false;
}
#endif /* DIVA */

#ifdef LOCATOR
static bool
locator_search_row_version(LocatorTuplePosition tuplePosition,
						   ItemPointer tid,
						   Snapshot snapshot,
						   TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	LocatorExecutor *executor = slot->tts_locator_executor;
	LocatorExecutorLevelDesc *level_desc
		= locator_get_level_columnar_desc(executor,
										  tuplePosition->partitionLevel);
	bool is_modification, found = false;
	CmdType old_cmdtype = curr_cmdtype;
	ListCell *lc;

#ifdef LOCATOR_DEBUG
	Assert(level_desc != NULL);
#endif /* LOCATOR_DEBUG */

	/*
	 * Set t_self to p-locator.
	 */
	bslot->base.tupdata.t_self = *tid;

	/* To allow the current transaction to read the version modified by itself */
	if (old_cmdtype != CMD_UPDATE && old_cmdtype != CMD_DELETE)
	{
		snapshot->curcid += 1;
		curr_cmdtype = CMD_SELECT;
	}

	is_modification = (curr_cmdtype == CMD_UPDATE ||
					   curr_cmdtype == CMD_DELETE);
	
	found = locator_search_and_deform_version(executor->rel,
											  level_desc,
											  slot,
											  snapshot,
											  tuplePosition,
											  NULL,
											  is_modification);

	/* Restore the curcid */
	if (old_cmdtype != CMD_UPDATE && old_cmdtype != CMD_DELETE)
	{
		snapshot->curcid -= 1;
		curr_cmdtype = old_cmdtype;
	}

	if (found)
	{
		slot->tts_tuple_position = tuplePosition;
	}
	else
	{
		slot->tts_tuple_position = NULL;
		pfree(tuplePosition);
	}

	/*
	 * The SIRO tuple has already been copied.
	 * So we can release its buffer.
	 */
	foreach(lc, level_desc->required_column_group_desc_list)
	{
		LocatorExecutorColumnGroupDesc *group_desc = lfirst(lc);

		if (!group_desc->isSiro)
			continue;

		if (BufferIsValid(group_desc->c_buf))
		{
			ReleaseBuffer(group_desc->c_buf);
			group_desc->c_buf = InvalidBuffer;
		}
	}

	return found;
}
#endif /* LOCATOR */

#ifdef LOCATOR
/*
 * Search the visible version with route synopsis. (similar with index search)
 * Referenced heapam_index_fetch_tuple() & heapam_fetch_fow_version
 */
static bool
locatoram_search_row_version(Relation relation,
							 ItemPointer tid,
							 Snapshot snapshot,
							 TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	LocatorRouteSynopsis route_synopsis = slot->tts_locator_route_synopsis;
	CmdType old_cmdtype = curr_cmdtype;
	LocatorExternalCatalog *exCatalog = LocatorGetExternalCatalog(relation->rd_node.relNode);
	LocatorTuplePosition tuplePosition;
	bool		all_dead;
	bool		got_partition_tuple;
	BlockNumber block;
	Buffer		buffer;
	DualRefDescData drefDesc;
	LocatorExecutor *locator_executor = slot->tts_locator_executor;
	
	Assert(TTS_IS_BUFFERTUPLE(slot));
	Assert(IsSiro(relation));
	Assert(IsLocator(relation));
	Assert(snapshot->snapshot_type == SNAPSHOT_MVCC);

	tuplePosition = (LocatorTuplePosition) palloc0(sizeof(LocatorTuplePositionData));

	/* Find target location. */		
	LocatorRouteSynopsisGetTuplePositionForUpdate(exCatalog, relation, 
												  route_synopsis, tuplePosition);

	if (locator_is_columnar_layout(locator_executor,
								   tuplePosition->partitionLevel))
		return locator_search_row_version(tuplePosition, tid, snapshot, slot);

	/* Get buffer by using tid */
	block = tuplePosition->partitionTuplePosition / relation->records_per_block;
	buffer = ReadPartitionBufferExtended(relation, 
										tuplePosition->partitionLevel, 
										tuplePosition->partitionNumber, 
										tuplePosition->partitionGenerationNumber,
										block, RBM_NORMAL, NULL);

	/* Get dual_ref for avoiding race with heap_insert() */
#ifdef USING_LOCK
    drefDesc.dual_ref = NULL;
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
#else
	drefDesc.dual_ref = (pg_atomic_uint64*) GetBufferDualRef(buffer);
	SetPageRefUnit(&drefDesc);
#endif

	/* Set t_self to p-locator */
	bslot->base.tupdata.t_self = *tid;

	/* To allow the current transaction to read the version modified by itself */
	if (old_cmdtype != CMD_UPDATE && old_cmdtype != CMD_DELETE)
	{
		snapshot->curcid += 1;
		curr_cmdtype = CMD_SELECT;
	}

	/* 
	 * Retrieve a tuple using the tuple position obtained through the route
	 * synopsis. 
	 */
	got_partition_tuple = locator_search_version(tuplePosition,
												 relation,
												 buffer,
												 snapshot,
												 &bslot->base.tupdata,
												 &bslot->base.copied_tuple,
												 &drefDesc,
												 &all_dead,
												 false,
												 NULL,
												 InvalidBlockNumber,
												 InvalidOffsetNumber);

	/* Restore the curcid */
	if (old_cmdtype != CMD_UPDATE && old_cmdtype != CMD_DELETE)
	{
		snapshot->curcid -= 1;
		curr_cmdtype = old_cmdtype;
	}

	/* Decrease ref_cnt */
#ifdef USING_LOCK
    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
#else
	DRefDecrRefCnt(drefDesc.dual_ref, drefDesc.page_ref_unit);
#endif

	if (got_partition_tuple)
	{
		ExecStoreBufferHeapTuple(bslot->base.copied_tuple, slot, buffer);
		slot->tts_tableOid = RelationGetRelid(relation);
		slot->tts_tuple_position = tuplePosition;

		ReleaseBuffer(buffer);
		return true;
	}
	else
	{
		slot->tts_tuple_position = NULL;
		pfree(tuplePosition);
	}

	ReleaseBuffer(buffer);
	return false;
}
#endif /* LOCATOR */

static bool
heapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;

	return ItemPointerIsValid(tid) &&
		ItemPointerGetBlockNumber(tid) < hscan->rs_nblocks;
}

static bool
heapam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								Snapshot snapshot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	bool		res;

	Assert(TTS_IS_BUFFERTUPLE(slot));
	Assert(BufferIsValid(bslot->buffer));

	/*
	 * We need buffer pin and lock to call HeapTupleSatisfiesVisibility.
	 * Caller should be holding pin, but not lock.
	 */
	LockBuffer(bslot->buffer, BUFFER_LOCK_SHARE);
	res = HeapTupleSatisfiesVisibility(bslot->base.tuple, snapshot,
									   bslot->buffer);
	LockBuffer(bslot->buffer, BUFFER_LOCK_UNLOCK);

	return res;
}


/* ----------------------------------------------------------------------------
 *  Functions for manipulations of physical tuples for heap AM.
 * ----------------------------------------------------------------------------
 */

static void
heapam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
					int options, BulkInsertState bistate)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* Update the tuple with table oid */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;
#ifdef LOCATOR
	tuple->t_locator_route_synopsis = NULL;
#endif /* LOCATOR */

	/* Perform the insertion, and copy the resulting ItemPointer */
	heap_insert(relation, tuple, cid, options, bistate);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

static void
heapam_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
								CommandId cid, int options,
								BulkInsertState bistate, uint32 specToken)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* Update the tuple with table oid */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	HeapTupleHeaderSetSpeculativeToken(tuple->t_data, specToken);
	options |= HEAP_INSERT_SPECULATIVE;

	/* Perform the insertion, and copy the resulting ItemPointer */
	heap_insert(relation, tuple, cid, options, bistate);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

static void
heapam_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
								  uint32 specToken, bool succeeded)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* adjust the tuple's state accordingly */
	if (succeeded)
		heap_finish_speculative(relation, &slot->tts_tid);
	else
		heap_abort_speculative(relation, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

static TM_Result
heapam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
					Snapshot snapshot, Snapshot crosscheck, bool wait,
					TM_FailureData *tmfd, bool changingPart)
{
#ifdef DIVA
	bool		siro;

	siro = IsSiro(relation);

	if (siro)
		/* heap deletion for vDriver. */
		return heap_delete_with_vc(relation, tid, cid, snapshot,
								   crosscheck, wait, tmfd, changingPart);
	else
		/* Original routine. */
		return heap_delete(relation, tid, cid, crosscheck, wait, tmfd, changingPart);


#else
	/*
	 * Currently Deleting of index tuples are handled at vacuum, in case if
	 * the storage itself is cleaning the dead tuples by itself, it is the
	 * time to call the index tuple deletion also.
	 */
	return heap_delete(relation, tid, cid, crosscheck, wait, tmfd, changingPart);
#endif
}


static TM_Result
heapam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
					CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					bool wait, TM_FailureData *tmfd,
					LockTupleMode *lockmode, bool *update_indexes)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
	TM_Result	result;

#ifdef DIVA
	bool siro;
#endif

	/* Update the tuple with table oid */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;
#ifdef DIVA

	siro = IsSiro(relation);

	if (siro)
		result = heap_update_with_vc(relation, otid, 
#ifdef LOCATOR
					NULL,
					NULL,
#endif /* LOCATOR */
					tuple, cid, snapshot,
					crosscheck, wait, tmfd, lockmode);
	else
		result = heap_update(relation, otid, tuple, cid, crosscheck, wait,
				tmfd, lockmode);

#else
	result = heap_update(relation, otid, tuple, cid, crosscheck, wait,
						 tmfd, lockmode);
#endif

	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	/*
	 * Decide whether new index entries are needed for the tuple
	 *
	 * Note: heap_update returns the tid (location) of the new tuple in the
	 * t_self field.
	 *
	 * If it's a HOT update, we mustn't insert new index entries.
	 */
#ifdef DIVA
	if (siro)
		*update_indexes = false;
	else
		*update_indexes = result == TM_Ok && !HeapTupleIsHeapOnly(tuple);
#else
	*update_indexes = result == TM_Ok && !HeapTupleIsHeapOnly(tuple);
#endif

	if (shouldFree)
		pfree(tuple);

	return result;
}

#ifdef LOCATOR
static TM_Result
locatoram_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
					CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					bool wait, TM_FailureData *tmfd,
					LockTupleMode *lockmode, bool *update_indexes)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
	TM_Result	result;

	Assert(IsSiro(relation));
	Assert(IsLocator(relation));

	/* Update the tuple with table oid */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	result = heap_update_with_vc(relation,
								 otid,
								 slot->tts_tuple_position,
								 slot->tts_locator_executor,
								 tuple,
								 cid, 
								 snapshot,
								 crosscheck,
								 wait,
								 tmfd,
								 lockmode);

	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	/*
	 * Decide whether new index entries are needed for the tuple
	 *
	 * Note: heap_update returns the tid (location) of the new tuple in the
	 * t_self field.
	 *
	 * If it's a HOT update, we mustn't insert new index entries.
	 */
	*update_indexes = false;

	if (shouldFree)
		pfree(tuple);

	return result;
}
#endif

static TM_Result
heapam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot,
				  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				  LockWaitPolicy wait_policy, uint8 flags,
				  TM_FailureData *tmfd)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	TM_Result	result;
	Buffer		buffer;
	HeapTuple	tuple = &bslot->base.tupdata;
	bool		follow_updates;

	follow_updates = (flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) != 0;
	tmfd->traversed = false;

	Assert(TTS_IS_BUFFERTUPLE(slot));

tuple_lock_retry:
	tuple->t_self = *tid;
	result = heap_lock_tuple(relation, tuple, cid, mode, wait_policy,
							 follow_updates, &buffer, tmfd);

	if (result == TM_Updated &&
		(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION))
	{
		/* Should not encounter speculative tuple on recheck */
		Assert(!HeapTupleHeaderIsSpeculative(tuple->t_data));

		ReleaseBuffer(buffer);

		if (!ItemPointerEquals(&tmfd->ctid, &tuple->t_self))
		{
			SnapshotData SnapshotDirty;
			TransactionId priorXmax;

			/* it was updated, so look at the updated version */
			*tid = tmfd->ctid;
			/* updated row should have xmin matching this xmax */
			priorXmax = tmfd->xmax;

			/* signal that a tuple later in the chain is getting locked */
			tmfd->traversed = true;

			/*
			 * fetch target tuple
			 *
			 * Loop here to deal with updated or busy tuples
			 */
			InitDirtySnapshot(SnapshotDirty);
			for (;;)
			{
				if (ItemPointerIndicatesMovedPartitions(tid))
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("tuple to be locked was already moved to another partition due to concurrent update")));

				tuple->t_self = *tid;
				if (heap_fetch(relation, &SnapshotDirty, tuple, &buffer, true))
				{
					/*
					 * If xmin isn't what we're expecting, the slot must have
					 * been recycled and reused for an unrelated tuple.  This
					 * implies that the latest version of the row was deleted,
					 * so we need do nothing.  (Should be safe to examine xmin
					 * without getting buffer's content lock.  We assume
					 * reading a TransactionId to be atomic, and Xmin never
					 * changes in an existing tuple, except to invalid or
					 * frozen, and neither of those can match priorXmax.)
					 */
					if (!TransactionIdEquals(HeapTupleHeaderGetXmin(tuple->t_data),
											 priorXmax))
					{
						ReleaseBuffer(buffer);
						return TM_Deleted;
					}

					/* otherwise xmin should not be dirty... */
					if (TransactionIdIsValid(SnapshotDirty.xmin))
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg_internal("t_xmin %u is uncommitted in tuple (%u,%u) to be updated in table \"%s\"",
												 SnapshotDirty.xmin,
												 ItemPointerGetBlockNumber(&tuple->t_self),
												 ItemPointerGetOffsetNumber(&tuple->t_self),
												 RelationGetRelationName(relation))));

					/*
					 * If tuple is being updated by other transaction then we
					 * have to wait for its commit/abort, or die trying.
					 */
					if (TransactionIdIsValid(SnapshotDirty.xmax))
					{
						ReleaseBuffer(buffer);
						switch (wait_policy)
						{
							case LockWaitBlock:
								XactLockTableWait(SnapshotDirty.xmax,
												  relation, &tuple->t_self,
												  XLTW_FetchUpdated);
								break;
							case LockWaitSkip:
								if (!ConditionalXactLockTableWait(SnapshotDirty.xmax))
									/* skip instead of waiting */
									return TM_WouldBlock;
								break;
							case LockWaitError:
								if (!ConditionalXactLockTableWait(SnapshotDirty.xmax))
									ereport(ERROR,
											(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
											 errmsg("could not obtain lock on row in relation \"%s\"",
													RelationGetRelationName(relation))));
								break;
						}
						continue;	/* loop back to repeat heap_fetch */
					}

					/*
					 * If tuple was inserted by our own transaction, we have
					 * to check cmin against cid: cmin >= current CID means
					 * our command cannot see the tuple, so we should ignore
					 * it. Otherwise heap_lock_tuple() will throw an error,
					 * and so would any later attempt to update or delete the
					 * tuple.  (We need not check cmax because
					 * HeapTupleSatisfiesDirty will consider a tuple deleted
					 * by our transaction dead, regardless of cmax.)  We just
					 * checked that priorXmax == xmin, so we can test that
					 * variable instead of doing HeapTupleHeaderGetXmin again.
					 */
					if (TransactionIdIsCurrentTransactionId(priorXmax) &&
						HeapTupleHeaderGetCmin(tuple->t_data) >= cid)
					{
						tmfd->xmax = priorXmax;

						/*
						 * Cmin is the problematic value, so store that. See
						 * above.
						 */
						tmfd->cmax = HeapTupleHeaderGetCmin(tuple->t_data);
						ReleaseBuffer(buffer);
						return TM_SelfModified;
					}

					/*
					 * This is a live tuple, so try to lock it again.
					 */
					ReleaseBuffer(buffer);
					goto tuple_lock_retry;
				}

				/*
				 * If the referenced slot was actually empty, the latest
				 * version of the row must have been deleted, so we need do
				 * nothing.
				 */
				if (tuple->t_data == NULL)
				{
					Assert(!BufferIsValid(buffer));
					return TM_Deleted;
				}

				/*
				 * As above, if xmin isn't what we're expecting, do nothing.
				 */
				if (!TransactionIdEquals(HeapTupleHeaderGetXmin(tuple->t_data),
										 priorXmax))
				{
					ReleaseBuffer(buffer);
					return TM_Deleted;
				}

				/*
				 * If we get here, the tuple was found but failed
				 * SnapshotDirty. Assuming the xmin is either a committed xact
				 * or our own xact (as it certainly should be if we're trying
				 * to modify the tuple), this must mean that the row was
				 * updated or deleted by either a committed xact or our own
				 * xact.  If it was deleted, we can ignore it; if it was
				 * updated then chain up to the next version and repeat the
				 * whole process.
				 *
				 * As above, it should be safe to examine xmax and t_ctid
				 * without the buffer content lock, because they can't be
				 * changing.  We'd better hold a buffer pin though.
				 */
				if (ItemPointerEquals(&tuple->t_self, &tuple->t_data->t_ctid))
				{
					/* deleted, so forget about it */
					ReleaseBuffer(buffer);
					return TM_Deleted;
				}

				/* updated, so look at the updated row */
				*tid = tuple->t_data->t_ctid;
				/* updated row should have xmin matching this xmax */
				priorXmax = HeapTupleHeaderGetUpdateXid(tuple->t_data);
				ReleaseBuffer(buffer);
				/* loop back to fetch next in chain */
			}
		}
		else
		{
			/* tuple was deleted, so give up */
			return TM_Deleted;
		}
	}

	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	/* store in slot, transferring existing pin */
	ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);

	return result;
}


/* ------------------------------------------------------------------------
 * DDL related callbacks for heap AM.
 * ------------------------------------------------------------------------
 */

static void
heapam_relation_set_new_filenode(Relation rel,
								 const RelFileNode *newrnode,
								 char persistence,
								 TransactionId *freezeXid,
								 MultiXactId *minmulti)
{
	SMgrRelation srel;

	/*
	 * Initialize to the minimum XID that could put tuples in the table. We
	 * know that no xacts older than RecentXmin are still running, so that
	 * will do.
	 */
	*freezeXid = RecentXmin;

	/*
	 * Similarly, initialize the minimum Multixact to the first value that
	 * could possibly be stored in tuples in the table.  Running transactions
	 * could reuse values from their local cache, so we are careful to
	 * consider all currently running multis.
	 *
	 * XXX this could be refined further, but is it worth the hassle?
	 */
	*minmulti = GetOldestMultiXactId();

	srel = RelationCreateStorage(*newrnode, persistence, true);

	/*
	 * If required, set up an init fork for an unlogged table so that it can
	 * be correctly reinitialized on restart.  An immediate sync is required
	 * even if the page has been logged, because the write did not go through
	 * shared_buffers and therefore a concurrent checkpoint may have moved the
	 * redo pointer past our xlog record.  Recovery may as well remove it
	 * while replaying, for example, XLOG_DBASE_CREATE* or XLOG_TBLSPC_CREATE
	 * record. Therefore, logging is necessary even if wal_level=minimal.
	 */
	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
			   rel->rd_rel->relkind == RELKIND_MATVIEW ||
			   rel->rd_rel->relkind == RELKIND_TOASTVALUE);
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrnode, INIT_FORKNUM);
		smgrimmedsync(srel, INIT_FORKNUM);
	}

	smgrclose(srel);
}

static void
heapam_relation_nontransactional_truncate(Relation rel)
{
	RelationTruncate(rel, 0);
}

static void
heapam_relation_copy_data(Relation rel, const RelFileNode *newrnode)
{
	SMgrRelation dstrel;

	dstrel = smgropen(*newrnode, rel->rd_backend);

	/*
	 * Since we copy the file directly without looking at the shared buffers,
	 * we'd better first flush out any pages of the source relation that are
	 * in shared buffers.  We assume no new changes will be made while we are
	 * holding exclusive lock on the rel.
	 */
	FlushRelationBuffers(rel);

	/*
	 * Create and copy all forks of the relation, and schedule unlinking of
	 * old physical files.
	 *
	 * NOTE: any conflict in relfilenode value will be caught in
	 * RelationCreateStorage().
	 */
	RelationCreateStorage(*newrnode, rel->rd_rel->relpersistence, true);

	/* copy main fork */
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	/* copy those extra forks that exist */
	for (ForkNumber forkNum = MAIN_FORKNUM + 1;
		 forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(RelationGetSmgr(rel), forkNum))
		{
			smgrcreate(dstrel, forkNum, false);

			/*
			 * WAL log creation if the relation is persistent, or this is the
			 * init fork of an unlogged relation.
			 */
			if (RelationIsPermanent(rel) ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 forkNum == INIT_FORKNUM))
				log_smgrcreate(newrnode, forkNum);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, forkNum,
								rel->rd_rel->relpersistence);
		}
	}


	/* drop old relation, and close new one */
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

static void
heapam_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap,
								 Relation OldIndex, bool use_sort,
								 TransactionId OldestXmin,
								 TransactionId *xid_cutoff,
								 MultiXactId *multi_cutoff,
								 double *num_tuples,
								 double *tups_vacuumed,
								 double *tups_recently_dead)
{
	RewriteState rwstate;
	IndexScanDesc indexScan;
	TableScanDesc tableScan;
	HeapScanDesc heapScan;
	bool		is_system_catalog;
	Tuplesortstate *tuplesort;
	TupleDesc	oldTupDesc = RelationGetDescr(OldHeap);
	TupleDesc	newTupDesc = RelationGetDescr(NewHeap);
	TupleTableSlot *slot;
	int			natts;
	Datum	   *values;
	bool	   *isnull;
	BufferHeapTupleTableSlot *hslot;
	BlockNumber prev_cblock = InvalidBlockNumber;

	/* Remember if it's a system catalog */
	is_system_catalog = IsSystemRelation(OldHeap);

	/*
	 * Valid smgr_targblock implies something already wrote to the relation.
	 * This may be harmless, but this function hasn't planned for it.
	 */
	Assert(RelationGetTargetBlock(NewHeap) == InvalidBlockNumber);

	/* Preallocate values/isnull arrays */
	natts = newTupDesc->natts;
	values = (Datum *) palloc(natts * sizeof(Datum));
	isnull = (bool *) palloc(natts * sizeof(bool));

	/* Initialize the rewrite operation */
	rwstate = begin_heap_rewrite(OldHeap, NewHeap, OldestXmin, *xid_cutoff,
								 *multi_cutoff);


	/* Set up sorting if wanted */
	if (use_sort)
		tuplesort = tuplesort_begin_cluster(oldTupDesc, OldIndex,
											maintenance_work_mem,
											NULL, TUPLESORT_NONE);
	else
		tuplesort = NULL;

	/*
	 * Prepare to scan the OldHeap.  To ensure we see recently-dead tuples
	 * that still need to be copied, we scan with SnapshotAny and use
	 * HeapTupleSatisfiesVacuum for the visibility test.
	 */
	if (OldIndex != NULL && !use_sort)
	{
		const int	ci_index[] = {
			PROGRESS_CLUSTER_PHASE,
			PROGRESS_CLUSTER_INDEX_RELID
		};
		int64		ci_val[2];

		/* Set phase and OIDOldIndex to columns */
		ci_val[0] = PROGRESS_CLUSTER_PHASE_INDEX_SCAN_HEAP;
		ci_val[1] = RelationGetRelid(OldIndex);
		pgstat_progress_update_multi_param(2, ci_index, ci_val);

		tableScan = NULL;
		heapScan = NULL;
		indexScan = index_beginscan(OldHeap, OldIndex, SnapshotAny, 0, 0);
		index_rescan(indexScan, NULL, 0, NULL, 0);
	}
	else
	{
		/* In scan-and-sort mode and also VACUUM FULL, set phase */
		pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
									 PROGRESS_CLUSTER_PHASE_SEQ_SCAN_HEAP);

#ifdef LOCATOR
		tableScan = table_beginscan(OldHeap, SnapshotAny, 0, (ScanKey) NULL,
									NULL);
#else
		tableScan = table_beginscan(OldHeap, SnapshotAny, 0, (ScanKey) NULL);
#endif /* LOCATOR */
		heapScan = (HeapScanDesc) tableScan;
		indexScan = NULL;

		/* Set total heap blocks */
		pgstat_progress_update_param(PROGRESS_CLUSTER_TOTAL_HEAP_BLKS,
									 heapScan->rs_nblocks);
	}

	slot = table_slot_create(OldHeap, NULL);
	hslot = (BufferHeapTupleTableSlot *) slot;

	/*
	 * Scan through the OldHeap, either in OldIndex order or sequentially;
	 * copy each tuple into the NewHeap, or transiently to the tuplesort
	 * module.  Note that we don't bother sorting dead tuples (they won't get
	 * to the new table anyway).
	 */
	for (;;)
	{
		HeapTuple	tuple;
		Buffer		buf;
		bool		isdead;

		CHECK_FOR_INTERRUPTS();

		if (indexScan != NULL)
		{
			if (!index_getnext_slot(indexScan, ForwardScanDirection, slot))
				break;

			/* Since we used no scan keys, should never need to recheck */
			if (indexScan->xs_recheck)
				elog(ERROR, "CLUSTER does not support lossy index conditions");
		}
		else
		{
			if (!table_scan_getnextslot(tableScan, ForwardScanDirection, slot))
			{
				/*
				 * If the last pages of the scan were empty, we would go to
				 * the next phase while heap_blks_scanned != heap_blks_total.
				 * Instead, to ensure that heap_blks_scanned is equivalent to
				 * total_heap_blks after the table scan phase, this parameter
				 * is manually updated to the correct value when the table
				 * scan finishes.
				 */
				pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_BLKS_SCANNED,
											 heapScan->rs_nblocks);
				break;
			}

			/*
			 * In scan-and-sort mode and also VACUUM FULL, set heap blocks
			 * scanned
			 *
			 * Note that heapScan may start at an offset and wrap around, i.e.
			 * rs_startblock may be >0, and rs_cblock may end with a number
			 * below rs_startblock. To prevent showing this wraparound to the
			 * user, we offset rs_cblock by rs_startblock (modulo rs_nblocks).
			 */
			if (prev_cblock != heapScan->rs_cblock)
			{
				pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_BLKS_SCANNED,
											 (heapScan->rs_cblock +
											  heapScan->rs_nblocks -
											  heapScan->rs_startblock
											  ) % heapScan->rs_nblocks + 1);
				prev_cblock = heapScan->rs_cblock;
			}
		}

		tuple = ExecFetchSlotHeapTuple(slot, false, NULL);
		buf = hslot->buffer;

		LockBuffer(buf, BUFFER_LOCK_SHARE);

		switch (HeapTupleSatisfiesVacuum(tuple, OldestXmin, buf))
		{
			case HEAPTUPLE_DEAD:
				/* Definitely dead */
				isdead = true;
				break;
			case HEAPTUPLE_RECENTLY_DEAD:
				*tups_recently_dead += 1;
				/* fall through */
			case HEAPTUPLE_LIVE:
				/* Live or recently dead, must copy it */
				isdead = false;
				break;
			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * Since we hold exclusive lock on the relation, normally the
				 * only way to see this is if it was inserted earlier in our
				 * own transaction.  However, it can happen in system
				 * catalogs, since we tend to release write lock before commit
				 * there.  Give a warning if neither case applies; but in any
				 * case we had better copy it.
				 */
				if (!is_system_catalog &&
					!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple->t_data)))
					elog(WARNING, "concurrent insert in progress within table \"%s\"",
						 RelationGetRelationName(OldHeap));
				/* treat as live */
				isdead = false;
				break;
			case HEAPTUPLE_DELETE_IN_PROGRESS:

				/*
				 * Similar situation to INSERT_IN_PROGRESS case.
				 */
				if (!is_system_catalog &&
					!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetUpdateXid(tuple->t_data)))
					elog(WARNING, "concurrent delete in progress within table \"%s\"",
						 RelationGetRelationName(OldHeap));
				/* treat as recently dead */
				*tups_recently_dead += 1;
				isdead = false;
				break;
			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				isdead = false; /* keep compiler quiet */
				break;
		}

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		if (isdead)
		{
			*tups_vacuumed += 1;
			/* heap rewrite module still needs to see it... */
			if (rewrite_heap_dead_tuple(rwstate, tuple))
			{
				/* A previous recently-dead tuple is now known dead */
				*tups_vacuumed += 1;
				*tups_recently_dead -= 1;
			}
			continue;
		}

		*num_tuples += 1;
		if (tuplesort != NULL)
		{
			tuplesort_putheaptuple(tuplesort, tuple);

			/*
			 * In scan-and-sort mode, report increase in number of tuples
			 * scanned
			 */
			pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_TUPLES_SCANNED,
										 *num_tuples);
		}
		else
		{
			const int	ct_index[] = {
				PROGRESS_CLUSTER_HEAP_TUPLES_SCANNED,
				PROGRESS_CLUSTER_HEAP_TUPLES_WRITTEN
			};
			int64		ct_val[2];

			reform_and_rewrite_tuple(tuple, OldHeap, NewHeap,
									 values, isnull, rwstate);

			/*
			 * In indexscan mode and also VACUUM FULL, report increase in
			 * number of tuples scanned and written
			 */
			ct_val[0] = *num_tuples;
			ct_val[1] = *num_tuples;
			pgstat_progress_update_multi_param(2, ct_index, ct_val);
		}
	}

	if (indexScan != NULL)
		index_endscan(indexScan);
	if (tableScan != NULL)
		table_endscan(tableScan);
	if (slot)
		ExecDropSingleTupleTableSlot(slot);

	/*
	 * In scan-and-sort mode, complete the sort, then read out all live tuples
	 * from the tuplestore and write them to the new relation.
	 */
	if (tuplesort != NULL)
	{
		double		n_tuples = 0;

		/* Report that we are now sorting tuples */
		pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
									 PROGRESS_CLUSTER_PHASE_SORT_TUPLES);

		tuplesort_performsort(tuplesort);

		/* Report that we are now writing new heap */
		pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
									 PROGRESS_CLUSTER_PHASE_WRITE_NEW_HEAP);

		for (;;)
		{
			HeapTuple	tuple;

			CHECK_FOR_INTERRUPTS();

			tuple = tuplesort_getheaptuple(tuplesort, true);
			if (tuple == NULL)
				break;

			n_tuples += 1;
			reform_and_rewrite_tuple(tuple,
									 OldHeap, NewHeap,
									 values, isnull,
									 rwstate);
			/* Report n_tuples */
			pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_TUPLES_WRITTEN,
										 n_tuples);
		}

		tuplesort_end(tuplesort);
	}

	/* Write out any remaining tuples, and fsync if needed */
	end_heap_rewrite(rwstate);

	/* Clean up */
	pfree(values);
	pfree(isnull);
}

static bool
heapam_scan_analyze_next_block(TableScanDesc scan, BlockNumber blockno,
							   BufferAccessStrategy bstrategy)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;

	/*
	 * We must maintain a pin on the target page's buffer to ensure that
	 * concurrent activity - e.g. HOT pruning - doesn't delete tuples out from
	 * under us.  Hence, pin the page until we are done looking at it.  We
	 * also choose to hold sharelock on the buffer throughout --- we could
	 * release and re-acquire sharelock for each tuple, but since we aren't
	 * doing much work per tuple, the extra lock traffic is probably better
	 * avoided.
	 */
	hscan->rs_cblock = blockno;
	hscan->rs_cindex = FirstOffsetNumber;
	hscan->rs_cbuf = ReadBufferExtended(scan->rs_rd, MAIN_FORKNUM,
										blockno, RBM_NORMAL, bstrategy);
	LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

	/* in heap all blocks can contain tuples, so always return true */
	return true;
}

static bool
heapam_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
							   double *liverows, double *deadrows,
							   TupleTableSlot *slot)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	Page		targpage;
	OffsetNumber maxoffset;
	BufferHeapTupleTableSlot *hslot;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	hslot = (BufferHeapTupleTableSlot *) slot;
	targpage = BufferGetPage(hscan->rs_cbuf);
	maxoffset = PageGetMaxOffsetNumber(targpage);

	/* Inner loop over all tuples on the selected page */
	for (; hscan->rs_cindex <= maxoffset; hscan->rs_cindex++)
	{
		ItemId		itemid;
		HeapTuple	targtuple = &hslot->base.tupdata;
		bool		sample_it = false;

		itemid = PageGetItemId(targpage, hscan->rs_cindex);

		/*
		 * We ignore unused and redirect line pointers.  DEAD line pointers
		 * should be counted as dead, because we need vacuum to run to get rid
		 * of them.  Note that this rule agrees with the way that
		 * heap_page_prune() counts things.
		 */
		if (!ItemIdIsNormal(itemid))
		{
			if (ItemIdIsDead(itemid))
				*deadrows += 1;
			continue;
		}

#ifdef DIVA
		if (LP_IS_PLEAF_FLAG(itemid))
			continue;
#endif

		ItemPointerSet(&targtuple->t_self, hscan->rs_cblock, hscan->rs_cindex);

		targtuple->t_tableOid = RelationGetRelid(scan->rs_rd);
		targtuple->t_data = (HeapTupleHeader) PageGetItem(targpage, itemid);
		targtuple->t_len = ItemIdGetLength(itemid);

		switch (HeapTupleSatisfiesVacuum(targtuple, OldestXmin,
										 hscan->rs_cbuf))
		{
			case HEAPTUPLE_LIVE:
				sample_it = true;
				*liverows += 1;
				break;

			case HEAPTUPLE_DEAD:
			case HEAPTUPLE_RECENTLY_DEAD:
				/* Count dead and recently-dead rows */
				*deadrows += 1;
				break;

			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * Insert-in-progress rows are not counted.  We assume that
				 * when the inserting transaction commits or aborts, it will
				 * send a stats message to increment the proper count.  This
				 * works right only if that transaction ends after we finish
				 * analyzing the table; if things happen in the other order,
				 * its stats update will be overwritten by ours.  However, the
				 * error will be large only if the other transaction runs long
				 * enough to insert many tuples, so assuming it will finish
				 * after us is the safer option.
				 *
				 * A special case is that the inserting transaction might be
				 * our own.  In this case we should count and sample the row,
				 * to accommodate users who load a table and analyze it in one
				 * transaction.  (pgstat_report_analyze has to adjust the
				 * numbers we report to the cumulative stats system to make
				 * this come out right.)
				 */
				if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(targtuple->t_data)))
				{
					sample_it = true;
					*liverows += 1;
				}
				break;

			case HEAPTUPLE_DELETE_IN_PROGRESS:

				/*
				 * We count and sample delete-in-progress rows the same as
				 * live ones, so that the stats counters come out right if the
				 * deleting transaction commits after us, per the same
				 * reasoning given above.
				 *
				 * If the delete was done by our own transaction, however, we
				 * must count the row as dead to make pgstat_report_analyze's
				 * stats adjustments come out right.  (Note: this works out
				 * properly when the row was both inserted and deleted in our
				 * xact.)
				 *
				 * The net effect of these choices is that we act as though an
				 * IN_PROGRESS transaction hasn't happened yet, except if it
				 * is our own transaction, which we assume has happened.
				 *
				 * This approach ensures that we behave sanely if we see both
				 * the pre-image and post-image rows for a row being updated
				 * by a concurrent transaction: we will sample the pre-image
				 * but not the post-image.  We also get sane results if the
				 * concurrent transaction never commits.
				 */
				if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetUpdateXid(targtuple->t_data)))
					*deadrows += 1;
				else
				{
					sample_it = true;
					*liverows += 1;
				}
				break;

			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}

		if (sample_it)
		{
			ExecStoreBufferHeapTuple(targtuple, slot, hscan->rs_cbuf);
			hscan->rs_cindex++;

			/* note that we leave the buffer locked here! */
			return true;
		}
	}

	/* Now release the lock and pin on the page */
	UnlockReleaseBuffer(hscan->rs_cbuf);
	hscan->rs_cbuf = InvalidBuffer;

	/* also prevent old slot contents from having pin on page */
	ExecClearTuple(slot);

	return false;
}

static double
heapam_index_build_range_scan(Relation heapRelation,
							  Relation indexRelation,
							  IndexInfo *indexInfo,
							  bool allow_sync,
							  bool anyvisible,
							  bool progress,
							  BlockNumber start_blockno,
							  BlockNumber numblocks,
							  IndexBuildCallback callback,
							  void *callback_state,
							  TableScanDesc scan)
{
	HeapScanDesc hscan;
	bool		is_system_catalog;
	bool		checking_uniqueness;
	HeapTuple	heapTuple;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	double		reltuples;
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	Snapshot	snapshot;
	bool		need_unregister_snapshot = false;
	TransactionId OldestXmin;
	BlockNumber previous_blkno = InvalidBlockNumber;
	BlockNumber root_blkno = InvalidBlockNumber;
	OffsetNumber root_offsets[MaxHeapTuplesPerPage];
#ifdef DIVA
	bool		siro = IsSiro(heapRelation);
#endif /* DIVA */

	/*
	 * sanity checks
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	/* Remember if it's a system catalog */
	is_system_catalog = IsSystemRelation(heapRelation);

	/* See whether we're verifying uniqueness/exclusion properties */
	checking_uniqueness = (indexInfo->ii_Unique ||
						   indexInfo->ii_ExclusionOps != NULL);

	/*
	 * "Any visible" mode is not compatible with uniqueness checks; make sure
	 * only one of those is requested.
	 */
	Assert(!(anyvisible && checking_uniqueness));

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.  Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heapRelation, NULL);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up execution state for predicate, if any. */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * Prepare for scan of the base relation.  In a normal index build, we use
	 * SnapshotAny because we must retrieve all tuples and do our own time
	 * qual checks (because we have to index RECENTLY_DEAD tuples). In a
	 * concurrent build, or during bootstrap, we take a regular MVCC snapshot
	 * and index whatever's live according to that.
	 */
	OldestXmin = InvalidTransactionId;

	/* okay to ignore lazy VACUUMs here */
	if (!IsBootstrapProcessingMode() && !indexInfo->ii_Concurrent)
		OldestXmin = GetOldestNonRemovableTransactionId(heapRelation);

	if (!scan)
	{
#ifdef DIVA
		if (siro)
		{
			/*
			 * heap_getnext() of DIVA returns only one visible version of each
			 * record, so we have to pass an appropriate snapshot.
			 */
			snapshot = RegisterSnapshot(GetTransactionSnapshot());
			need_unregister_snapshot = true;
		}
		else
		{
			/*
			 * Serial index build.
			 *
			 * Must begin our own heap scan in this case.  We may also need to
			 * register a snapshot whose lifetime is under our direct control.
			 */
			if (!TransactionIdIsValid(OldestXmin))
			{
				snapshot = RegisterSnapshot(GetTransactionSnapshot());
				need_unregister_snapshot = true;
			}
			else
				snapshot = SnapshotAny;
			}
#else /* !DIVA */
		/*
		 * Serial index build.
		 *
		 * Must begin our own heap scan in this case.  We may also need to
		 * register a snapshot whose lifetime is under our direct control.
		 */
		if (!TransactionIdIsValid(OldestXmin))
		{
			snapshot = RegisterSnapshot(GetTransactionSnapshot());
			need_unregister_snapshot = true;
		}
		else
			snapshot = SnapshotAny;
#endif /* DIVA */
#ifdef LOCATOR
		scan = table_beginscan_strat(heapRelation,	/* relation */
									 snapshot,	/* snapshot */
									 0, /* number of keys */
									 NULL,	/* scan key */
									 true,	/* buffer access strategy OK */
									 allow_sync,	/* syncscan OK? */
									 NULL);
#else /* !LOCATOR */
		scan = table_beginscan_strat(heapRelation,	/* relation */
									 snapshot,	/* snapshot */
									 0, /* number of keys */
									 NULL,	/* scan key */
									 true,	/* buffer access strategy OK */
									 allow_sync);	/* syncscan OK? */
#endif /* LOCATOR */
	}
	else
	{
		/*
		 * Parallel index build.
		 *
		 * Parallel case never registers/unregisters own snapshot.  Snapshot
		 * is taken from parallel heap scan, and is SnapshotAny or an MVCC
		 * snapshot, based on same criteria as serial case.
		 */
		Assert(!IsBootstrapProcessingMode());
		Assert(allow_sync);
		snapshot = scan->rs_snapshot;
	}

	hscan = (HeapScanDesc) scan;

	/*
	 * Must have called GetOldestNonRemovableTransactionId() if using
	 * SnapshotAny.  Shouldn't have for an MVCC snapshot. (It's especially
	 * worth checking this for parallel builds, since ambuild routines that
	 * support parallel builds must work these details out for themselves.)
	 */
	Assert(snapshot == SnapshotAny || IsMVCCSnapshot(snapshot));
#ifdef DIVA
	if (!siro)
#endif /* DIVA */
	Assert(snapshot == SnapshotAny ? TransactionIdIsValid(OldestXmin) :
		   !TransactionIdIsValid(OldestXmin));
	Assert(snapshot == SnapshotAny || !anyvisible);

	/* Publish number of blocks to scan */
	if (progress)
	{
		BlockNumber nblocks;

		if (hscan->rs_base.rs_parallel != NULL)
		{
			ParallelBlockTableScanDesc pbscan;

			pbscan = (ParallelBlockTableScanDesc) hscan->rs_base.rs_parallel;
			nblocks = pbscan->phs_nblocks;
		}
		else
			nblocks = hscan->rs_nblocks;

		pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
									 nblocks);
	}

	/* set our scan endpoints */
	if (!allow_sync)
		heap_setscanlimits(scan, start_blockno, numblocks);
	else
	{
		/* syncscan can only be requested on whole relation */
		Assert(start_blockno == 0);
		Assert(numblocks == InvalidBlockNumber);
	}

	reltuples = 0;

	/*
	 * Scan all tuples in the base relation.
	 */
	while ((heapTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool		tupleIsAlive;

		CHECK_FOR_INTERRUPTS();

		/* Report scan progress, if asked to. */
		if (progress)
		{
			BlockNumber blocks_done = heapam_scan_get_blocks_done(hscan);

			if (blocks_done != previous_blkno)
			{
				pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
											 blocks_done);
				previous_blkno = blocks_done;
			}
		}

		/*
		 * When dealing with a HOT-chain of updated tuples, we want to index
		 * the values of the live tuple (if any), but index it under the TID
		 * of the chain's root tuple.  This approach is necessary to preserve
		 * the HOT-chain structure in the heap. So we need to be able to find
		 * the root item offset for every tuple that's in a HOT-chain.  When
		 * first reaching a new page of the relation, call
		 * heap_get_root_tuples() to build a map of root item offsets on the
		 * page.
		 *
		 * It might look unsafe to use this information across buffer
		 * lock/unlock.  However, we hold ShareLock on the table so no
		 * ordinary insert/update/delete should occur; and we hold pin on the
		 * buffer continuously while visiting the page, so no pruning
		 * operation can occur either.
		 *
		 * In cases with only ShareUpdateExclusiveLock on the table, it's
		 * possible for some HOT tuples to appear that we didn't know about
		 * when we first read the page.  To handle that case, we re-obtain the
		 * list of root offsets when a HOT tuple points to a root item that we
		 * don't know about.
		 *
		 * Also, although our opinions about tuple liveness could change while
		 * we scan the page (due to concurrent transaction commits/aborts),
		 * the chain root locations won't, so this info doesn't need to be
		 * rebuilt after waiting for another transaction.
		 *
		 * Note the implied assumption that there is no more than one live
		 * tuple per HOT-chain --- else we could create more than one index
		 * entry pointing to the same root tuple.
		 */
		if (hscan->rs_cblock != root_blkno)
		{
#ifdef LOCATOR
			Page		page;

			if (BufferIsValid(hscan->rs_cbuf))
			{
				page = BufferGetPage(hscan->rs_cbuf);

				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);
				heap_get_root_tuples(page, root_offsets);
				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
			}
			else
			{
				page = hscan->c_readbuf_dp;
				heap_get_root_tuples(page, root_offsets);
			}
#else /* !LOCATOR */
			Page		page = BufferGetPage(hscan->rs_cbuf);

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);
			heap_get_root_tuples(page, root_offsets);
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
#endif /* LOCATOR */

			root_blkno = hscan->rs_cblock;
		}

		if (snapshot == SnapshotAny)
		{
			/* do our own time qual check */
			bool		indexIt;
			TransactionId xwait;

	recheck:

			/*
			 * We could possibly get away with not locking the buffer here,
			 * since caller should hold ShareLock on the relation, but let's
			 * be conservative about it.  (This remark is still correct even
			 * with HOT-pruning: our pin on the buffer prevents pruning.)
			 */
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

			/*
			 * The criteria for counting a tuple as live in this block need to
			 * match what analyze.c's heapam_scan_analyze_next_tuple() does,
			 * otherwise CREATE INDEX and ANALYZE may produce wildly different
			 * reltuples values, e.g. when there are many recently-dead
			 * tuples.
			 */
			switch (HeapTupleSatisfiesVacuum(heapTuple, OldestXmin,
											 hscan->rs_cbuf))
			{
				case HEAPTUPLE_DEAD:
					/* Definitely dead, we can ignore it */
					indexIt = false;
					tupleIsAlive = false;
					break;
				case HEAPTUPLE_LIVE:
					/* Normal case, index and unique-check it */
					indexIt = true;
					tupleIsAlive = true;
					/* Count it as live, too */
					reltuples += 1;
					break;
				case HEAPTUPLE_RECENTLY_DEAD:

					/*
					 * If tuple is recently deleted then we must index it
					 * anyway to preserve MVCC semantics.  (Pre-existing
					 * transactions could try to use the index after we finish
					 * building it, and may need to see such tuples.)
					 *
					 * However, if it was HOT-updated then we must only index
					 * the live tuple at the end of the HOT-chain.  Since this
					 * breaks semantics for pre-existing snapshots, mark the
					 * index as unusable for them.
					 *
					 * We don't count recently-dead tuples in reltuples, even
					 * if we index them; see heapam_scan_analyze_next_tuple().
					 */
					if (HeapTupleIsHotUpdated(heapTuple))
					{
						indexIt = false;
						/* mark the index as unsafe for old snapshots */
						indexInfo->ii_BrokenHotChain = true;
					}
					else
						indexIt = true;
					/* In any case, exclude the tuple from unique-checking */
					tupleIsAlive = false;
					break;
				case HEAPTUPLE_INSERT_IN_PROGRESS:

					/*
					 * In "anyvisible" mode, this tuple is visible and we
					 * don't need any further checks.
					 */
					if (anyvisible)
					{
						indexIt = true;
						tupleIsAlive = true;
						reltuples += 1;
						break;
					}

					/*
					 * Since caller should hold ShareLock or better, normally
					 * the only way to see this is if it was inserted earlier
					 * in our own transaction.  However, it can happen in
					 * system catalogs, since we tend to release write lock
					 * before commit there.  Give a warning if neither case
					 * applies.
					 */
					xwait = HeapTupleHeaderGetXmin(heapTuple->t_data);
					if (!TransactionIdIsCurrentTransactionId(xwait))
					{
						if (!is_system_catalog)
							elog(WARNING, "concurrent insert in progress within table \"%s\"",
								 RelationGetRelationName(heapRelation));

						/*
						 * If we are performing uniqueness checks, indexing
						 * such a tuple could lead to a bogus uniqueness
						 * failure.  In that case we wait for the inserting
						 * transaction to finish and check again.
						 */
						if (checking_uniqueness)
						{
							/*
							 * Must drop the lock on the buffer before we wait
							 */
							LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
							XactLockTableWait(xwait, heapRelation,
											  &heapTuple->t_self,
											  XLTW_InsertIndexUnique);
							CHECK_FOR_INTERRUPTS();
							goto recheck;
						}
					}
					else
					{
						/*
						 * For consistency with
						 * heapam_scan_analyze_next_tuple(), count
						 * HEAPTUPLE_INSERT_IN_PROGRESS tuples as live only
						 * when inserted by our own transaction.
						 */
						reltuples += 1;
					}

					/*
					 * We must index such tuples, since if the index build
					 * commits then they're good.
					 */
					indexIt = true;
					tupleIsAlive = true;
					break;
				case HEAPTUPLE_DELETE_IN_PROGRESS:

					/*
					 * As with INSERT_IN_PROGRESS case, this is unexpected
					 * unless it's our own deletion or a system catalog; but
					 * in anyvisible mode, this tuple is visible.
					 */
					if (anyvisible)
					{
						indexIt = true;
						tupleIsAlive = false;
						reltuples += 1;
						break;
					}

					xwait = HeapTupleHeaderGetUpdateXid(heapTuple->t_data);
					if (!TransactionIdIsCurrentTransactionId(xwait))
					{
						if (!is_system_catalog)
							elog(WARNING, "concurrent delete in progress within table \"%s\"",
								 RelationGetRelationName(heapRelation));

						/*
						 * If we are performing uniqueness checks, assuming
						 * the tuple is dead could lead to missing a
						 * uniqueness violation.  In that case we wait for the
						 * deleting transaction to finish and check again.
						 *
						 * Also, if it's a HOT-updated tuple, we should not
						 * index it but rather the live tuple at the end of
						 * the HOT-chain.  However, the deleting transaction
						 * could abort, possibly leaving this tuple as live
						 * after all, in which case it has to be indexed. The
						 * only way to know what to do is to wait for the
						 * deleting transaction to finish and check again.
						 */
						if (checking_uniqueness ||
							HeapTupleIsHotUpdated(heapTuple))
						{
							/*
							 * Must drop the lock on the buffer before we wait
							 */
							LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
							XactLockTableWait(xwait, heapRelation,
											  &heapTuple->t_self,
											  XLTW_InsertIndexUnique);
							CHECK_FOR_INTERRUPTS();
							goto recheck;
						}

						/*
						 * Otherwise index it but don't check for uniqueness,
						 * the same as a RECENTLY_DEAD tuple.
						 */
						indexIt = true;

						/*
						 * Count HEAPTUPLE_DELETE_IN_PROGRESS tuples as live,
						 * if they were not deleted by the current
						 * transaction.  That's what
						 * heapam_scan_analyze_next_tuple() does, and we want
						 * the behavior to be consistent.
						 */
						reltuples += 1;
					}
					else if (HeapTupleIsHotUpdated(heapTuple))
					{
						/*
						 * It's a HOT-updated tuple deleted by our own xact.
						 * We can assume the deletion will commit (else the
						 * index contents don't matter), so treat the same as
						 * RECENTLY_DEAD HOT-updated tuples.
						 */
						indexIt = false;
						/* mark the index as unsafe for old snapshots */
						indexInfo->ii_BrokenHotChain = true;
					}
					else
					{
						/*
						 * It's a regular tuple deleted by our own xact. Index
						 * it, but don't check for uniqueness nor count in
						 * reltuples, the same as a RECENTLY_DEAD tuple.
						 */
						indexIt = true;
					}
					/* In any case, exclude the tuple from unique-checking */
					tupleIsAlive = false;
					break;
				default:
					elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
					indexIt = tupleIsAlive = false; /* keep compiler quiet */
					break;
			}

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			if (!indexIt)
				continue;
		}
		else
		{
			/* heap_getnext did the time qual check */
			tupleIsAlive = true;
			reltuples += 1;
		}

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		/* Set up for predicate or expression evaluation */
		ExecStoreBufferHeapTuple(heapTuple, slot, hscan->rs_cbuf);

		/*
		 * In a partial index, discard tuples that don't satisfy the
		 * predicate.
		 */
		if (predicate != NULL)
		{
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * For the current heap tuple, extract all the attributes we use in
		 * this index, and note which are null.  This also performs evaluation
		 * of any expressions needed.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/*
		 * You'd think we should go ahead and build the index tuple here, but
		 * some index AMs want to do further processing on the data first.  So
		 * pass the values[] and isnull[] arrays, instead.
		 */

		if (HeapTupleIsHeapOnly(heapTuple))
		{
			/*
			 * For a heap-only tuple, pretend its TID is that of the root. See
			 * src/backend/access/heap/README.HOT for discussion.
			 */
			ItemPointerData tid;
			OffsetNumber offnum;

			offnum = ItemPointerGetOffsetNumber(&heapTuple->t_self);

			/*
			 * If a HOT tuple points to a root that we don't know about,
			 * obtain root items afresh.  If that still fails, report it as
			 * corruption.
			 */
			if (root_offsets[offnum - 1] == InvalidOffsetNumber)
			{
				Page		page = BufferGetPage(hscan->rs_cbuf);

				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);
				heap_get_root_tuples(page, root_offsets);
				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
			}

			if (!OffsetNumberIsValid(root_offsets[offnum - 1]))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("failed to find parent tuple for heap-only tuple at (%u,%u) in table \"%s\"",
										 ItemPointerGetBlockNumber(&heapTuple->t_self),
										 offnum,
										 RelationGetRelationName(heapRelation))));

			ItemPointerSet(&tid, ItemPointerGetBlockNumber(&heapTuple->t_self),
						   root_offsets[offnum - 1]);

			/* Call the AM's callback routine to process the tuple */
			callback(indexRelation, &tid, values, isnull, tupleIsAlive,
					 callback_state);
		}
		else
		{
			/* Call the AM's callback routine to process the tuple */
			callback(indexRelation, &heapTuple->t_self, values, isnull,
					 tupleIsAlive, callback_state);
		}
	}

	/* Report scan progress one last time. */
	if (progress)
	{
		BlockNumber blks_done;

		if (hscan->rs_base.rs_parallel != NULL)
		{
			ParallelBlockTableScanDesc pbscan;

			pbscan = (ParallelBlockTableScanDesc) hscan->rs_base.rs_parallel;
			blks_done = pbscan->phs_nblocks;
		}
		else
			blks_done = hscan->rs_nblocks;

		pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
									 blks_done);
	}

	table_endscan(scan);

	/* we can now forget our snapshot, if set and registered by us */
	if (need_unregister_snapshot)
		UnregisterSnapshot(snapshot);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* These may have been pointing to the now-gone estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;

	return reltuples;
}

static void
heapam_index_validate_scan(Relation heapRelation,
						   Relation indexRelation,
						   IndexInfo *indexInfo,
						   Snapshot snapshot,
						   ValidateIndexState *state)
{
	TableScanDesc scan;
	HeapScanDesc hscan;
	HeapTuple	heapTuple;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	BlockNumber root_blkno = InvalidBlockNumber;
	OffsetNumber root_offsets[MaxHeapTuplesPerPage];
	bool		in_index[MaxHeapTuplesPerPage];
	BlockNumber previous_blkno = InvalidBlockNumber;

	/* state variables for the merge */
	ItemPointer indexcursor = NULL;
	ItemPointerData decoded;
	bool		tuplesort_empty = false;

	/*
	 * sanity checks
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.  Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRelation),
									&TTSOpsHeapTuple);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up execution state for predicate, if any. */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * Prepare for scan of the base relation.  We need just those tuples
	 * satisfying the passed-in reference snapshot.  We must disable syncscan
	 * here, because it's critical that we read from block zero forward to
	 * match the sorted TIDs.
	 */
#ifdef LOCATOR
	scan = table_beginscan_strat(heapRelation,	/* relation */
								 snapshot,	/* snapshot */
								 0, /* number of keys */
								 NULL,	/* scan key */
								 true,	/* buffer access strategy OK */
								 false,	/* syncscan not OK */
								 NULL);
#else /* !LOCATOR */
	scan = table_beginscan_strat(heapRelation,	/* relation */
								 snapshot,	/* snapshot */
								 0, /* number of keys */
								 NULL,	/* scan key */
								 true,	/* buffer access strategy OK */
								 false);	/* syncscan not OK */
#endif /* LOCATOR */
	hscan = (HeapScanDesc) scan;

	pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
								 hscan->rs_nblocks);

	/*
	 * Scan all tuples matching the snapshot.
	 */
	while ((heapTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		ItemPointer heapcursor = &heapTuple->t_self;
		ItemPointerData rootTuple;
		OffsetNumber root_offnum;

		CHECK_FOR_INTERRUPTS();

		state->htups += 1;

		if ((previous_blkno == InvalidBlockNumber) ||
			(hscan->rs_cblock != previous_blkno))
		{
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
										 hscan->rs_cblock);
			previous_blkno = hscan->rs_cblock;
		}

		/*
		 * As commented in table_index_build_scan, we should index heap-only
		 * tuples under the TIDs of their root tuples; so when we advance onto
		 * a new heap page, build a map of root item offsets on the page.
		 *
		 * This complicates merging against the tuplesort output: we will
		 * visit the live tuples in order by their offsets, but the root
		 * offsets that we need to compare against the index contents might be
		 * ordered differently.  So we might have to "look back" within the
		 * tuplesort output, but only within the current page.  We handle that
		 * by keeping a bool array in_index[] showing all the
		 * already-passed-over tuplesort output TIDs of the current page. We
		 * clear that array here, when advancing onto a new heap page.
		 */
		if (hscan->rs_cblock != root_blkno)
		{
			Page		page = BufferGetPage(hscan->rs_cbuf);

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);
			heap_get_root_tuples(page, root_offsets);
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			memset(in_index, 0, sizeof(in_index));

			root_blkno = hscan->rs_cblock;
		}

		/* Convert actual tuple TID to root TID */
		rootTuple = *heapcursor;
		root_offnum = ItemPointerGetOffsetNumber(heapcursor);

		if (HeapTupleIsHeapOnly(heapTuple))
		{
			root_offnum = root_offsets[root_offnum - 1];
			if (!OffsetNumberIsValid(root_offnum))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("failed to find parent tuple for heap-only tuple at (%u,%u) in table \"%s\"",
										 ItemPointerGetBlockNumber(heapcursor),
										 ItemPointerGetOffsetNumber(heapcursor),
										 RelationGetRelationName(heapRelation))));
			ItemPointerSetOffsetNumber(&rootTuple, root_offnum);
		}

		/*
		 * "merge" by skipping through the index tuples until we find or pass
		 * the current root tuple.
		 */
		while (!tuplesort_empty &&
			   (!indexcursor ||
				ItemPointerCompare(indexcursor, &rootTuple) < 0))
		{
			Datum		ts_val;
			bool		ts_isnull;

			if (indexcursor)
			{
				/*
				 * Remember index items seen earlier on the current heap page
				 */
				if (ItemPointerGetBlockNumber(indexcursor) == root_blkno)
					in_index[ItemPointerGetOffsetNumber(indexcursor) - 1] = true;
			}

			tuplesort_empty = !tuplesort_getdatum(state->tuplesort, true,
												  &ts_val, &ts_isnull, NULL);
			Assert(tuplesort_empty || !ts_isnull);
			if (!tuplesort_empty)
			{
				itemptr_decode(&decoded, DatumGetInt64(ts_val));
				indexcursor = &decoded;

				/* If int8 is pass-by-ref, free (encoded) TID Datum memory */
#ifndef USE_FLOAT8_BYVAL
				pfree(DatumGetPointer(ts_val));
#endif
			}
			else
			{
				/* Be tidy */
				indexcursor = NULL;
			}
		}

		/*
		 * If the tuplesort has overshot *and* we didn't see a match earlier,
		 * then this tuple is missing from the index, so insert it.
		 */
		if ((tuplesort_empty ||
			 ItemPointerCompare(indexcursor, &rootTuple) > 0) &&
			!in_index[root_offnum - 1])
		{
			MemoryContextReset(econtext->ecxt_per_tuple_memory);

			/* Set up for predicate or expression evaluation */
			ExecStoreHeapTuple(heapTuple, slot, false);

			/*
			 * In a partial index, discard tuples that don't satisfy the
			 * predicate.
			 */
			if (predicate != NULL)
			{
				if (!ExecQual(predicate, econtext))
					continue;
			}

			/*
			 * For the current heap tuple, extract all the attributes we use
			 * in this index, and note which are null.  This also performs
			 * evaluation of any expressions needed.
			 */
			FormIndexDatum(indexInfo,
						   slot,
						   estate,
						   values,
						   isnull);

			/*
			 * You'd think we should go ahead and build the index tuple here,
			 * but some index AMs want to do further processing on the data
			 * first. So pass the values[] and isnull[] arrays, instead.
			 */

			/*
			 * If the tuple is already committed dead, you might think we
			 * could suppress uniqueness checking, but this is no longer true
			 * in the presence of HOT, because the insert is actually a proxy
			 * for a uniqueness check on the whole HOT-chain.  That is, the
			 * tuple we have here could be dead because it was already
			 * HOT-updated, and if so the updating transaction will not have
			 * thought it should insert index entries.  The index AM will
			 * check the whole HOT-chain and correctly detect a conflict if
			 * there is one.
			 */

			index_insert(indexRelation,
						 values,
						 isnull,
						 &rootTuple,
						 heapRelation,
						 indexInfo->ii_Unique ?
						 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
						 false,
						 indexInfo);

			state->tups_inserted += 1;
		}
	}

	table_endscan(scan);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* These may have been pointing to the now-gone estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
}

/*
 * Return the number of blocks that have been read by this scan since
 * starting.  This is meant for progress reporting rather than be fully
 * accurate: in a parallel scan, workers can be concurrently reading blocks
 * further ahead than what we report.
 */
static BlockNumber
heapam_scan_get_blocks_done(HeapScanDesc hscan)
{
	ParallelBlockTableScanDesc bpscan = NULL;
	BlockNumber startblock;
	BlockNumber blocks_done;

	if (hscan->rs_base.rs_parallel != NULL)
	{
		bpscan = (ParallelBlockTableScanDesc) hscan->rs_base.rs_parallel;
		startblock = bpscan->phs_startblock;
	}
	else
		startblock = hscan->rs_startblock;

	/*
	 * Might have wrapped around the end of the relation, if startblock was
	 * not zero.
	 */
	if (hscan->rs_cblock > startblock)
		blocks_done = hscan->rs_cblock - startblock;
	else
	{
		BlockNumber nblocks;

		nblocks = bpscan != NULL ? bpscan->phs_nblocks : hscan->rs_nblocks;
		blocks_done = nblocks - startblock +
			hscan->rs_cblock;
	}

	return blocks_done;
}


/* ------------------------------------------------------------------------
 * Miscellaneous callbacks for the heap AM
 * ------------------------------------------------------------------------
 */

/*
 * Check to see whether the table needs a TOAST table.  It does only if
 * (1) there are any toastable attributes, and (2) the maximum length
 * of a tuple could exceed TOAST_TUPLE_THRESHOLD.  (We don't want to
 * create a toast table for something like "f1 varchar(20)".)
 */
static bool
heapam_relation_needs_toast_table(Relation rel)
{
	int32		data_length = 0;
	bool		maxlength_unknown = false;
	bool		has_toastable_attrs = false;
	TupleDesc	tupdesc = rel->rd_att;
	int32		tuple_length;
	int			i;

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		data_length = att_align_nominal(data_length, att->attalign);
		if (att->attlen > 0)
		{
			/* Fixed-length types are never toastable */
			data_length += att->attlen;
		}
		else
		{
			int32		maxlen = type_maximum_size(att->atttypid,
												   att->atttypmod);

			if (maxlen < 0)
				maxlength_unknown = true;
			else
				data_length += maxlen;
			if (att->attstorage != TYPSTORAGE_PLAIN)
				has_toastable_attrs = true;
		}
	}
	if (!has_toastable_attrs)
		return false;			/* nothing to toast? */
	if (maxlength_unknown)
		return true;			/* any unlimited-length attrs? */
	tuple_length = MAXALIGN(SizeofHeapTupleHeader +
							BITMAPLEN(tupdesc->natts)) +
		MAXALIGN(data_length);
	return (tuple_length > TOAST_TUPLE_THRESHOLD);
}

/*
 * TOAST tables for heap relations are just heap relations.
 */
static Oid
heapam_relation_toast_am(Relation rel)
{
	return rel->rd_rel->relam;
}


/* ------------------------------------------------------------------------
 * Planner related callbacks for the heap AM
 * ------------------------------------------------------------------------
 */

#define HEAP_OVERHEAD_BYTES_PER_TUPLE \
	(MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData))
#define HEAP_USABLE_BYTES_PER_PAGE \
	(BLCKSZ - SizeOfPageHeaderData)

static void
heapam_estimate_rel_size(Relation rel, int32 *attr_widths,
						 BlockNumber *pages, double *tuples,
						 double *allvisfrac)
{
	table_block_relation_estimate_size(rel, attr_widths, pages,
									   tuples, allvisfrac,
									   HEAP_OVERHEAD_BYTES_PER_TUPLE,
									   HEAP_USABLE_BYTES_PER_PAGE);
}


/* ------------------------------------------------------------------------
 * Executor related callbacks for the heap AM
 * ------------------------------------------------------------------------
 */

static bool
heapam_scan_bitmap_next_block(TableScanDesc scan,
							  TBMIterateResult *tbmres)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	BlockNumber page = tbmres->blockno;
	Buffer		buffer;
	Snapshot	snapshot;
	int			ntup;
	Relation	relation;
#ifdef LOCATOR
	DualRefDescData	drefDesc;
#endif /* LOCATOR */

#ifdef DIVA
	bool  siro = IsSiro(scan->rs_rd);
#endif

	hscan->rs_cindex = 0;
	hscan->rs_ntuples = 0;

	relation = scan->rs_rd;

#ifdef LOCATOR
	Assert(!IsLocator(relation));
#endif /* LOCATOR */

	/*
	 * Ignore any claimed entries past what we think is the end of the
	 * relation. It may have been extended after the start of our scan (we
	 * only hold an AccessShareLock, and it could be inserts from this
	 * backend).
	 */
	if (page >= hscan->rs_nblocks)
		return false;

	/*
	 * Acquire pin on the target heap page, trading in any pin we held before.
	 */
	hscan->rs_cbuf = ReleaseAndReadBuffer(hscan->rs_cbuf,
										  scan->rs_rd,
										  page);
	hscan->rs_cblock = page;
	buffer = hscan->rs_cbuf;
	snapshot = scan->rs_snapshot;

	ntup = 0;

	/*
	 * Prune and repair fragmentation for the whole page, if possible.
	 */
	heap_page_prune_opt(scan->rs_rd, buffer);

#ifdef LOCATOR
#ifdef USING_LOCK
	drefDesc.dual_ref = NULL;
#else
	if (siro)
	{
		/* Get dual_ref for avoiding race with heap_insert() */
		drefDesc.dual_ref = (pg_atomic_uint64*)GetBufferDualRef(buffer);
		SetPageRefUnit(&drefDesc);
	}
	else
#endif
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
#else /* !LOCATOR */
	/*
	 * We must hold share lock on the buffer content while examining tuple
	 * visibility.  Afterwards, however, the tuples we have found to be
	 * visible are guaranteed good as long as we hold the buffer pin.
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
#endif /* LOCATOR */

	/*
	 * We need two separate strategies for lossy and non-lossy cases.
	 */
	if (tbmres->ntuples >= 0)
	{
		/*
		 * Bitmap is non-lossy, so we just look through the offsets listed in
		 * tbmres; but we have to follow any HOT chain starting at each such
		 * offset.
		 */
		int			curslot;

		for (curslot = 0; curslot < tbmres->ntuples; curslot++)
		{
			OffsetNumber offnum = tbmres->offsets[curslot];
			ItemPointerData tid;
			HeapTupleData heapTuple;

			ItemPointerSet(&tid, page, offnum);

#ifdef DIVA
			if (siro)
			{
#ifdef LOCATOR
				if (heap_hot_search_buffer_with_vc(
							&tid, scan->rs_rd, buffer, snapshot, &heapTuple,
							&hscan->rs_vistuples_copied[ntup], &drefDesc,
							NULL, true, NULL))
					hscan->rs_vistuples[ntup++] =
						ItemPointerGetOffsetNumber(&tid);
#else /* !LOCATOR */
				if (heap_hot_search_buffer_with_vc(
							&tid, scan->rs_rd, buffer, snapshot, &heapTuple,
							&hscan->rs_vistuples_copied[ntup],
							NULL, true, NULL))
					hscan->rs_vistuples[ntup++] =
						ItemPointerGetOffsetNumber(&tid);
#endif /* LOCATOR */
				
			}
			else
			{
				if (heap_hot_search_buffer(&tid, scan->rs_rd, buffer,
							snapshot, &heapTuple, NULL, true))
					hscan->rs_vistuples[ntup++] =
						ItemPointerGetOffsetNumber(&tid);
			}
#else
			if (heap_hot_search_buffer(&tid, scan->rs_rd, buffer, snapshot,
									   &heapTuple, NULL, true))
				hscan->rs_vistuples[ntup++] = ItemPointerGetOffsetNumber(&tid);
#endif
		}
	}
	else
	{
#ifdef DIVA
		bool		valid;

		Item p_locator;
		uint64 l_off, r_off;
		TransactionId xid_bound;
		int ret_id;

#ifdef LOCATOR
		uint64		rec_ref_unit;
		uint8		check_var;
		bool		set_hint_bits;
		Buffer		hint_bits_buf;
#endif /* LOCATOR */

		HeapTupleData	v_loctup;
		BlockNumber		v_block;
		OffsetNumber	v_offset;
		Buffer			v_buffer;
		Page			v_dp;
		ItemId			v_lpp;

		bool modification = (curr_cmdtype == CMD_UPDATE ||
							 curr_cmdtype == CMD_DELETE);
#endif /* DIVA */

		/*
		 * Bitmap is lossy, so we must examine each line pointer on the page.
		 * But we can ignore HOT chains, since we'll check each tuple anyway.
		 */
		Page		dp = (Page) BufferGetPage(buffer);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(dp);
		OffsetNumber offnum;
		Oid     rel_id = RelationGetRelid(relation);

#ifdef DIVA
		ItemId		lp;
		HeapTupleData loctup;

		if (siro)
		{
			v_loctup.t_tableOid = rel_id;

			/* Same as heapgetpage() */
			for (offnum = FirstOffsetNumber; offnum <= maxoff;
				 offnum = OffsetNumberNext(offnum))
			{
				lp = PageGetItemId(dp, offnum);

				/* Don't read version directly */
				if (!LP_IS_PLEAF_FLAG(lp))
					continue;
				
				/* Get p-locator of this record */
				p_locator = PageGetItem(dp, lp);

				/* Set self pointer of tuple */
				ItemPointerSet(&(v_loctup.t_self), page, offnum);

				/* Init checking variables */
				valid = false;
				v_buffer = buffer;

#ifdef LOCATOR
				/* Increment reference count and determine where to check */
				GetCheckVar(&drefDesc, &rec_ref_unit, &check_var,
							&set_hint_bits, offnum);
#endif /* LOCATOR */

				/* Check visibility of left, right versions with check_var */
				for (uint8 test_var = CHECK_RIGHT; test_var > CHECK_NONE; test_var--)
				{
#ifdef LOCATOR
					/* Is this version need to be checked? */
					if (!(check_var & test_var))
						continue;
#endif /* LOCATOR */

					/* Release previous version buffer */
					if (unlikely(v_buffer != buffer))
						ReleaseBuffer(v_buffer);

					/* Set block number and offset */
					v_block = page;
					v_offset = offnum + test_var; 

					Assert(BlockNumberIsValid(v_block));
					Assert(OffsetNumberIsValid(v_offset));

					/*
					 * If the version is in the same page with p-locator, just
					 * get it.
					 * Or not, read the buffer that it is in.
					 */
					if (likely(v_block == page))
					{
						v_buffer = buffer;
						v_dp = dp;
					}
					else
					{
						v_buffer = ReadBuffer(relation, v_block);
						Assert(BufferIsValid(v_buffer));
						v_dp = BufferGetPage(v_buffer);
					}

					v_lpp = PageGetItemId(v_dp, v_offset);

					/* The target has never been updated after INSERT */
					if (unlikely(LP_OVR_IS_UNUSED(v_lpp)))
						continue;
						
					v_loctup.t_data = (HeapTupleHeader) PageGetItem(v_dp, v_lpp);
					v_loctup.t_len = ItemIdGetLength(v_lpp);

#ifdef LOCATOR
					/* Set buffer to set hint bits */
					hint_bits_buf = set_hint_bits ? v_buffer : InvalidBuffer;

					/* Check visibility of version */
					valid = HeapTupleSatisfiesVisibility(&v_loctup, snapshot,
														 hint_bits_buf);
#else /* !LOCATOR */
					/* Check visibility of version */
					valid = HeapTupleSatisfiesVisibility(&v_loctup, snapshot,
														 v_buffer);
#endif /* LOCATOR */

					if (valid)
					{
						if (hscan->rs_vistuples_copied[ntup] != NULL)
							heap_freetuple(hscan->rs_vistuples_copied[ntup]);

						hscan->rs_vistuples_copied[ntup] = heap_copytuple(&v_loctup);

						hscan->rs_vistuples[ntup] = offnum;
						ntup++;

						break;
					}
				}

				/* If we found visible version from heap page, continue */
				if (valid)
				{
					/* Release version buffer */
					if (unlikely(v_buffer != buffer))
						ReleaseBuffer(v_buffer);

#ifdef LOCATOR
					/* Decrease ref_cnt */
					if (rec_ref_unit != 0)
						DRefDecrRefCnt(drefDesc.dual_ref, rec_ref_unit);
#endif /* LOCATOR */

					++(scan->counter_in_heap);
					
					continue;
				}

				/* Both left and right-side versions are invisible */
				if (modification)
				{
					/*
					 * If this scanning is for update, we don't need to bother
					 * searching deeply.
					 */
					if (hscan->rs_vistuples_copied[ntup] != NULL)
						heap_freetuple(hscan->rs_vistuples_copied[ntup]);

					/*
					 * We cannot find visible tuple inside the heap page.
					 * Copy one of any tuple in the heap page so that
					 * following ExecStoreBufferHeapTuple can be passed.
					 */
					hscan->rs_vistuples_copied[ntup] = heap_copytuple(&v_loctup);

					hscan->rs_vistuples[ntup] = offnum;
					ntup++;

					/* Release version buffer */
					if (unlikely(v_buffer != buffer))
						ReleaseBuffer(v_buffer);

#ifdef LOCATOR
					/* Decrease ref_cnt */
					if (rec_ref_unit != 0)
						DRefDecrRefCnt(drefDesc.dual_ref, rec_ref_unit);
#endif /* LOCATOR */

					continue;
				}

				/* Release version buffer */
				if (unlikely(v_buffer != buffer))
					ReleaseBuffer(v_buffer);

				/* Only MVCC snapshot can traverse p-leaf & ebi-tree */
				if (snapshot->snapshot_type != SNAPSHOT_MVCC)
#ifdef LOCATOR
					goto failed;
#else /* !LOCATOR */
					continue;
#endif /* LOCATOR */

				/*
				* Both versions are invisible to this transaction.
				* Need to search from p-leaf & ebi-tree.
				*/
				memcpy(&l_off, p_locator, sizeof(uint64));
				memcpy(&r_off, p_locator + sizeof(uint64), sizeof(uint64));
				memcpy(&xid_bound, p_locator + sizeof(uint64) * 2, sizeof(TransactionId));

				if (l_off == 0 && r_off == 0)
				{
					ret_id = -1;
				}
				else 
				{
					PLeafOffset p_offset;

					if (PLeafIsLeftLookup(l_off, r_off, xid_bound, snapshot))
						p_offset = l_off;
					else
						p_offset = r_off;

					ret_id = PLeafLookupTuple(rel_id, 
											  false,
											  NULL,
											  p_offset, 
											  snapshot, 
											  &(v_loctup.t_len), 
											  (void**) &(v_loctup.t_data));
				}

				/* If head version is visible in memory, get that version */
				if (ret_id > -1)
				{
					if (hscan->rs_vistuples_copied[ntup] != NULL)
						heap_freetuple(hscan->rs_vistuples_copied[ntup]);

					hscan->rs_vistuples_copied[ntup] = heap_copytuple(&v_loctup);
					
					/* Unpin a EBI sub page */
					UnpinEbiSubBuffer(ret_id);

					hscan->rs_vistuples[ntup] = offnum;
					ntup++;
				
					++(scan->counter_in_ebi);
				}

#ifdef LOCATOR
failed:
				/* Decrease ref_cnt */
				if (rec_ref_unit != 0)
					DRefDecrRefCnt(drefDesc.dual_ref, rec_ref_unit);
#endif /* LOCATOR */
			}
		}
		else
		{
			for (offnum = FirstOffsetNumber; offnum <= maxoff;
				 offnum = OffsetNumberNext(offnum))
			{
				lp = PageGetItemId(dp, offnum);
				if (!ItemIdIsNormal(lp))
					continue;
				loctup.t_data = (HeapTupleHeader) PageGetItem((Page) dp, lp);
				loctup.t_len = ItemIdGetLength(lp);
				loctup.t_tableOid = rel_id;
				ItemPointerSet(&loctup.t_self, page, offnum);
				valid = HeapTupleSatisfiesVisibility(&loctup, snapshot, buffer);
				if (valid)
				{
					hscan->rs_vistuples[ntup++] = offnum;
					PredicateLockTID(scan->rs_rd, &loctup.t_self, snapshot,
									 HeapTupleHeaderGetXmin(loctup.t_data));

				}
				HeapCheckForSerializableConflictOut(valid, scan->rs_rd, &loctup,
													buffer, snapshot);
			}
		}
#else
		for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum = OffsetNumberNext(offnum))
		{
			ItemId		lp;
			HeapTupleData loctup;
			bool		valid;

			lp = PageGetItemId(dp, offnum);
			if (!ItemIdIsNormal(lp))
				continue;
			loctup.t_data = (HeapTupleHeader) PageGetItem((Page) dp, lp);
			loctup.t_len = ItemIdGetLength(lp);
			loctup.t_tableOid = rel_id;
			ItemPointerSet(&loctup.t_self, page, offnum);
			valid = HeapTupleSatisfiesVisibility(&loctup, snapshot, buffer);
			if (valid)
			{
				hscan->rs_vistuples[ntup++] = offnum;
				PredicateLockTID(scan->rs_rd, &loctup.t_self, snapshot,
								 HeapTupleHeaderGetXmin(loctup.t_data));
			}
			HeapCheckForSerializableConflictOut(valid, scan->rs_rd, &loctup,
												buffer, snapshot);
		}
#endif /* DIVA */
	}

#ifdef LOCATOR
#ifndef USING_LOCK
	if (siro)
		DRefDecrRefCnt(drefDesc.dual_ref, drefDesc.page_ref_unit);
	else
#endif
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
#else
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
#endif /* LOCATOR */

	Assert(ntup <= MaxHeapTuplesPerPage);
	hscan->rs_ntuples = ntup;

	return ntup > 0;
}

static bool
heapam_scan_bitmap_next_tuple(TableScanDesc scan,
							  TBMIterateResult *tbmres,
							  TupleTableSlot *slot)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	OffsetNumber targoffset;
	Page		dp;
	ItemId		lp;

#ifdef DIVA
	Relation relation;
	bool		siro;
#endif
	/*
	 * Out of range?  If so, nothing more to look at on this page
	 */
	if (hscan->rs_cindex < 0 || hscan->rs_cindex >= hscan->rs_ntuples)
		return false;

	targoffset = hscan->rs_vistuples[hscan->rs_cindex];
	dp = (Page) BufferGetPage(hscan->rs_cbuf);
	lp = PageGetItemId(dp, targoffset);
	Assert(ItemIdIsNormal(lp));

	hscan->rs_ctup.t_data = (HeapTupleHeader) PageGetItem((Page) dp, lp);
	hscan->rs_ctup.t_len = ItemIdGetLength(lp);
	hscan->rs_ctup.t_tableOid = scan->rs_rd->rd_id;
	ItemPointerSet(&hscan->rs_ctup.t_self, hscan->rs_cblock, targoffset);

	pgstat_count_heap_fetch(scan->rs_rd);

#ifdef DIVA
	relation = scan->rs_rd;
	siro = IsSiro(relation);

	if (siro)
	{
#ifdef DIVA
		ExecStoreBufferHeapTuple(hscan->rs_vistuples_copied[hscan->rs_cindex],
								 slot, InvalidBuffer);
		hscan->rs_vistuples_copied[hscan->rs_cindex] = NULL;
#else
		ExecStoreBufferHeapTuple(hscan->rs_vistuples_copied[hscan->rs_cindex],
								 slot,
								 hscan->rs_cbuf);
#endif /* DIVA */
	}
	else
		/*
		 * Set up the result slot to point to this tuple.  Note that the slot
		 * acquires a pin on the buffer.
		 */
		ExecStoreBufferHeapTuple(&hscan->rs_ctup,
								 slot,
								 hscan->rs_cbuf);
#else
	/*
	 * Set up the result slot to point to this tuple.  Note that the slot
	 * acquires a pin on the buffer.
	 */
	ExecStoreBufferHeapTuple(&hscan->rs_ctup,
							 slot,
							 hscan->rs_cbuf);
#endif

	hscan->rs_cindex++;

	return true;
}

static bool
heapam_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno;

	/* return false immediately if relation is empty */
	if (hscan->rs_nblocks == 0)
		return false;

	if (tsm->NextSampleBlock)
	{
		blockno = tsm->NextSampleBlock(scanstate, hscan->rs_nblocks);
		hscan->rs_cblock = blockno;
	}
	else
	{
		/* scanning table sequentially */

		if (hscan->rs_cblock == InvalidBlockNumber)
		{
			Assert(!hscan->rs_inited);
			blockno = hscan->rs_startblock;
		}
		else
		{
			Assert(hscan->rs_inited);

			blockno = hscan->rs_cblock + 1;

			if (blockno >= hscan->rs_nblocks)
			{
				/* wrap to beginning of rel, might not have started at 0 */
				blockno = 0;
			}

			/*
			 * Report our new scan position for synchronization purposes.
			 *
			 * Note: we do this before checking for end of scan so that the
			 * final state of the position hint is back at the start of the
			 * rel.  That's not strictly necessary, but otherwise when you run
			 * the same query multiple times the starting position would shift
			 * a little bit backwards on every invocation, which is confusing.
			 * We don't guarantee any specific ordering in general, though.
			 */
			if (scan->rs_flags & SO_ALLOW_SYNC)
				ss_report_location(scan->rs_rd, blockno);

			if (blockno == hscan->rs_startblock)
			{
				blockno = InvalidBlockNumber;
			}
		}
	}

	if (!BlockNumberIsValid(blockno))
	{
		if (BufferIsValid(hscan->rs_cbuf))
			ReleaseBuffer(hscan->rs_cbuf);
		hscan->rs_cbuf = InvalidBuffer;
		hscan->rs_cblock = InvalidBlockNumber;
		hscan->rs_inited = false;

		return false;
	}

	heapgetpage(scan, blockno);
	hscan->rs_inited = true;

	return true;
}

static bool
heapam_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
							  TupleTableSlot *slot)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno = hscan->rs_cblock;
	bool		pagemode = (scan->rs_flags & SO_ALLOW_PAGEMODE) != 0;

	Page		page;
	bool		all_visible;
	OffsetNumber maxoffset;

	/*
	 * When not using pagemode, we must lock the buffer during tuple
	 * visibility checks.
	 */
	if (!pagemode)
		LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

	page = (Page) BufferGetPage(hscan->rs_cbuf);
	all_visible = PageIsAllVisible(page) &&
		!scan->rs_snapshot->takenDuringRecovery;
	maxoffset = PageGetMaxOffsetNumber(page);

	for (;;)
	{
		OffsetNumber tupoffset;

		CHECK_FOR_INTERRUPTS();

		/* Ask the tablesample method which tuples to check on this page. */
		tupoffset = tsm->NextSampleTuple(scanstate,
										 blockno,
										 maxoffset);

		if (OffsetNumberIsValid(tupoffset))
		{
			ItemId		itemid;
			bool		visible;
			HeapTuple	tuple = &(hscan->rs_ctup);

			/* Skip invalid tuple pointers. */
			itemid = PageGetItemId(page, tupoffset);
			if (!ItemIdIsNormal(itemid))
				continue;

			tuple->t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple->t_len = ItemIdGetLength(itemid);
			ItemPointerSet(&(tuple->t_self), blockno, tupoffset);


			if (all_visible)
				visible = true;
			else
				visible = SampleHeapTupleVisible(scan, hscan->rs_cbuf,
												 tuple, tupoffset);

			/* in pagemode, heapgetpage did this for us */
			if (!pagemode)
				HeapCheckForSerializableConflictOut(visible, scan->rs_rd, tuple,
													hscan->rs_cbuf, scan->rs_snapshot);

			/* Try next tuple from same page. */
			if (!visible)
				continue;

			/* Found visible tuple, return it. */
			if (!pagemode)
				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			ExecStoreBufferHeapTuple(tuple, slot, hscan->rs_cbuf);

			/* Count successfully-fetched tuples as heap fetches */
			pgstat_count_heap_getnext(scan->rs_rd);

			return true;
		}
		else
		{
			/*
			 * If we get here, it means we've exhausted the items on this page
			 * and it's time to move to the next.
			 */
			if (!pagemode)
				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			ExecClearTuple(slot);
			return false;
		}
	}

	Assert(0);
}


/* ----------------------------------------------------------------------------
 *  Helper functions for the above.
 * ----------------------------------------------------------------------------
 */

/*
 * Reconstruct and rewrite the given tuple
 *
 * We cannot simply copy the tuple as-is, for several reasons:
 *
 * 1. We'd like to squeeze out the values of any dropped columns, both
 * to save space and to ensure we have no corner-case failures. (It's
 * possible for example that the new table hasn't got a TOAST table
 * and so is unable to store any large values of dropped cols.)
 *
 * 2. The tuple might not even be legal for the new table; this is
 * currently only known to happen as an after-effect of ALTER TABLE
 * SET WITHOUT OIDS.
 *
 * So, we must reconstruct the tuple from component Datums.
 */
static void
reform_and_rewrite_tuple(HeapTuple tuple,
						 Relation OldHeap, Relation NewHeap,
						 Datum *values, bool *isnull, RewriteState rwstate)
{
	TupleDesc	oldTupDesc = RelationGetDescr(OldHeap);
	TupleDesc	newTupDesc = RelationGetDescr(NewHeap);
	HeapTuple	copiedTuple;
	int			i;

	heap_deform_tuple(tuple, oldTupDesc, values, isnull);

	/* Be sure to null out any dropped columns */
	for (i = 0; i < newTupDesc->natts; i++)
	{
		if (TupleDescAttr(newTupDesc, i)->attisdropped)
			isnull[i] = true;
	}

	copiedTuple = heap_form_tuple(newTupDesc, values, isnull);

	/* The heap rewrite module does the rest */
	rewrite_heap_tuple(rwstate, tuple, copiedTuple);

	heap_freetuple(copiedTuple);
}

/*
 * Check visibility of the tuple.
 */
static bool
SampleHeapTupleVisible(TableScanDesc scan, Buffer buffer,
					   HeapTuple tuple,
					   OffsetNumber tupoffset)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;

	if (scan->rs_flags & SO_ALLOW_PAGEMODE)
	{
		/*
		 * In pageatatime mode, heapgetpage() already did visibility checks,
		 * so just look at the info it left in rs_vistuples[].
		 *
		 * We use a binary search over the known-sorted array.  Note: we could
		 * save some effort if we insisted that NextSampleTuple select tuples
		 * in increasing order, but it's not clear that there would be enough
		 * gain to justify the restriction.
		 */
		int			start = 0,
					end = hscan->rs_ntuples - 1;

		while (start <= end)
		{
			int			mid = (start + end) / 2;
			OffsetNumber curoffset = hscan->rs_vistuples[mid];

			if (tupoffset == curoffset)
				return true;
			else if (tupoffset < curoffset)
				end = mid - 1;
			else
				start = mid + 1;
		}

		return false;
	}
	else
	{
		/* Otherwise, we have to check the tuple individually. */
		return HeapTupleSatisfiesVisibility(tuple, scan->rs_snapshot,
											buffer);
	}
}


/* ------------------------------------------------------------------------
 * Definition of the heap table access method.
 * ------------------------------------------------------------------------
 */

static const TableAmRoutine heapam_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = heapam_slot_callbacks,

	.scan_begin = heap_beginscan,
	.scan_end = heap_endscan,
	.scan_rescan = heap_rescan,
	.scan_getnextslot = heap_getnextslot,
#ifdef DIVA
	.scan_getnextslot_from_ebi = ebi_getnextslot,
#endif

	.scan_set_tidrange = heap_set_tidrange,
	.scan_getnextslot_tidrange = heap_getnextslot_tidrange,

	.parallelscan_estimate = table_block_parallelscan_estimate,
	.parallelscan_initialize = table_block_parallelscan_initialize,
	.parallelscan_reinitialize = table_block_parallelscan_reinitialize,

	.index_fetch_begin = heapam_index_fetch_begin,
	.index_fetch_reset = heapam_index_fetch_reset,
	.index_fetch_end = heapam_index_fetch_end,
	.index_fetch_tuple = heapam_index_fetch_tuple,

	.tuple_insert = heapam_tuple_insert,
	.tuple_insert_speculative = heapam_tuple_insert_speculative,
	.tuple_complete_speculative = heapam_tuple_complete_speculative,
	.multi_insert = heap_multi_insert,
	.tuple_delete = heapam_tuple_delete,
	.tuple_update = heapam_tuple_update,
	.tuple_lock = heapam_tuple_lock,

	.tuple_fetch_row_version = heapam_fetch_row_version,
#ifdef DIVA
	.tuple_search_row_version = heapam_search_row_version,
#endif /* DIVA */
	.tuple_get_latest_tid = heap_get_latest_tid,
	.tuple_tid_valid = heapam_tuple_tid_valid,
	.tuple_satisfies_snapshot = heapam_tuple_satisfies_snapshot,
	.index_delete_tuples = heap_index_delete_tuples,

	.relation_set_new_filenode = heapam_relation_set_new_filenode,
	.relation_nontransactional_truncate = heapam_relation_nontransactional_truncate,
	.relation_copy_data = heapam_relation_copy_data,
	.relation_copy_for_cluster = heapam_relation_copy_for_cluster,
	.relation_vacuum = heap_vacuum_rel,
	.scan_analyze_next_block = heapam_scan_analyze_next_block,
	.scan_analyze_next_tuple = heapam_scan_analyze_next_tuple,
	.index_build_range_scan = heapam_index_build_range_scan,
	.index_validate_scan = heapam_index_validate_scan,

	.relation_size = table_block_relation_size,
	.relation_needs_toast_table = heapam_relation_needs_toast_table,
	.relation_toast_am = heapam_relation_toast_am,
	.relation_fetch_toast_slice = heap_fetch_toast_slice,

	.relation_estimate_size = heapam_estimate_rel_size,

	.scan_bitmap_next_block = heapam_scan_bitmap_next_block,
	.scan_bitmap_next_tuple = heapam_scan_bitmap_next_tuple,
	.scan_sample_next_block = heapam_scan_sample_next_block,
	.scan_sample_next_tuple = heapam_scan_sample_next_tuple
};


const TableAmRoutine *
GetHeapamTableAmRoutine(void)
{
	return &heapam_methods;
}

Datum
heap_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&heapam_methods);
}

#ifdef LOCATOR
/* ----------------------------------------------------------------------------
 *  Functions for manipulations of physical tuples for heap AM.
 * ----------------------------------------------------------------------------
 */

static void
locatoram_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
					int options, BulkInsertState bistate)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* Update the tuple with table oid */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;
	tuple->t_locator_route_synopsis = slot->tts_locator_route_synopsis;

	Assert(relation->rd_locator_level_count == slot->lvl_cnt);

	/* Perform the insertion, and copy the resulting ItemPointer */
	locator_insert(relation, tuple, cid, options, bistate);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

/* ------------------------------------------------------------------------
 * Definition of the heap table access method.
 * ------------------------------------------------------------------------
 */
static const TableAmRoutine locatoram_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = heapam_slot_callbacks,

	.scan_begin = locator_beginscan,
	.scan_end = locator_endscan,
	.scan_rescan = locator_rescan,
	.scan_getnextslot = locator_getnextslot,
	.scan_getnextslot_from_ebi = ebi_getnextslot,

	.scan_set_tidrange = heap_set_tidrange,
	.scan_getnextslot_tidrange = heap_getnextslot_tidrange,

	.parallelscan_estimate = table_block_parallelscan_estimate,
	.parallelscan_initialize = table_block_parallelscan_initialize,
	.parallelscan_reinitialize = table_block_parallelscan_reinitialize,

	.index_fetch_begin = heapam_index_fetch_begin,
	.index_fetch_reset = heapam_index_fetch_reset,
	.index_fetch_end = heapam_index_fetch_end,
	.index_fetch_tuple = locatoram_index_fetch_tuple,

	.tuple_insert = locatoram_tuple_insert,
	.tuple_insert_speculative = heapam_tuple_insert_speculative,
	.tuple_complete_speculative = heapam_tuple_complete_speculative,
	.multi_insert = heap_multi_insert,
	.tuple_delete = heapam_tuple_delete,
	.tuple_update = locatoram_tuple_update,

	.tuple_lock = heapam_tuple_lock,

	.tuple_fetch_row_version = heapam_fetch_row_version,
	.tuple_search_row_version = locatoram_search_row_version,

	.tuple_get_latest_tid = heap_get_latest_tid,
	.tuple_tid_valid = heapam_tuple_tid_valid,
	.tuple_satisfies_snapshot = heapam_tuple_satisfies_snapshot,
	.index_delete_tuples = heap_index_delete_tuples,

	.relation_set_new_filenode = heapam_relation_set_new_filenode,
	.relation_nontransactional_truncate = heapam_relation_nontransactional_truncate,
	.relation_copy_data = heapam_relation_copy_data,
	.relation_copy_for_cluster = heapam_relation_copy_for_cluster,
	.relation_vacuum = heap_vacuum_rel,
	.scan_analyze_next_block = heapam_scan_analyze_next_block,
	.scan_analyze_next_tuple = heapam_scan_analyze_next_tuple,
	.index_build_range_scan = heapam_index_build_range_scan,
	.index_validate_scan = heapam_index_validate_scan,

	.relation_size = table_block_relation_size,
	.relation_needs_toast_table = heapam_relation_needs_toast_table,
	.relation_toast_am = heapam_relation_toast_am,
	.relation_fetch_toast_slice = heap_fetch_toast_slice,

	.relation_estimate_size = heapam_estimate_rel_size,

	.scan_bitmap_next_block = heapam_scan_bitmap_next_block,
	.scan_bitmap_next_tuple = heapam_scan_bitmap_next_tuple,
	.scan_sample_next_block = heapam_scan_sample_next_block,
	.scan_sample_next_tuple = heapam_scan_sample_next_tuple
};

const TableAmRoutine *
GetLocatoramTableAmRoutine(void)
{
	return &locatoram_methods;
}

Datum
locatoram_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&locatoram_methods);
}

#else

Datum
locatoram_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_NULL();
}

#endif /* LOCATOR */
