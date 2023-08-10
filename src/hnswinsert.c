#include "postgres.h"

#include <math.h>

#include "hnsw.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"

/*
 * Get the insert page
 */
static BlockNumber
GetInsertPage(Relation index)
{
	Buffer		buf;
	Page		page;
	HnswMetaPage metap;
	BlockNumber insertPage;

	buf = ReadBuffer(index, HNSW_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	metap = HnswPageGetMeta(page);

	insertPage = metap->insertPage;

	UnlockReleaseBuffer(buf);

	return insertPage;
}

/*
 * Check for a free offset
 */
static bool
HnswFreeOffset(Relation index, Buffer buf, Page page, HnswElement element, Size ntupSize, Buffer *nbuf, Page *npage, OffsetNumber *freeOffno, OffsetNumber *freeNeighborOffno, BlockNumber *firstFreePage)
{
	OffsetNumber offno;
	OffsetNumber maxoffno = PageGetMaxOffsetNumber(page);

	for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
	{
		HnswElementTuple etup = (HnswElementTuple) PageGetItem(page, PageGetItemId(page, offno));

		/* Skip neighbor tuples */
		if (!HnswIsElementTuple(etup))
			continue;

		if (etup->deleted)
		{
			BlockNumber neighborPage = ItemPointerGetBlockNumber(&etup->neighbortid);
			OffsetNumber neighborOffno = ItemPointerGetOffsetNumber(&etup->neighbortid);
			ItemId		itemid;

			if (!BlockNumberIsValid(*firstFreePage))
				*firstFreePage = neighborPage;

			if (neighborPage == BufferGetBlockNumber(buf))
			{
				*nbuf = buf;
				*npage = page;
			}
			else
			{
				*nbuf = ReadBuffer(index, neighborPage);
				LockBuffer(*nbuf, BUFFER_LOCK_EXCLUSIVE);

				/* Skip WAL for now */
				*npage = BufferGetPage(*nbuf);
			}

			itemid = PageGetItemId(*npage, neighborOffno);

			/* Check for space on neighbor tuple page */
			if (PageGetFreeSpace(*npage) + ItemIdGetLength(itemid) - sizeof(ItemIdData) >= ntupSize)
			{
				*freeOffno = offno;
				*freeNeighborOffno = neighborOffno;
				return true;
			}
			else if (*nbuf != buf)
				UnlockReleaseBuffer(*nbuf);
		}
	}

	return false;
}

/*
 * Add a new page
 */
static void
HnswInsertAppendPage(Relation index, Buffer *nbuf, Page *npage, GenericXLogState *state, Page page)
{
	/* Add a new page */
	LockRelationForExtension(index, ExclusiveLock);
	*nbuf = HnswNewBuffer(index, MAIN_FORKNUM);
	UnlockRelationForExtension(index, ExclusiveLock);

	/* Init new page */
	*npage = GenericXLogRegisterBuffer(state, *nbuf, GENERIC_XLOG_FULL_IMAGE);
	HnswInitPage(*nbuf, *npage);

	/* Update previous buffer */
	HnswPageGetOpaque(page)->nextblkno = BufferGetBlockNumber(*nbuf);
}

/*
 * Add to element and neighbor pages
 */
static void
WriteNewElementPages(Relation index, HnswElement e, int m)
{
	Buffer		buf;
	Page		page;
	GenericXLogState *state;
	Size		etupSize;
	Size		ntupSize;
	Size		combinedSize;
	HnswElementTuple etup;
	BlockNumber insertPage = GetInsertPage(index);
	BlockNumber originalInsertPage = insertPage;
	int			dimensions = e->vec->dim;
	HnswNeighborTuple ntup;
	Buffer		nbuf;
	Page		npage;
	OffsetNumber freeOffno = InvalidOffsetNumber;
	OffsetNumber freeNeighborOffno = InvalidOffsetNumber;
	BlockNumber firstFreePage = InvalidBlockNumber;

	/* Calculate sizes */
	etupSize = HNSW_ELEMENT_TUPLE_SIZE(dimensions);
	ntupSize = HNSW_NEIGHBOR_TUPLE_SIZE(e->level, m);
	combinedSize = etupSize + ntupSize + sizeof(ItemIdData);

	/* Prepare element tuple */
	etup = palloc0(etupSize);
	HnswSetElementTuple(etup, e);

	/* Prepare neighbor tuple */
	ntup = palloc0(ntupSize);
	HnswSetNeighborTuple(ntup, e, m);

	/* Find a page to insert the item */
	for (;;)
	{
		buf = ReadBuffer(index, insertPage);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buf, 0);

		/* Space for both */
		if (PageGetFreeSpace(page) >= combinedSize)
		{
			nbuf = buf;
			npage = page;
			break;
		}

		/* Space for element but not neighbors and last page */
		if (PageGetFreeSpace(page) >= etupSize && !BlockNumberIsValid(HnswPageGetOpaque(page)->nextblkno))
		{
			HnswInsertAppendPage(index, &nbuf, &npage, state, page);
			break;
		}

		/* Space from deleted item */
		if (HnswFreeOffset(index, buf, page, e, ntupSize, &nbuf, &npage, &freeOffno, &freeNeighborOffno, &firstFreePage))
		{
			if (nbuf != buf)
				npage = GenericXLogRegisterBuffer(state, nbuf, 0);

			break;
		}

		insertPage = HnswPageGetOpaque(page)->nextblkno;

		if (BlockNumberIsValid(insertPage))
		{
			/* Move to next page */
			GenericXLogAbort(state);
			UnlockReleaseBuffer(buf);
		}
		else
		{
			Buffer		newbuf;
			Page		newpage;

			HnswInsertAppendPage(index, &newbuf, &newpage, state, page);

			/* Commit */
			MarkBufferDirty(newbuf);
			MarkBufferDirty(buf);
			GenericXLogFinish(state);

			/* Unlock previous buffer */
			UnlockReleaseBuffer(buf);

			/* Prepare new buffer */
			state = GenericXLogStart(index);
			buf = newbuf;
			page = GenericXLogRegisterBuffer(state, buf, 0);

			/* Create new page for neighbors if needed */
			if (PageGetFreeSpace(page) < combinedSize)
				HnswInsertAppendPage(index, &nbuf, &npage, state, page);
			else
			{
				nbuf = buf;
				npage = page;
			}

			break;
		}
	}

	e->blkno = BufferGetBlockNumber(buf);
	e->neighborPage = BufferGetBlockNumber(nbuf);

	insertPage = e->neighborPage;

	if (OffsetNumberIsValid(freeOffno))
	{
		e->offno = freeOffno;
		e->neighborOffno = freeNeighborOffno;
	}
	else
	{
		e->offno = OffsetNumberNext(PageGetMaxOffsetNumber(page));
		if (nbuf == buf)
			e->neighborOffno = OffsetNumberNext(e->offno);
		else
			e->neighborOffno = FirstOffsetNumber;
	}

	ItemPointerSet(&etup->neighbortid, e->neighborPage, e->neighborOffno);

	/* Add element and neighbors */
	if (OffsetNumberIsValid(freeOffno))
	{
		if (!PageIndexTupleOverwrite(page, e->offno, (Item) etup, etupSize))
			elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

		if (!PageIndexTupleOverwrite(npage, e->neighborOffno, (Item) ntup, ntupSize))
			elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));
	}
	else
	{
		if (PageAddItem(page, (Item) etup, etupSize, InvalidOffsetNumber, false, false) != e->offno)
			elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

		if (PageAddItem(npage, (Item) ntup, ntupSize, InvalidOffsetNumber, false, false) != e->neighborOffno)
			elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));
	}

	/* Commit */
	MarkBufferDirty(buf);
	if (nbuf != buf)
		MarkBufferDirty(nbuf);
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
	if (nbuf != buf)
		UnlockReleaseBuffer(nbuf);

	/* Update the insert page */
	if (insertPage != originalInsertPage && (!OffsetNumberIsValid(freeOffno) || firstFreePage == insertPage))
		HnswUpdateMetaPage(index, false, NULL, insertPage, MAIN_FORKNUM);
}

/*
 * Update neighbors
 */
static void
UpdateNeighborPages(Relation index, FmgrInfo *procinfo, Oid collation, HnswElement e, int m)
{
	for (int lc = e->level; lc >= 0; lc--)
	{
		int			lm = HnswGetLayerM(m, lc);
		HnswNeighborArray *neighbors = &e->neighbors[lc];

		for (int i = 0; i < neighbors->length; i++)
		{
			HnswCandidate *hc = &neighbors->items[i];
			Buffer		buf;
			Page		page;
			GenericXLogState *state;
			ItemId		itemid;
			HnswNeighborTuple ntup;
			Size		ntupSize;
			int			idx = -1;
			int			startIdx;
			OffsetNumber offno = hc->element->neighborOffno;

			/* Get latest neighbors */
			HnswLoadNeighbors(hc->element, index);

			HnswUpdateConnection(e, hc, lm, lc, &idx, index, procinfo, collation);

			if (idx == -1)
				continue;

			/* Register page */
			buf = ReadBuffer(index, hc->element->neighborPage);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			state = GenericXLogStart(index);
			page = GenericXLogRegisterBuffer(state, buf, 0);

			/* Get tuple */
			itemid = PageGetItemId(page, offno);
			ntup = (HnswNeighborTuple) PageGetItem(page, itemid);
			ntupSize = ItemIdGetLength(itemid);

			/* Calculate index for update */
			startIdx = (hc->element->level - lc) * m;

			if (idx == -2)
			{
				/* Find free offset if still exists */
				/* TODO Retry updating connections if not */
				for (int i = 0; i < lm; i++)
					if (!ItemPointerIsValid(&ntup->indextids[startIdx + i]))
						idx = startIdx + i;
			}
			else
				idx += startIdx;

			/* Make robust to issues */
			if (idx >= 0 && idx < ntup->count)
			{
				ItemPointer indextid = &ntup->indextids[idx];

				/* Update neighbor */
				ItemPointerSet(indextid, e->blkno, e->offno);

				/* Overwrite tuple */
				if (!PageIndexTupleOverwrite(page, offno, (Item) ntup, ntupSize))
					elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

				/* Commit */
				MarkBufferDirty(buf);
				GenericXLogFinish(state);
			}
			else
				GenericXLogAbort(state);

			UnlockReleaseBuffer(buf);
		}
	}
}

/*
 * Add a heap TID to an existing element
 */
static bool
HnswAddDuplicate(Relation index, HnswElement element, HnswElement dup)
{
	Buffer		buf;
	Page		page;
	GenericXLogState *state;
	Size		etupSize = HNSW_ELEMENT_TUPLE_SIZE(dup->vec->dim);
	HnswElementTuple etup;
	int			i;

	/* Read page */
	buf = ReadBuffer(index, dup->blkno);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buf, 0);

	/* Find space */
	etup = (HnswElementTuple) PageGetItem(page, PageGetItemId(page, dup->offno));
	for (i = 0; i < HNSW_HEAPTIDS; i++)
	{
		if (!ItemPointerIsValid(&etup->heaptids[i]))
			break;
	}

	/* Either being deleted or we lost our chance to another backend */
	if (i == 0 || i == HNSW_HEAPTIDS)
	{
		GenericXLogAbort(state);
		UnlockReleaseBuffer(buf);
		return false;
	}

	/* Add heap TID */
	etup->heaptids[i] = *((ItemPointer) linitial(element->heaptids));

	/* Overwrite tuple */
	if (!PageIndexTupleOverwrite(page, dup->offno, (Item) etup, etupSize))
		elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

	/* Commit */
	MarkBufferDirty(buf);
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);

	return true;
}

/*
 * Write changes to disk
 */
static void
WriteElement(Relation index, FmgrInfo *procinfo, Oid collation, HnswElement element, int m, int efConstruction, HnswElement dup, HnswElement entryPoint)
{
	/* Try to add to existing page */
	if (dup != NULL)
	{
		if (HnswAddDuplicate(index, element, dup))
			return;
	}

	/* If fails, take this path */
	WriteNewElementPages(index, element, m);
	UpdateNeighborPages(index, procinfo, collation, element, m);

	/* Update metapage if needed */
	if (entryPoint == NULL || element->level > entryPoint->level)
	{
		/* TODO Lock metapage for entire block */
		HnswElement newEntryPoint = HnswGetEntryPoint(index);

		if (entryPoint == NULL && newEntryPoint != NULL)
		{
			/* Try again with new entry point */
			HnswInsertElement(element, newEntryPoint, index, procinfo, collation, m, efConstruction, false);
			UpdateNeighborPages(index, procinfo, collation, element, m);
		}
		else
			HnswUpdateMetaPage(index, true, element, InvalidBlockNumber, MAIN_FORKNUM);
	}
}

/*
 * Insert a tuple into the index
 */
bool
HnswInsertTuple(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heapRel)
{
	Datum		value;
	FmgrInfo   *normprocinfo;
	HnswElement entryPoint;
	HnswElement element;
	int			m = HnswGetM(index);
	int			efConstruction = HnswGetEfConstruction(index);
	double		ml = HnswGetMl(m);
	FmgrInfo   *procinfo = index_getprocinfo(index, 1, HNSW_DISTANCE_PROC);
	Oid			collation = index->rd_indcollation[0];
	HnswElement dup;

	/* Detoast once for all calls */
	value = PointerGetDatum(PG_DETOAST_DATUM(values[0]));

	/* Normalize if needed */
	normprocinfo = HnswOptionalProcInfo(index, HNSW_NORM_PROC);
	if (normprocinfo != NULL)
	{
		if (!HnswNormValue(normprocinfo, collation, &value, NULL))
			return false;
	}

	/* Create an element */
	element = HnswInitElement(heap_tid, m, ml, HnswGetMaxLevel(m));
	element->vec = DatumGetVector(value);

	/* Get entry point */
	entryPoint = HnswGetEntryPoint(index);

	/* Insert element in graph */
	HnswInsertElement(element, entryPoint, index, procinfo, collation, m, efConstruction, false);

	/* Look for duplicate */
	dup = HnswFindDuplicate(element);

	/* Write to disk */
	WriteElement(index, procinfo, collation, element, m, efConstruction, dup, entryPoint);

	return true;
}

/*
 * Insert a tuple into the index
 */
bool
hnswinsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid,
		   Relation heap, IndexUniqueCheck checkUnique
#if PG_VERSION_NUM >= 140000
		   ,bool indexUnchanged
#endif
		   ,IndexInfo *indexInfo
)
{
	MemoryContext oldCtx;
	MemoryContext insertCtx;

	/* Skip nulls */
	if (isnull[0])
		return false;

	/* Create memory context */
	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "Hnsw insert temporary context",
									  ALLOCSET_DEFAULT_SIZES);
	oldCtx = MemoryContextSwitchTo(insertCtx);

	/* Insert tuple */
	HnswInsertTuple(index, values, isnull, heap_tid, heap);

	/* Delete memory context */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	return false;
}