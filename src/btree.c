#include "btreeInt.h"
#include "vdbeInt.h"
#include <kvstore/kvs_base.h>
#undef NDEBUG // TODO
#include <kvstore/kvs_schema.h>
#define NDEBUG 1

#if 0
#define LOG(fmt,...)   sqlite3DebugPrintf("%s:%d " fmt "\n", __func__, __LINE__, __VA_ARGS__)
#else
#define LOG(fmt,...)	((void)0)
#endif

/*
 * Globals are protected by the static "open" mutex (SQLITE_MUTEX_STATIC_OPEN).
 */

/* The head of the linked list of shared Btree objects */
struct BtShared *sqlite3SharedCacheList = NULL;

/* rowid is an 8 byte int */
#define ROWIDMAXSIZE	10

#ifndef SQLITE_DEFAULT_FILE_PERMISSIONS
#define SQLITE_DEFAULT_FILE_PERMISSIONS	0644
#endif

#ifndef SQLITE_DEFAULT_PROXYDIR_PERMISSIONS
#define SQLITE_DEFAULT_PROXYDIR_PERMISSIONS	0755
#endif

#define LOCKSUFF	"-lock"

#define	BT_MAX_PATH	512

#define MDB_MAXKEYSIZE 511

// TODO: Supposedly these are synonyms
#define BTREE_ZERODATA BTREE_BLOBKEY

// TODO: MASSIVE HACKS
static void kvs_bind_sint64(KVS_val *const val, int64_t const x) {
	kvs_bind_uint64(val, x >= 0);
	kvs_bind_uint64(val, x >= 0 ? x : -x);
}
static int64_t kvs_read_sint64(KVS_val *const val) {
	uint64_t a = kvs_read_uint64(val);
	uint64_t b = kvs_read_uint64(val);
	kvs_assert(a <= 1);
	kvs_assert(b <= INT64_MAX);
	return a ? (int64_t)b : -(int64_t)b;
}
static void kvs_bind_double(KVS_val *const val, double const x) {
	kvs_bind_sint64(val, 1000000000.0*x);
};

enum {
	// 0-19 are reserved
	TABLE_META = 20,
	TABLE_SCHEMA = 21,
	// 22-29 are reserved
	TABLE_START = 30,
};

static int errmap(int err)
{
  switch(err) {
  case 0:
	return SQLITE_OK;
  case EACCES:
	return SQLITE_READONLY;
  case EIO:
  case KVS_PANIC:
    return SQLITE_IOERR;
  case EPERM:
    return SQLITE_PERM;
  case ENOMEM:
    return SQLITE_NOMEM;
  case ENOENT:
    return SQLITE_CANTOPEN;
  case ENOSPC:
  case KVS_MAP_FULL:
    return SQLITE_FULL;
  case KVS_NOTFOUND:
    return SQLITE_NOTFOUND;
  case KVS_VERSION_MISMATCH:
  case KVS_INVALID:
    return SQLITE_NOTADB;
//  case KVS_PAGE_NOTFOUND:
  case KVS_CORRUPTED:
    return SQLITE_CORRUPT_BKPT;
//  case KVS_INCOMPATIBLE:
//    return SQLITE_SCHEMA;
//  case KVS_BAD_RSLOT:
//    return SQLITE_MISUSE;
  case KVS_BAD_TXN:
    return SQLITE_ABORT;
  case KVS_BAD_VALSIZE:
    return SQLITE_TOOBIG;
  default:
    return SQLITE_INTERNAL;
  }
}

#define BTREE_TABLE_RANGE(range, table) \
	KVS_RANGE_STORAGE(range, KVS_VARINT_MAX); \
	kvs_bind_uint64((range)->min, TABLE_START + (table)); \
	kvs_range_genmax((range));

static int BtreeTableSchema(Btree *const p, int const iTable, int *const flags) {
	assert(p);
	assert(iTable >= 0);
	assert(flags);
	if(1 == iTable) { // Table 1 is auto-generated.
		*flags = BTREE_INTKEY;
		return 0;
	}
	KVS_val key[1], val[1];
	KVS_VAL_STORAGE(key, KVS_VARINT_MAX*2);
	kvs_bind_uint64(key, TABLE_SCHEMA);
	kvs_bind_uint64(key, iTable);
	int rc = kvs_get(p->curr_txn, key, val);
	if(rc < 0) return errmap(rc);
	uint64_t x = kvs_read_uint64(val);
	assert(x <= INT_MAX);
	*flags = x;
	return 0;
}

/*
** Start a statement subtransaction. The subtransaction can can be rolled
** back independently of the main transaction. You must start a transaction 
** before starting a subtransaction. The subtransaction is ended automatically 
** if the main transaction commits or rolls back.
**
** Statement subtransactions are used around individual SQL statements
** that are contained within a BEGIN...COMMIT block.  If a constraint
** error occurs within the statement, the effect of that one statement
** can be rolled back without having to rollback the entire transaction.
**
** A statement sub-transaction is implemented as an anonymous savepoint. The
** value passed as the second parameter is the total number of savepoints,
** including the new anonymous savepoint, open on the B-Tree. i.e. if there
** are no active savepoints and no other statement-transactions open,
** iStatement is 1. This anonymous savepoint can be released or rolled back
** using the sqlite3BtreeSavepoint() function.
*/
int sqlite3BtreeBeginStmt(Btree *p, int iStatement){
  KVS_txn *txn;
  BtShared *pBt = p->pBt;
  int rc;
  sqlite3BtreeEnter(p);
  assert( p->inTrans==TRANS_WRITE );
  assert( iStatement>0 );
  assert( iStatement>p->db->nSavepoint );
  assert( pBt->inTransaction==TRANS_WRITE );
  /* At the pager level, a statement transaction is a savepoint with
  ** an index greater than all savepoints created explicitly using
  ** SQL statements. It is illegal to open, release or rollback any
  ** such savepoints while the statement transaction savepoint is active.
  */
  rc = kvs_txn_begin(pBt->env, p->curr_txn, 0, &txn);
  if (rc == 0)
	p->curr_txn = txn;
  sqlite3BtreeLeave(p);
  LOG("rc=%d",rc);
  return errmap(rc);
}

/*
** Attempt to start a new transaction. A write-transaction
** is started if the second argument is nonzero, otherwise a read-
** transaction.  If the second argument is 2 or more an exclusive
** transaction is started, meaning that no other process is allowed
** to access the database.  A preexisting transaction may not be
** upgraded to exclusive by calling this routine a second time - the
** exclusivity flag only works for a new transaction.
**
** A write-transaction must be started before attempting any 
** changes to the database.  None of the following routines 
** will work unless a transaction is started first:
**
**      sqlite3BtreeCreateTable()
**      sqlite3BtreeCreateIndex()
**      sqlite3BtreeClearTable()
**      sqlite3BtreeDropTable()
**      sqlite3BtreeInsert()
**      sqlite3BtreeDelete()
**      sqlite3BtreeUpdateMeta()
*/
int sqlite3BtreeBeginTrans(Btree *p, int wrflag){
	KVS_txn *txn, *rtxn = NULL;
	BtShared *pBt = p->pBt;
	int rc = SQLITE_OK;

	if ((p->inTrans == TRANS_WRITE) || (p->inTrans == TRANS_READ && !wrflag))
		goto done;

	/* If we already started a read txn and now want to write,
	* we need to cleanup the read txn
	*/
	if (p->inTrans == TRANS_READ && wrflag)
		rtxn = p->main_txn;

	rc = kvs_txn_begin(pBt->env, NULL, wrflag ? 0 : KVS_RDONLY, &txn);
	if (rc < 0) goto done;
	if (wrflag) {
		p->inTrans = TRANS_WRITE;
		if (rtxn) {
			KVS_val key[1];
			BtCursor *pCur;
			int rc2;
			/* move all existing cursors to new transaction */
			for (pCur = p->pCursor; pCur; pCur = pCur->pNext) {
				KVS_cursor *mc = pCur->cursor;
				rc2 = kvs_cursor_get(mc, key, NULL, KVS_GET_CURRENT);
				kvs_cursor_close(mc);
				int rc3 = kvs_cursor_open(txn, &mc);
				pCur->cursor = mc;
				if (0 == rc2)
					kvs_cursor_get(mc, key, NULL, KVS_SET);
			}
			kvs_txn_abort(rtxn);
		}
	} else {
		p->inTrans = TRANS_READ;
	}
	p->main_txn = txn;
	p->curr_txn = txn;

done:
	LOG("rc=%d",rc);
	return errmap(rc);
}

#ifndef SQLITE_OMIT_INCRBLOB
/*
** Argument pCsr must be a cursor opened for writing on an 
** INTKEY table currently pointing at a valid table entry. 
** This function modifies the data stored as part of that entry.
**
** Only the data content may be modified, it is not possible to 
** change the length of the data stored. If this function is called with
** parameters that attempt to write past the end of the existing data,
** no modifications are made and SQLITE_CORRUPT is returned.
*/
int sqlite3BtreePutData(BtCursor *pCur, u32 offset, u32 amt, void *z){
	KVS_cursor *const mc = pCur->cursor;
	KVS_val key[1], val[1], put[1];
	unsigned char *buf = NULL;
	int flags;
	int rc = BtreeTableSchema(pCur->pBtree, pCur->iTable, &flags);
	if(rc < 0) goto cleanup;
	if(!(BTREE_INTKEY & flags)) rc = KVS_PANIC;
	if(rc < 0) goto cleanup;

	rc = kvs_cursor_current(mc, key, val);
	if(KVS_NOTFOUND == rc) rc = KVS_PANIC;
	if(rc < 0) goto cleanup;

	uint64_t nKey = kvs_read_uint64(val); // Consumes part of val
	if(nKey+offset+amt > val->size) rc = KVS_CORRUPTED;
	if(rc < 0) goto cleanup;

	buf = sqlite3_malloc(KVS_VARINT_MAX+val->size);
	if(!buf) rc = KVS_ENOMEM;
	if(rc < 0) goto cleanup;
	put->data = buf;
	put->size = 0;
	kvs_bind_uint64(put, nKey);
	memcpy((char *)put->data+put->size, val->data, val->size);
	memcpy((char *)put->data+put->size+nKey+offset, z, amt);
	put->size += val->size;
	rc = kvs_cursor_put(mc, key, put, 0); // KVS_GET_CURRENT
cleanup:
	sqlite3_free(buf);
	LOG("rc=%d", rc);
	return errmap(rc);
}

/* 
** Set a flag on this cursor to cache the locations of pages from the 
** overflow list for the current row. This is used by cursors opened
** for incremental blob IO only.
*/
void sqlite3BtreeCacheOverflow(BtCursor *pCur){
  LOG("done",0);
}
#endif

#ifndef SQLITE_OMIT_WAL
/*
** Run a checkpoint on the Btree passed as the first argument.
**
** Return SQLITE_LOCKED if this or any other connection has an open 
** transaction on the shared-cache the argument Btree is connected to.
**
** Parameter eMode is one of SQLITE_CHECKPOINT_PASSIVE, FULL or RESTART.
*/
int sqlite3BtreeCheckpoint(Btree *p, int eMode, int *pnLog, int *pnCkpt){
  int rc = 0;
/*  if( p ){
    BtShared *pBt = p->pBt;
	rc = mdb_env_sync(pBt->env, 1);
  }*/
  LOG("rc=%d",rc);
  return errmap(rc);
}
#endif

/*
** Clear the current cursor position.
*/
void sqlite3BtreeClearCursor(BtCursor *pCur){
  KVS_cursor *const mc = pCur->cursor;
  kvs_cursor_clear(mc);
  LOG("done",0);
}

#if 0
static int BtreeCompare0(void *ctx, KVS_val const *const a, KVS_val const *const b)
{
	Btree *const p = ctx;
	BtCursor *const pCur = p->active; // TODO: Even this doesn't work because leveldb has to compact (compare) in the background?
	assert(pCur);
	struct KeyInfo *const pKeyInfo = pCur->pKeyInfo;
	unsigned char aSpace[1024];
	char *pFree = NULL;
	UnpackedRecord *p = sqlite3VdbeAllocUnpackedRecord(pKeyInfo, aSpace, sizeof(aSpace), &pFree);
	if(!p) return SQLITE_NOMEM;
	sqlite3VdbeRecordUnpack(pKeyInfo, (int)b->mv_size, b->mv_data, p);
	sqlite3_free(pFree);
	return sqlite3VdbeRecordCompare(a->mv_size, a->mv_data, p);
}
static int BtreeCompare(void *ctx, KVS_val const *const a, KVS_val const *const b)
{
	assert(a->mv_size >= 1);
	assert(b->mv_size >= 1);
	unsigned char const *const aa = a->mv_data;
	unsigned char const *const bb = b->mv_data;
	if(aa[0] < bb[0]) return +1;
	if(aa[0] > bb[0]) return -1;
	if(aa[0] & KEY_SORT_SQLITE) {
		KVS_val const a2 = {a->mv_size-1, aa+1};
		KVS_val const b2 = {b->mv_size-1, bb+1};
		return BtreeCompare0(ctx, &a2, &b2);
	} else {
		int x = memcmp(aa, bb, MIN(a->size, b->size));
		if(0 != x) return x;
		if(a->size < b->size) return -1;
		if(a->size > b->size) return +1;
		return 0;
	}
}

static int BtreeTableHandle(Btree *p, int iTable, MDB_dbi *dbi)
{
  char name[13], *nptr;
  int rc;

  if (iTable == 1 && !p->curr_txn->mt_dbs[MAIN_DBI].md_entries) {
	iTable = 0;
	nptr = NULL;
  } else {
	nptr = name;
	sprintf(name, "Tab.%08x", iTable);
  }
  rc = mdb_dbi_open(p->curr_txn, nptr, 0, dbi);
//  if (!rc && (p->curr_txn->mt_dbs[*dbi].md_flags & MDB_DUPSORT)) {
//	  mdb_set_compare(p->curr_txn, *dbi, BtreeCompare);
//  }
  return errmap(rc);
}
#endif

/*
** Delete all information from a single table in the database.  iTable is
** the page number of the root of the table.  After this routine returns,
** the root page is empty, but still exists.
**
** This routine will fail with SQLITE_LOCKED if there are any open
** read cursors on the table.  Open write cursors are moved to the
** root of the table.
**
** If pnChange is not NULL, then table iTable must be an intkey table. The
** integer value pointed to by pnChange is incremented by the number of
** entries in the table.
*/
int sqlite3BtreeClearTable(Btree *p, int iTable, int *pnChange){
	assert(p);
	assert(p->curr_txn);
	int changes = 0;
	KVS_cursor *cursor = NULL;
	KVS_range range[1];
	KVS_val key[1];
	int rc = kvs_cursor_open(p->curr_txn, &cursor);
	if(rc < 0) goto cleanup;
	BTREE_TABLE_RANGE(range, iTable);
	rc = kvs_cursor_firstr(cursor, range, key, NULL, +1);
	for(; rc >= 0; rc = kvs_cursor_nextr(cursor, range, key, NULL, +1)) {
		rc = kvs_del(p->curr_txn, key, 0);
		if(rc < 0) goto cleanup;
		changes++;
	}
	if(KVS_NOTFOUND == rc) rc = 0;
	if(rc < 0) goto cleanup;
	if(pnChange) *pnChange = changes;
cleanup:
	kvs_cursor_close(cursor); cursor = NULL;
	LOG("rc=%d",rc);
	return errmap(rc);
}

/*
** Close an open database and invalidate all cursors.
*/
int sqlite3BtreeClose(Btree *p){
	BtShared *pBt = p->pBt;
	BtCursor *pCur;
	sqlite3_mutex *mutexOpen;

	/* Close all cursors opened via this handle. */
	pCur = p->pCursor;
	while (pCur) {
		BtCursor *pTmp = pCur;
		pCur = pCur->pNext;
		sqlite3BtreeCloseCursor(pTmp);
	}

	/* Abort any active transaction */
	kvs_txn_abort(p->main_txn);

	if (p->isTemp) {
		unlink(pBt->filename);
		unlink(pBt->lockname);
		sqlite3_free(pBt->filename);
		sqlite3_free(pBt->lockname);
		kvs_env_close(pBt->env);
		sqlite3_free(pBt);
	} else {
		mutexOpen = sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_OPEN);
		sqlite3_mutex_enter(mutexOpen);
		if (--pBt->nRef == 0) {
			BtShared **prev;
			if (pBt->xFreeSchema && pBt->pSchema)
				pBt->xFreeSchema(pBt->pSchema);
			sqlite3DbFree(0, pBt->pSchema);
			kvs_env_close(pBt->env);
			prev = &sqlite3SharedCacheList;
			while (*prev != pBt) prev = &(*prev)->pNext;
			*prev = pBt->pNext;
			sqlite3_free(pBt->filename);
			sqlite3_free(pBt->lockname);
			sqlite3_free(pBt);
		} else {
			Btree **prev;
			prev = &pBt->trees;
			while (*prev != p) prev = &(*prev)->pNext;
			*prev = p->pNext;
		}
		sqlite3_mutex_leave(mutexOpen);
	}
	sqlite3_free(p);
	LOG("done",0);
	return SQLITE_OK;
}

/*
** Close a cursor.
*/
int sqlite3BtreeCloseCursor(BtCursor *pCur){
  Btree *pBtree = pCur->pBtree;
  if (pBtree) {
    BtCursor **prev = &pBtree->pCursor;
	while (*prev != pCur) prev = &((*prev)->pNext);
	*prev = pCur->pNext;
  }
  kvs_cursor_close(pCur->cursor); pCur->cursor = NULL;
//  sqlite3_free(pCur->index.mv_data);
  sqlite3BtreeClearCursor(pCur);
  LOG("done",0);
  return SQLITE_OK;
}

/*
** Do both phases of a commit.
*/
int sqlite3BtreeCommit(Btree *p){
  int rc;

  rc = sqlite3BtreeCommitPhaseOne(p, NULL);
  if (rc == 0)
    rc = sqlite3BtreeCommitPhaseTwo(p, 0);
  LOG("rc=%d",rc);
  return rc;
}

/*
** This routine does the first phase of a two-phase commit.  This routine
** causes a rollback journal to be created (if it does not already exist)
** and populated with enough information so that if a power loss occurs
** the database can be restored to its original state by playing back
** the journal.  Then the contents of the journal are flushed out to
** the disk.  After the journal is safely on oxide, the changes to the
** database are written into the database file and flushed to oxide.
** At the end of this call, the rollback journal still exists on the
** disk and we are still holding all locks, so the transaction has not
** committed.  See sqlite3BtreeCommitPhaseTwo() for the second phase of the
** commit process.
**
** This call is a no-op if no write-transaction is currently active on pBt.
**
** Otherwise, sync the database file for the btree pBt. zMaster points to
** the name of a master journal file that should be written into the
** individual journal file, or is NULL, indicating no master journal file 
** (single database transaction).
**
** When this is called, the master journal should already have been
** created, populated with this journal pointer and synced to disk.
**
** Once this is routine has returned, the only thing required to commit
** the write-transaction for this database file is to delete the journal.
*/
int sqlite3BtreeCommitPhaseOne(Btree *p, const char *zMaster){
  BtCursor *pc, *pn;
  int rc = 0;
  if (p->main_txn) {
    rc = kvs_txn_commit(p->main_txn);
    p->main_txn = NULL;
    p->curr_txn = NULL;
	p->inTrans = TRANS_NONE;
  }
  for (pn = p->pCursor, pc=pn; pc; pc=pn) {
    pn = pc->pNext;
    sqlite3BtreeCloseCursor(pc);
	sqlite3BtreeCursorZero(pc);
  }

  LOG("rc=%d",rc);
  return errmap(rc);
}

/*
** Commit the transaction currently in progress.
**
** This routine implements the second phase of a 2-phase commit.  The
** sqlite3BtreeCommitPhaseOne() routine does the first phase and should
** be invoked prior to calling this routine.  The sqlite3BtreeCommitPhaseOne()
** routine did all the work of writing information out to disk and flushing the
** contents so that they are written onto the disk platter.  All this
** routine has to do is delete or truncate or zero the header in the
** the rollback journal (which causes the transaction to commit) and
** drop locks.
**
** Normally, if an error occurs while the pager layer is attempting to 
** finalize the underlying journal file, this function returns an error and
** the upper layer will attempt a rollback. However, if the second argument
** is non-zero then this b-tree transaction is part of a multi-file 
** transaction. In this case, the transaction has already been committed 
** (by deleting a master journal file) and the caller will ignore this 
** functions return code. So, even if an error occurs in the pager layer,
** reset the b-tree objects internal state to indicate that the write
** transaction has been closed. This is quite safe, as the pager will have
** transitioned to the error state.
**
** This will release the write lock on the database file.  If there
** are no active cursors, it also releases the read lock.
*/
int sqlite3BtreeCommitPhaseTwo(Btree *p, int bCleanup){
  LOG("done",0);
  return SQLITE_OK;
}

#ifndef SQLITE_OMIT_BTREECOUNT
/*
** The first argument, pCur, is a cursor opened on some b-tree. Count the
** number of entries in the b-tree and write the result to *pnEntry.
**
** SQLITE_OK is returned if the operation is successfully executed. 
** Otherwise, if an error is encountered (i.e. an IO error or database
** corruption) an SQLite error code is returned.
*/
int sqlite3BtreeCount(BtCursor *pCur, i64 *pnEntry){
  KVS_cursor *const mc = pCur->cursor;
//  KVS_stat stats[1];
//  mdb_stat(mdb_cursor_txn(mc), mdb_cursor_dbi(mc), stats);
//  *pnEntry = stats->ms_entries;
  *pnEntry = 100; // TODO
  LOG("done",0);
  return SQLITE_OK;
}
#endif

/*
** Create a new BTree table.  Write into *piTable the page
** number for the root page of the new table.
**
** The type of table is determined by the flags parameter.  Only the
** following values of flags are currently in use.  Other values for
** flags might not work:
**
**     BTREE_INTKEY|BTREE_LEAFDATA     Used for SQL tables with rowid keys
**     BTREE_ZERODATA                  Used for SQL indices
*/
int sqlite3BtreeCreateTable(Btree *const p, int *const piTable, int const flags){
	KVS_val key[1], val[1];
	u32 last;
	int rc = SQLITE_OK;

	sqlite3BtreeGetMeta(p, BTREE_LARGEST_ROOT_PAGE, &last);
	if(0 == last) last++; // Table 1 is auto-generated.
	last++;

	KVS_VAL_STORAGE(key, KVS_VARINT_MAX*2);
	kvs_bind_uint64(key, TABLE_SCHEMA);
	kvs_bind_uint64(key, last);
	KVS_VAL_STORAGE(val, KVS_VARINT_MAX);
	kvs_bind_uint64(val, flags);
	rc = errmap(kvs_put(p->curr_txn, key, val, 0));
	if(rc < 0) goto cleanup;

	rc = sqlite3BtreeUpdateMeta(p, BTREE_LARGEST_ROOT_PAGE, last);
	if(SQLITE_OK != rc) goto cleanup;

	*piTable = last;
cleanup:
	LOG("rc=%d",rc);
	return errmap(rc);
}

/*
** Create a new cursor for the BTree whose root is on the page
** iTable. If a read-only cursor is requested, it is assumed that
** the caller already has at least a read-only transaction open
** on the database already. If a write-cursor is requested, then
** the caller is assumed to have an open write transaction.
**
** If wrFlag==0, then the cursor can only be used for reading.
** If wrFlag==1, then the cursor can be used for reading or for
** writing if other conditions for writing are also met.  These
** are the conditions that must be met in order for writing to
** be allowed:
**
** 1:  The cursor must have been opened with wrFlag==1
**
** 2:  The database must be writable (not on read-only media)
**
** 3:  There must be an active transaction.
**
** No checking is done to make sure that page iTable really is the
** root page of a b-tree.  If it is not, then the cursor acquired
** will not work correctly.
**
** It is assumed that the sqlite3BtreeCursorZero() has been called
** on pCur to initialize the memory space prior to invoking this routine.
*/
int sqlite3BtreeCursor(
  Btree *p,                                   /* The btree */
  int iTable,                                 /* Root page of table to open */
  int wrFlag,                                 /* 1 to write. 0 read-only */
  struct KeyInfo *pKeyInfo,                   /* First arg to xCompare() */
  BtCursor *pCur                              /* Write new cursor here */
){
	KVS_cursor *mc = NULL;
	int rc = kvs_cursor_open(p->curr_txn, &mc);
	if(rc < 0) goto cleanup;
	pCur->cursor = mc;
	pCur->pNext = p->pCursor;
	p->pCursor = pCur;
	pCur->pBtree = p;
	pCur->pKeyInfo = pKeyInfo;
	pCur->iTable = iTable;
cleanup:
	LOG("rc=%d, iTable=%d",rc, iTable);
	return errmap(rc);
}

/*
** Determine whether or not a cursor has moved from the position it
** was last placed at.  Cursors can move when the row they are pointing
** at is deleted out from under them.
**
** This routine returns an error code if something goes wrong.  The
** integer *pHasMoved is set to one if the cursor has moved and 0 if not.
*/
int sqlite3BtreeCursorHasMoved(BtCursor *pCur, int *pHasMoved){
	KVS_cursor *const mc = pCur->cursor;
	int rc = kvs_cursor_current(mc, NULL, NULL);
	if(KVS_NOTFOUND == rc) {
		*pHasMoved = 1;
		rc = 0;
	} else if(rc >= 0) {
		*pHasMoved = 0;
	}
	LOG("rc=%d, *pHasMoved=%d", rc, *pHasMoved);
	return errmap(rc);
}

/*
** Return the size of a BtCursor object in bytes.
**
** This interfaces is needed so that users of cursors can preallocate
** sufficient storage to hold a cursor.  The BtCursor object is opaque
** to users so they cannot do the sizeof() themselves - they must call
** this routine.
*/
int sqlite3BtreeCursorSize(void){
  LOG("done",0);
  return ROUND8(sizeof(BtCursor));
}

/*
** Initialize memory that will be converted into a BtCursor object.
**
** The simple approach here would be to memset() the entire object
** to zero.  But it turns out that the apPage[] and aiIdx[] arrays
** do not need to be zeroed and they are large, so we can save a lot
** of run-time by skipping the initialization of those elements.
*/
void sqlite3BtreeCursorZero(BtCursor *p){
  p->pKeyInfo = NULL;
  p->pBtree = NULL;
  p->cachedRowid = 0;
  p->cursor = NULL;
  p->iTable = 0;
  LOG("done",0);
}

/*
** Read part of the data associated with cursor pCur.  Exactly
** "amt" bytes will be transfered into pBuf[].  The transfer
** begins at "offset".
**
** Return SQLITE_OK on success or an error code if anything goes
** wrong.  An error is returned if "offset+amt" is larger than
** the available payload.
*/
int sqlite3BtreeData(BtCursor *pCur, u32 offset, u32 amt, void *pBuf){
	KVS_cursor *const mc = pCur->cursor;
	KVS_val val[1];
	int rc = kvs_cursor_current(mc, NULL, val);
	if(rc < 0) goto cleanup;
	if(offset+amt > val->size) rc = KVS_CORRUPTED;
	if(rc < 0) goto cleanup;
	memcpy(pBuf, (char *)val->data+offset, amt);
cleanup:
	LOG("done", 0);
	return errmap(rc);
}

/*
** For the entry that cursor pCur is point to, return as
** many bytes of the key or data as are available on the local
** b-tree page.  Write the number of available bytes into *pAmt.
**
** These routines are used to get quick access to key and data
** in the common case where no overflow pages are used.
*/
const void *sqlite3BtreeKeyFetch(BtCursor *pCur, int *pAmt){
	KVS_cursor *const mc = pCur->cursor;
	int flags;
	KVS_val key[1], val[1];
	int amt = 0;
	void const *data = NULL;
	int rc = BtreeTableSchema(pCur->pBtree, pCur->iTable, &flags);
	if(rc < 0) goto cleanup;
	if(BTREE_INTKEY & flags) goto cleanup; // Meaningless.

	rc = kvs_cursor_current(mc, key, val);
	if(rc < 0) goto cleanup;
	uint64_t nKey = kvs_read_uint64(val);
	if(nKey > val->size) rc = KVS_CORRUPTED;
	if(rc < 0) goto cleanup;
	amt = nKey;
	data = val->data;
cleanup:
	*pAmt = amt;
	LOG("rc=%d, *pAmt=%d, buf=%p", rc, *pAmt, data);
	return data;
}
const void *sqlite3BtreeDataFetch(BtCursor *pCur, int *pAmt){
	KVS_cursor *const mc = pCur->cursor;
	int flags;
	KVS_val val[1];
	int amt = 0;
	void const *data = NULL;
	int rc = BtreeTableSchema(pCur->pBtree, pCur->iTable, &flags);
	if(rc < 0) goto cleanup;
	if(BTREE_ZERODATA & flags) goto cleanup;

	rc = kvs_cursor_current(mc, NULL, val);
	if(rc < 0) goto cleanup;
	if(BTREE_INTKEY & flags) {
		amt = val->size;
		data = val->data;
	} else {
		uint64_t nKey = kvs_read_uint64(val);
		if(nKey > val->size) rc = KVS_CORRUPTED;
		if(rc < 0) goto cleanup;
		amt = val->size - nKey;
		data = (char *)val->data + nKey;
	}
cleanup:
	*pAmt = amt;
	LOG("rc=%d, *pAmt=%d, buf=%p", rc, *pAmt, data);
	return data;
}

/*
** Set *pSize to the number of bytes of data in the entry the
** cursor currently points to.
**
** The caller must guarantee that the cursor is pointing to a non-NULL
** valid entry.  In other words, the calling procedure must guarantee
** that the cursor has Cursor.eState==CURSOR_VALID.
**
** Failure is not possible.  This function always returns SQLITE_OK.
** It might just as well be a procedure (returning void) but we continue
** to return an integer result code for historical reasons.
*/
int sqlite3BtreeDataSize(BtCursor *pCur, u32 *pSize){
	int amt = 0;
	void const *x = sqlite3BtreeDataFetch(pCur, &amt);
	*pSize = amt;
	LOG("res=%p, *pSize=%lu", x, (unsigned long)*pSize);
	return SQLITE_OK;
}

/*
** Delete the entry that the cursor is pointing to.  The cursor
** is left pointing at a arbitrary location.
*/
int sqlite3BtreeDelete(BtCursor *pCur){
	KVS_cursor *const mc = pCur->cursor;
	int rc = kvs_cursor_del(mc, 0);
	LOG("rc=%d",rc);
	return errmap(rc);
}

/*
** Erase all information in a table and add the root of the table to
** the freelist.  Except, the root of the principle table (the one on
** page 1) is never added to the freelist.
**
** This routine will fail with SQLITE_LOCKED if there are any open
** cursors on the table.
*/
int sqlite3BtreeDropTable(Btree *p, int iTable, int *piMoved){
  int rc = sqlite3BtreeClearTable(p, iTable, NULL);
  if(rc < 0) goto cleanup;
  KVS_val key[1];
  KVS_VAL_STORAGE(key, KVS_VARINT_MAX*2);
  kvs_bind_uint64(key, TABLE_SCHEMA);
  kvs_bind_uint64(key, TABLE_START + iTable);
  rc = kvs_del(p->curr_txn, key, 0);
  if(rc < 0) goto cleanup;
cleanup:
  *piMoved = 0;
  LOG("rc=%d",rc);
  return errmap(rc);
}

/*
** Return TRUE if the cursor is not pointing at an entry of the table.
**
** TRUE will be returned after a call to sqlite3BtreeNext() moves
** past the last entry in the table or sqlite3BtreePrev() moves past
** the first entry.  TRUE is also returned if the table is empty.
*/
int sqlite3BtreeEof(BtCursor *pCur){
	KVS_cursor *const mc = pCur->cursor;
	int rc = kvs_cursor_get(mc, NULL, NULL, KVS_GET_CURRENT);
	return rc < 0;
}

/* Move the cursor to the first entry in the table.  Return SQLITE_OK
** on success.  Set *pRes to 0 if the cursor actually points to something
** or set *pRes to 1 if the table is empty.
*/
int sqlite3BtreeFirst(BtCursor *pCur, int *pRes){
	KVS_cursor *const mc = pCur->cursor;
	KVS_range range[1];
	BTREE_TABLE_RANGE(range, pCur->iTable);
	int rc = kvs_cursor_firstr(mc, range, NULL, NULL, +1);
	if(rc >= 0) {
		*pRes = 0;
	} else if(KVS_NOTFOUND == rc) {
		*pRes = 1;
		rc = 0;
	}
	LOG("rc=%d, *pRes=%d", rc, *pRes);
	return errmap(rc);
}

/*
** Return the value of the 'auto-vacuum' property. If auto-vacuum is 
** enabled 1 is returned. Otherwise 0.
*/
int sqlite3BtreeGetAutoVacuum(Btree *p){
  LOG("done",0);
  return 0;
}

/*
** Return the cached rowid for the given cursor.  A negative or zero
** return value indicates that the rowid cache is invalid and should be
** ignored.  If the rowid cache has never before been set, then a
** zero is returned.
*/
sqlite3_int64 sqlite3BtreeGetCachedRowid(BtCursor *pCur){
  LOG("done",0);
  return pCur->cachedRowid;
}

/*
** Return the full pathname of the underlying database file.
**
** The pager filename is invariant as long as the pager is
** open so it is safe to access without the BtShared mutex.
*/
const char *sqlite3BtreeGetFilename(Btree *p){
  LOG("done",0);
  return p->pBt->filename;
}

/*
** Return the pathname of the journal file for this database. The return
** value of this routine is the same regardless of whether the journal file
** has been created or not.
**
** The pager journal filename is invariant as long as the pager is
** open so it is safe to access without the BtShared mutex.
*/
const char *sqlite3BtreeGetJournalname(Btree *p){
  LOG("done",0);
  return p->pBt->lockname;
}

/*
** This function may only be called if the b-tree connection already
** has a read or write transaction open on the database.
**
** Read the meta-information out of a database file.  Meta[0]
** is the number of free pages currently in the database.  Meta[1]
** through meta[15] are available for use by higher layers.  Meta[0]
** is read-only, the others are read/write.
** 
** The schema layer numbers meta values differently.  At the schema
** layer (and the SetCookie and ReadCookie opcodes) the number of
** free pages is not visible.  So Cookie[0] is the same as Meta[1].
*/
void sqlite3BtreeGetMeta(Btree *p, int idx, u32 *pMeta){
	assert(idx >= 0);
	assert(idx < NUMMETA);
	uint64_t meta = 0;
	KVS_val key[1], val[1];
	int rc = 0;
	if(0 == idx) goto cleanup; // Number of free pages
	KVS_VAL_STORAGE(key, KVS_VARINT_MAX*2);
	kvs_bind_uint64(key, TABLE_META);
	kvs_bind_uint64(key, idx);
	rc = kvs_get(p->curr_txn, key, val);
	if(rc < 0) goto cleanup;
	meta = kvs_read_uint64(val);
cleanup:
	*pMeta = meta;
	LOG("idx=%d, *pMeta=%u",idx,*pMeta);
}

/*
** Return the currently defined page size
*/
int sqlite3BtreeGetPageSize(Btree *p){
  LOG("done",0);
  return 4096;
}

#if !defined(SQLITE_OMIT_PAGER_PRAGMAS) || !defined(SQLITE_OMIT_VACUUM)
/*
** Return the number of bytes of space at the end of every page that
** are intentually left unused.  This is the "reserved" space that is
** sometimes used by extensions.
*/
int sqlite3BtreeGetReserve(Btree *p){
  LOG("done",0);
  return 0;
}

/*
** Set the maximum page count for a database if mxPage is positive.
** No changes are made if mxPage is 0 or negative.
** Regardless of the value of mxPage, return the maximum page count.
*/
int sqlite3BtreeMaxPageCount(Btree *p, int mxPage){
  int n;
  LOG("done",0);
  if (mxPage > 0) {
    size_t x = mxPage * 4096;
    kvs_env_set_config(p->pBt->env, KVS_CFG_MAPSIZE, &x);
    p->pBt->mxPage = mxPage;
  }
  return p->pBt->mxPage; // TODO: Initialize on creation
}

/*
** Set the secureDelete flag if newFlag is 0 or 1.  If newFlag is -1,
** then make no changes.  Always return the value of the secureDelete
** setting after the change.
*/
int sqlite3BtreeSecureDelete(Btree *p, int newFlag){
  LOG("done",0);
  return 0;
}
#endif /* !defined(SQLITE_OMIT_PAGER_PRAGMAS) || !defined(SQLITE_OMIT_VACUUM) */

/*
** Change the 'auto-vacuum' property of the database. If the 'autoVacuum'
** parameter is non-zero, then auto-vacuum mode is enabled. If zero, it
** is disabled. The default value for the auto-vacuum property is 
** determined by the SQLITE_DEFAULT_AUTOVACUUM macro.
*/
int sqlite3BtreeSetAutoVacuum(Btree *p, int autoVacuum){
  LOG("done",0);
  return SQLITE_READONLY;
}

#ifndef SQLITE_OMIT_AUTOVACUUM
/*
** A write-transaction must be opened before calling this function.
** It performs a single unit of work towards an incremental vacuum.
**
** If the incremental vacuum is finished after this function has run,
** SQLITE_DONE is returned. If it is not finished, but no error occurred,
** SQLITE_OK is returned. Otherwise an SQLite error code. 
*/
int sqlite3BtreeIncrVacuum(Btree *p){
  LOG("done",0);
  return SQLITE_DONE;
}
#endif

#if 0
/* Store the rowid in the index as data
 * instead of as part of the key, so rows
 * that have the same indexed value have only one
 * key in the index.
 * The original index key looks like:
 * hdrSize_column1Size_columnNSize_rowIdSize_column1Data_columnNData_rowid
 * The new index key looks like:
 * hdrSize_column1Size_columnNSize_column1Data_columnNData
 * With a data section that looks like:
 * rowIdSize_rowid
 */
static void splitIndexKey(MDB_val *key, MDB_val *data)
{
	u32 hdrSize, rowidType;
	unsigned char *aKey = (unsigned char *)key->mv_data;
	getVarint32(aKey, hdrSize);
	getVarint32(&aKey[hdrSize-1], rowidType);
	data->mv_size = sqlite3VdbeSerialTypeLen(rowidType) + 1;
	key->mv_size -= data->mv_size;
	memmove(&aKey[hdrSize-1], &aKey[hdrSize], key->mv_size-(hdrSize-1));
	putVarint32(&aKey[key->mv_size], rowidType);
	putVarint32(aKey, hdrSize-1);
	data->mv_data = &aKey[key->mv_size];
}

static int joinIndexKey(MDB_val *key, MDB_val *data, BtCursor *pCur, u_int32_t amount)
{
	u32 hdrSize;
	unsigned char *aKey = (unsigned char *)key->mv_data;
	unsigned char *aData = (unsigned char *)data->mv_data;
	unsigned char *newKey;

	if (pCur->index.mv_size < amount) {
	  sqlite3_free(pCur->index.mv_data);
	  pCur->index.mv_data = sqlite3_malloc(amount*2);
	  if (!pCur->index.mv_data)
	    return SQLITE_NOMEM;
	  pCur->index.mv_size = amount*2;
	}
	newKey = (unsigned char *)pCur->index.mv_data;
	getVarint32(aKey, hdrSize);
	memcpy(newKey, aKey, hdrSize);
	memcpy(&newKey[hdrSize+1], &aKey[hdrSize], key->mv_size - hdrSize);
	memcpy(&newKey[key->mv_size+1], &aData[1], data->mv_size - 1);
	newKey[hdrSize] = aData[0];
	putVarint32(newKey, hdrSize+1);
	return SQLITE_OK;
}

static void squashIndexKey(UnpackedRecord *pun, int file_format, MDB_val *key)
{
	int i, changed = 0;
	u32 serial_type;
	Mem *pMem;
	MDB_val v;
	mdb_hash_t h;

	/* Look for any large strings or blobs */
	pMem = pun->aMem;
	for (i=0; i<pun->nField; i++) {
		serial_type = sqlite3VdbeSerialType(pMem, file_format);
		if (serial_type >= 12 && pMem->n >72) {
			v.mv_data = (char *)pMem->z + 64;
			v.mv_size = pMem->n - 64;
			h = mdb_hash_val(&v, MDB_HASH_INIT);
			pMem->n = 72;
			memcpy(v.mv_data, &h, sizeof(h));
			changed = 1;
		}
		pMem++;
	}

	/* If we changed anything and the key was provided, rewrite the key */
	if (changed && key) {
		u8 *zNewRecord;
		int nHdr = 0;
		int nData = 0;
		int nByte;
		int nVarint;
		int len;

		/* Loop thru and find out how much space is needed */
		pMem = pun->aMem;
		for (i=0; i<pun->nField; i++) {
			serial_type = sqlite3VdbeSerialType(pMem, file_format);
			len = sqlite3VdbeSerialTypeLen(serial_type);
			nData += len;
			nHdr += sqlite3VarintLen(serial_type);
			pMem++;
		}
		nHdr += nVarint = sqlite3VarintLen(nHdr);
		if (nVarint < sqlite3VarintLen(nHdr))
			nHdr++;
		nByte = nHdr+nData;
		zNewRecord = key->mv_data;
		len = putVarint32(zNewRecord, nHdr);
		pMem = pun->aMem;
		for (i=0; i<pun->nField; i++) {
			serial_type = sqlite3VdbeSerialType(pMem, file_format);
			len += putVarint32(&zNewRecord[len], serial_type);
			pMem++;
		}
		pMem = pun->aMem;
		for (i=0; i<pun->nField; i++) {
			len += sqlite3VdbeSerialPut(&zNewRecord[len], (int)(nByte-len), pMem, file_format);
			pMem++;
		}
		key->mv_size = len;
	}
}
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))
static int BtreeRawKey(KVS_txn *const txn, KVS_val *const key, UnpackedRecord const *const p) {
	assert(key);
	assert(key->data);
	assert(p);
	u16 i;
	for(i = 0; i < p->nField; i++) {
		Mem const *const mem = &p->aMem[i];
		// TODO: Most of these are broken
		// Get better serialization code from SQLite4?
		if(mem->flags & MEM_Null) {
			kvs_bind_uint64(key, 0);
		} else if(mem->flags & MEM_Real) {
			kvs_bind_uint64(key, 1);
			kvs_bind_double(key, mem->r);
		} else if(mem->flags & MEM_Int) {
			kvs_bind_uint64(key, 1);
			kvs_bind_double(key, (double)mem->u.i);
		} else if(mem->flags & MEM_Str) {
			kvs_bind_uint64(key, 2);
			kvs_bind_string_len(key, mem->z, mem->n, 0, txn);
		} else if(mem->flags & MEM_Blob) {
			kvs_bind_uint64(key, 3);
			kvs_bind_blob(key, mem->z, MIN(mem->n, 64));
		} else {
			sqlite3DebugPrintf("Unrecognized column type %d\n", mem->flags);
			assert(0);
		}
		assert(key->size < MDB_MAXKEYSIZE);
	}
	return 0;
}

/*
** Insert a new record into the BTree.  The key is given by (pKey,nKey)
** and the data is given by (pData,nData).  The cursor is used only to
** define what table the record should be inserted into.  The cursor
** is left pointing at a random location.
**
** For an INTKEY table, only the nKey value of the key is used.  pKey is
** ignored.  For a ZERODATA table, the pData and nData are both ignored.
**
** If the seekResult parameter is non-zero, then a successful call to
** MovetoUnpacked() to seek cursor pCur to (pKey, nKey) has already
** been performed. seekResult is the search result returned (a negative
** number if pCur points at an entry that is smaller than (pKey, nKey), or
** a positive value if pCur points at an entry that is larger than
** (pKey, nKey)). 
**
** If the seekResult parameter is non-zero, then the caller guarantees that
** cursor pCur is pointing at the existing copy of a row that is to be
** overwritten.  If the seekResult parameter is 0, then cursor pCur may
** point to any entry or to no entry at all and so this function has to seek
** the cursor before the new key can be inserted.
*/
int sqlite3BtreeInsert(
  BtCursor *pCur,                /* Insert data into the table of this cursor */
  const void *pKey, i64 nKey,    /* The key of the new record */
  const void *pData, int nData,  /* The data of the new record */
  int nZero,                     /* Number of extra 0 bytes to append to data */
  int appendBias,                /* True if this is likely an append */
  int seekResult                 /* Result of prior MovetoUnpacked() call */
){
	assert(pCur);
	assert(nZero >= 0);
	KVS_cursor *const mc = pCur->cursor;
	int flags;
	KVS_val key[1], val[1];
	char *unpacked = NULL;
	char *payload = NULL;
	int rc = 0;

	rc = BtreeTableSchema(pCur->pBtree, pCur->iTable, &flags);
	if(SQLITE_OK != rc) return rc;
	if(BTREE_ZERODATA & flags) {
		pData = NULL;
		nData = 0;
	}

	KVS_VAL_STORAGE(key, MDB_MAXKEYSIZE); // TODO: Not guaranteed to be enough
	kvs_bind_uint64(key, TABLE_START + pCur->iTable);
	if(BTREE_INTKEY & flags) {
		kvs_bind_sint64(key, nKey);
		val->size = nData;
		val->data = (void *)pData;
	} else {
		UnpackedRecord *p = sqlite3VdbeAllocUnpackedRecord(pCur->pKeyInfo, NULL, 0, &unpacked);
		if(!p) rc = KVS_ENOMEM;
		if(rc < 0) goto cleanup;
		sqlite3VdbeRecordUnpack(pCur->pKeyInfo, nKey, pKey, p);
		rc = BtreeRawKey(pCur->pBtree->curr_txn, key, p);
		if(rc < 0) goto cleanup;

		payload = sqlite3_malloc(KVS_VARINT_MAX+nKey+nData);
		if(!payload) rc = KVS_ENOMEM;
		if(rc < 0) goto cleanup;
		val->size = 0;
		val->data = payload;
		kvs_bind_uint64(val, nKey);
		kvs_bind_blob(val, pKey, nKey);
		kvs_bind_blob(val, pData, nData);
	}
	assert(key->size+nZero <= MDB_MAXKEYSIZE);
	memset((char *)key->data+key->size, 0, nZero);
	key->size += nZero;

	rc = kvs_cursor_put(mc, key, val, 0);

cleanup:
	LOG("rc=%d",rc);
	sqlite3_free(unpacked); unpacked = NULL;
	sqlite3_free(payload); payload = NULL;
	return errmap(rc);
}

#ifndef SQLITE_OMIT_INTEGRITY_CHECK
/*
** This routine does a complete check of the given BTree file.  aRoot[] is
** an array of pages numbers were each page number is the root page of
** a table.  nRoot is the number of entries in aRoot.
**
** A read-only or read-write transaction must be opened before calling
** this function.
**
** Write the number of error seen in *pnErr.  Except for some memory
** allocation errors,  an error message held in memory obtained from
** malloc is returned if *pnErr is non-zero.  If *pnErr==0 then NULL is
** returned.  If a memory allocation error occurs, NULL is returned.
*/
char *sqlite3BtreeIntegrityCheck(
  Btree *p,     /* The btree to be checked */
  int *aRoot,   /* An array of root pages numbers for individual trees */
  int nRoot,    /* Number of entries in aRoot[] */
  int mxErr,    /* Stop reporting errors after this many */
  int *pnErr    /* Write number of errors seen to this variable */
){
  LOG("done",0);
  *pnErr = 0;
  return NULL;
}
#endif

/*
** Return non-zero if a transaction is active.
*/
int sqlite3BtreeIsInTrans(Btree *p){
  int rc = (p && (p->inTrans==TRANS_WRITE));
  LOG("rc=%d",rc);
  return rc;
}

/*
** Return non-zero if a read (or write) transaction is active.
*/
int sqlite3BtreeIsInReadTrans(Btree *p){
  int rc = (p && p->inTrans!=TRANS_NONE);
  LOG("rc=%d",rc);
  return rc;
}

int sqlite3BtreeIsInBackup(Btree *p){
  LOG("rc=0",0);
  return 0;
}

/*
** Read part of the key associated with cursor pCur.  Exactly
** "amt" bytes will be transfered into pBuf[].  The transfer
** begins at "offset".
**
** The caller must ensure that pCur is pointing to a valid row
** in the table.
**
** Return SQLITE_OK on success or an error code if anything goes
** wrong.  An error is returned if "offset+amt" is larger than
** the available payload.
*/
int sqlite3BtreeKey(BtCursor *pCur, u32 offset, u32 amt, void *pBuf){
	KVS_cursor *const mc = pCur->cursor;
	KVS_val val[1];
	int rc = kvs_cursor_current(mc, NULL, val);
	if(rc < 0) goto cleanup;
	uint64_t const nKey = kvs_read_uint64(val);
	if(nKey > val->size) rc = KVS_CORRUPTED;
	if(rc < 0) goto cleanup;
	if(offset+amt > nKey) rc = KVS_CORRUPTED;
	if(rc < 0) goto cleanup;
	memcpy(pBuf, val->data+offset, amt);
cleanup:
	LOG("done", 0);
	return errmap(rc);
}

/*
** Set *pSize to the size of the buffer needed to hold the value of
** the key for the current entry.  If the cursor is not pointing
** to a valid entry, *pSize is set to 0. 
**
** For a table with the INTKEY flag set, this routine returns the key
** itself, not the number of bytes in the key.
**
** The caller must position the cursor prior to invoking this routine.
** 
** This routine cannot fail.  It always returns SQLITE_OK.  
*/
int sqlite3BtreeKeySize(BtCursor *pCur, i64 *pSize){
	KVS_cursor *const mc = pCur->cursor;
	i64 size = 0;
	int flags;
	KVS_val key[1], val[1];
	int rc = BtreeTableSchema(pCur->pBtree, pCur->iTable, &flags);
	if(rc < 0) goto cleanup;
	rc = kvs_cursor_current(mc, key, val);
	if(rc < 0) goto cleanup;
	if(BTREE_INTKEY & flags) {
		uint64_t const iTable = kvs_read_uint64(key);
		kvs_assert(iTable-TABLE_START == pCur->iTable);
		size = kvs_read_sint64(key);
	} else {
		size = kvs_read_uint64(val);
	}
cleanup:
	*pSize = size;
	LOG("rc=%d, *pSize=%lld", rc, (long long)*pSize);
	return SQLITE_OK;
}

/* Move the cursor to the last entry in the table.  Return SQLITE_OK
** on success.  Set *pRes to 0 if the cursor actually points to something
** or set *pRes to 1 if the table is empty.
*/
int sqlite3BtreeLast(BtCursor *pCur, int *pRes){
	KVS_cursor *const mc = pCur->cursor;
	KVS_range range[1];
	BTREE_TABLE_RANGE(range, pCur->iTable);
	int rc = kvs_cursor_firstr(mc, range, NULL, NULL, -1);
	if(rc >= 0) {
		*pRes = 0;
	} else if(KVS_NOTFOUND == rc) {
		*pRes = 1;
		rc = 0;
	}
	LOG("rc=%d, *pRes=%d", rc, *pRes);
	return errmap(rc);
}

/*
** Return the size of the database file in pages. If there is any kind of
** error, return ((unsigned int)-1).
*/
u32 sqlite3BtreeLastPage(Btree *p){
	LOG("done",0);
	// TODO
	return 0;
}

#ifndef SQLITE_OMIT_SHARED_CACHE
/*
** Obtain a lock on the table whose root page is iTab.  The
** lock is a write lock if isWritelock is true or a read lock
** if it is false.
*/
int sqlite3BtreeLockTable(Btree *p, int iTab, u8 isWriteLock){
  LOG("rc=0",0);
  return SQLITE_OK;
}
#endif

/* Move the cursor so that it points to an entry near the key 
** specified by pIdxKey or intKey.   Return a success code.
**
** For INTKEY tables, the intKey parameter is used.  pUnKey
** must be NULL.  For index tables, pUnKey is used and intKey
** is ignored.
**
** If an exact match is not found, then the cursor is always
** left pointing at a leaf page which would hold the entry if it
** were present.  The cursor might point to an entry that comes
** before or after the key.
**
** An integer is written into *pRes which is the result of
** comparing the key with the entry to which the cursor is 
** pointing.  The meaning of the integer written into
** *pRes is as follows:
**
**     *pRes==0     The cursor is left pointing at an entry that
**                  exactly matches intKey/pUnKey.
**
**     *pRes>0      The cursor is left pointing at an entry that
**                  is larger than intKey/pUnKey.
**
*/
int sqlite3BtreeMovetoUnpacked(
  BtCursor *pCur,          /* The cursor to be moved */
  UnpackedRecord *pUnKey,  /* Unpacked index key */
  i64 intKey,              /* The table key */
  int biasRight,           /* If true, bias the search to the high end */
  int *pRes                /* Write search results here */
){
	KVS_cursor *const mc = pCur->cursor;
	int flags;
	KVS_range range[1];
	KVS_val key[1], res[1];
	int rc = 0;
	rc = BtreeTableSchema(pCur->pBtree, pCur->iTable, &flags);
	if(rc < 0) goto cleanup;

	BTREE_TABLE_RANGE(range, pCur->iTable);

	KVS_VAL_STORAGE(key, MDB_MAXKEYSIZE);
	kvs_bind_uint64(key, TABLE_START + pCur->iTable);
	if(BTREE_INTKEY & flags) {
		kvs_bind_sint64(key, intKey);
	} else {
		rc = BtreeRawKey(pCur->pBtree->curr_txn, key, pUnKey);
		if(rc < 0) goto cleanup;
	}

	*res = *key;
	rc = kvs_cursor_seekr(mc, range, res, NULL, +1);
	if(KVS_NOTFOUND == rc) {
		*pRes = 1;
		rc = 0;
	} else if(rc >= 0) {
		*pRes = kvs_cursor_cmp(mc, res, key);
	}
cleanup:
	LOG("rc=%d, *pRes=%d", rc, *pRes);
	return errmap(rc);
}

/*
** Advance the cursor to the next entry in the database.  If
** successful then set *pRes=0.  If the cursor
** was already pointing to the last entry in the database before
** this routine was called, then set *pRes=1.
*/
int sqlite3BtreeNext(BtCursor *pCur, int *pRes){
	KVS_cursor *const mc = pCur->cursor;
	KVS_range range[1];
	BTREE_TABLE_RANGE(range, pCur->iTable);
	int rc = kvs_cursor_nextr(mc, range, NULL, NULL, +1);
	if(rc >= 0) {
		*pRes = 0;
	} else if(KVS_NOTFOUND == rc) {
		*pRes = 1;
		rc = 0;
	}
	LOG("rc=%d, *pRes=%d", rc, *pRes);
	return errmap(rc);
}

/*
** Open a database file.
** 
** zFilename is the name of the database file.  If zFilename is NULL
** then an ephemeral database is created.  The ephemeral database might
** be exclusively in memory, or it might use a disk-based memory cache.
** Either way, the ephemeral database will be automatically deleted 
** when sqlite3BtreeClose() is called.
**
** If zFilename is ":memory:" then an in-memory database is created
** that is automatically destroyed when it is closed.
**
** The "flags" parameter is a bitmask that might contain bits
** BTREE_OMIT_JOURNAL and/or BTREE_NO_READLOCK.  The BTREE_NO_READLOCK
** bit is also set if the SQLITE_NoReadlock flags is set in db->flags.
** These flags are passed through into sqlite3PagerOpen() and must
** be the same values as PAGER_OMIT_JOURNAL and PAGER_NO_READLOCK.
**
** If the database is already opened in the same database connection
** and we are in shared cache mode, then the open will fail with an
** SQLITE_CONSTRAINT error.  We cannot allow two or more BtShared
** objects in the same database connection since doing so will lead
** to problems with locking.
*/
int sqlite3BtreeOpen(
  sqlite3_vfs *pVfs,      /* VFS to use for this b-tree */
  const char *zFilename,  /* Name of the file containing the BTree database */
  sqlite3 *db,            /* Associated database handle */
  Btree **ppBtree,        /* Pointer to new Btree object written here */
  int flags,              /* Options */
  int vfsFlags            /* Flags passed through to sqlite3_vfs.xOpen() */
){
	Btree *p;
	BtShared *pBt;
	sqlite3_mutex *mutexOpen = NULL;
	int eflags = 0, rc = SQLITE_OK;
	char dirPathBuf[BT_MAX_PATH], *dirPathName = dirPathBuf;

	if ((p = (Btree *)sqlite3_malloc(sizeof(Btree))) == NULL) {
		rc = SQLITE_NOMEM;
		goto done;
	}
	p->db = db;
	p->pCursor = NULL;
	p->main_txn = NULL;
	p->curr_txn = NULL;
	p->inTrans = TRANS_NONE;
	p->isTemp = 0;
	p->locked = 0;
	p->wantToLock = 0;
	/* Transient and in-memory are all the same, use /tmp */
	if ((vfsFlags & SQLITE_OPEN_TRANSIENT_DB) || !zFilename || !zFilename[0] ||
			!strcmp(zFilename, ":memory:")) {
		char *envpath;
		p->isTemp = 1;
		envpath = tempnam(NULL, "kvs.");
		strcpy(dirPathBuf, envpath);
		free(envpath);
	} else {
		sqlite3OsFullPathname(pVfs, zFilename, sizeof(dirPathBuf), dirPathName);
		mutexOpen = sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_OPEN);
		sqlite3_mutex_enter(mutexOpen);
		struct stat stats[1];
		if(stat(dirPathName, stats) >= 0) {
			for (pBt = sqlite3SharedCacheList; pBt; pBt = pBt->pNext) {
				if (pBt->env && !strcmp(pBt->filename, dirPathName)) {
					p->pBt = pBt;
					pBt->nRef++;
					break;
				}
			}
			if (pBt) {
				p->pNext = pBt->trees;
				pBt->trees = p;
				pBt->nRef++;
				sqlite3_mutex_leave(mutexOpen);
				*ppBtree = p;
				goto done;
			}
		}
	}
	pBt = sqlite3_malloc(sizeof(BtShared));
	if (!pBt) {
		if (!p->isTemp) {
			sqlite3_mutex_leave(mutexOpen);
		}
		rc = SQLITE_NOMEM;
		goto done;
	}
//	rc = kvs_env_create_base(p->isTemp ? "mdb" : "leveldb", &pBt->env);
	rc = kvs_env_create_base("mdb", &pBt->env);
	if (rc) {
		if (!p->isTemp) {
			sqlite3_mutex_leave(mutexOpen);
		}
		rc = errmap(rc);
		goto done;
	}
	size_t mapsize = 256*1048576;
	kvs_env_set_config(pBt->env, KVS_CFG_MAPSIZE, &mapsize);
	if (vfsFlags & SQLITE_OPEN_READONLY)
		eflags |= KVS_RDONLY;
	if (vfsFlags & (SQLITE_OPEN_DELETEONCLOSE|SQLITE_OPEN_TEMP_DB|
		SQLITE_OPEN_TRANSIENT_DB))
		eflags |= KVS_NOSYNC;
	rc = kvs_env_open(pBt->env, dirPathName, eflags, SQLITE_DEFAULT_FILE_PERMISSIONS);
	if (rc) {
		if (!p->isTemp)
			sqlite3_mutex_leave(mutexOpen);
		rc = errmap(rc);
		goto done;
	}
	{
		int len = strlen(dirPathName);
		pBt->filename = sqlite3_malloc(len+1);
		pBt->lockname = sqlite3_malloc(len + sizeof(LOCKSUFF));
		if (!pBt->filename || !pBt->lockname) {
			if (!p->isTemp)
				sqlite3_mutex_leave(mutexOpen);
			rc = SQLITE_NOMEM;
			goto done;
		}
		sprintf(pBt->filename, "%s", dirPathName);
		sprintf(pBt->lockname, "%s" LOCKSUFF, dirPathName);
	}
	pBt->db = db;
	pBt->openFlags = flags;
	pBt->inTransaction = TRANS_NONE;
	pBt->nTransaction = 0;
	pBt->pSchema = NULL;
	pBt->xFreeSchema = NULL;
	pBt->nRef = 1;
	pBt->pWriter = NULL;
	if (p->isTemp) {
	} else {
		pBt->pNext = sqlite3SharedCacheList;
		sqlite3SharedCacheList = pBt;
		sqlite3_mutex_leave(mutexOpen);
	}
	p->pNext = NULL;
	pBt->trees = p;
	p->pBt = pBt;
	*ppBtree = p;

done:
	LOG("rc=%d",rc);
	return rc;;
}

/*
** Return the pager associated with a BTree.  This routine is used for
** testing and debugging only.
*/
Pager *sqlite3BtreePager(Btree *p){
  LOG("done",0);
  return (Pager *)p;
}

/*
** Step the cursor back to the previous entry in the database.  If
** successful then set *pRes=0.  If the cursor
** was already pointing to the first entry in the database before
** this routine was called, then set *pRes=1.
*/
int sqlite3BtreePrevious(BtCursor *pCur, int *pRes){
	KVS_cursor *const mc = pCur->cursor;
	KVS_range range[1];
	BTREE_TABLE_RANGE(range, pCur->iTable);
	int rc = kvs_cursor_nextr(mc, range, NULL, NULL, -1);
	if(rc >= 0) {
		*pRes = 0;
	} else if(KVS_NOTFOUND == rc) {
		*pRes = 1;
		rc = 0;
	}
	LOG("rc=%d, *pRes=%d", rc, *pRes);
	return errmap(rc);
}

/*
** Rollback the transaction in progress.  All cursors will be
** invalidated by this operation.  Any attempt to use a cursor
** that was open at the beginning of this operation will result
** in an error.
*/
int sqlite3BtreeRollback(Btree *p, int tripCode){
  LOG("done",0);
  return sqlite3BtreeSavepoint(p, SAVEPOINT_ROLLBACK, -1);
}

/*
** The second argument to this function, op, is always SAVEPOINT_ROLLBACK
** or SAVEPOINT_RELEASE. This function either releases or rolls back the
** savepoint identified by parameter iSavepoint, depending on the value 
** of op.
**
** Normally, iSavepoint is greater than or equal to zero. However, if op is
** SAVEPOINT_ROLLBACK, then iSavepoint may also be -1. In this case the 
** contents of the entire transaction are rolled back. This is different
** from a normal transaction rollback, as no locks are released and the
** transaction remains open.
*/
int sqlite3BtreeSavepoint(Btree *p, int op, int iSavepoint){
  KVS_txn *parent;
  int rc = SQLITE_OK;

  if (!p->curr_txn)
    goto done;

  rc = errmap(kvs_txn_parent(p->curr_txn, &parent));
  if(SQLITE_OK != rc) goto done;

  if (op == SAVEPOINT_ROLLBACK) {
    if (iSavepoint == -1) {
      kvs_txn_abort(p->main_txn);
    } else {
      kvs_txn_abort(p->curr_txn);
    }
  } else {
    if (iSavepoint == -1)
      rc = kvs_txn_commit(p->main_txn);
    else
      rc = kvs_txn_commit(p->curr_txn);
  }
  if (iSavepoint == -1) {
    p->main_txn = NULL;
    p->curr_txn = NULL;
    p->inTrans = TRANS_NONE;
  } else {
    p->curr_txn = parent;
      if (!parent) {
        p->main_txn = NULL;
        p->inTrans = TRANS_NONE;
      }
  }
done:
  LOG("rc=%d",rc);
  return errmap(rc);
}

/*
** This function returns a pointer to a blob of memory associated with
** a single shared-btree. The memory is used by client code for its own
** purposes (for example, to store a high-level schema associated with 
** the shared-btree). The btree layer manages reference counting issues.
**
** The first time this is called on a shared-btree, nBytes bytes of memory
** are allocated, zeroed, and returned to the caller. For each subsequent 
** call the nBytes parameter is ignored and a pointer to the same blob
** of memory returned. 
**
** If the nBytes parameter is 0 and the blob of memory has not yet been
** allocated, a null pointer is returned. If the blob has already been
** allocated, it is returned as normal.
**
** Just before the shared-btree is closed, the function passed as the 
** xFree argument when the memory allocation was made is invoked on the 
** blob of allocated memory. The xFree function should not call sqlite3_free()
** on the memory, the btree layer does that.
*/
void *sqlite3BtreeSchema(Btree *p, int nBytes, void(*xFree)(void *)){
  if (p->pBt->pSchema == NULL && nBytes > 0) {
    p->pBt->pSchema = sqlite3MallocZero(nBytes);
	p->pBt->xFreeSchema = xFree;
  }
  LOG("done",0);
  return p->pBt->pSchema;
}

/*
** Return SQLITE_LOCKED_SHAREDCACHE if another user of the same shared 
** btree as the argument handle holds an exclusive lock on the 
** sqlite_master table. Otherwise SQLITE_OK.
*/
int sqlite3BtreeSchemaLocked(Btree *p){
  LOG("rc=0",0);
  return SQLITE_OK;
}

/*
** Change the limit on the number of pages allowed in the cache.
**
** The maximum number of cache pages is set to the absolute
** value of mxPage.  If mxPage is negative, the pager will
** operate asynchronously - it will not stop to do fsync()s
** to insure data is written to the disk surface before
** continuing.  Transactions still work if synchronous is off,
** and the database cannot be corrupted if this program
** crashes.  But if the operating system crashes or there is
** an abrupt power failure when synchronous is off, the database
** could be left in an inconsistent and unrecoverable state.
** Synchronous is on by default so database corruption is not
** normally a worry.
*/
int sqlite3BtreeSetCacheSize(Btree *p, int mxPage){
  LOG("done",0);
  return SQLITE_OK;
}

/*
** Change the limit on the amount of the database file that may be
** memory mapped.
*/
int sqlite3BtreeSetMmapLimit(Btree *p, sqlite3_int64 szMmap){
  return SQLITE_OK;
}

/*
** Set the cached rowid value of every cursor in the same database file
** as pCur and having the same root page number as pCur.  The value is
** set to iRowid.
**
** Only positive rowid values are considered valid for this cache.
** The cache is initialized to zero, indicating an invalid cache.
** A btree will work fine with zero or negative rowids.  We just cannot
** cache zero or negative rowids, which means tables that use zero or
** negative rowids might run a little slower.  But in practice, zero
** or negative rowids are very uncommon so this should not be a problem.
*/
void sqlite3BtreeSetCachedRowid(BtCursor *pCur, sqlite3_int64 iRowid){
	BtShared *pBt;
	BtCursor *pc;
	Btree *p;
	pBt = pCur->pBtree->pBt;

	for (p=pBt->trees; p; p=p->pNext) {
		for (pc=p->pCursor; pc; pc=pc->pNext) {
			if (pc->iTable != pCur->iTable) continue;
			pc->cachedRowid = iRowid;
		}
	}
	LOG("done",0);
}

/*
** Change the default pages size and the number of reserved bytes per page.
** Or, if the page size has already been fixed, return SQLITE_READONLY 
** without changing anything.
**
** The page size must be a power of 2 between 512 and 65536.  If the page
** size supplied does not meet this constraint then the page size is not
** changed.
**
** Page sizes are constrained to be a power of two so that the region
** of the database file used for locking (beginning at PENDING_BYTE,
** the first byte past the 1GB boundary, 0x40000000) needs to occur
** at the beginning of a page.
**
** If parameter nReserve is less than zero, then the number of reserved
** bytes per page is left unchanged.
**
** If the iFix!=0 then the pageSizeFixed flag is set so that the page size
** and autovacuum mode can no longer be changed.
*/
int sqlite3BtreeSetPageSize(Btree *p, int pageSize, int nReserve, int iFix){
  LOG("done",0);
	return SQLITE_READONLY;
}

/*
** Change the way data is synced to disk in order to increase or decrease
** how well the database resists damage due to OS crashes and power
** failures.  Level 1 is the same as asynchronous (no syncs() occur and
** there is a high probability of damage)  Level 2 is the default.  There
** is a very low but non-zero probability of damage.  Level 3 reduces the
** probability of damage to near zero but with a write performance reduction.
*/
#ifndef SQLITE_OMIT_PAGER_PRAGMAS
int sqlite3BtreeSetSafetyLevel(
  Btree *p,              /* The btree to set the safety level on */
  int level,             /* PRAGMA synchronous.  1=OFF, 2=NORMAL, 3=FULL */
  int fullSync,          /* PRAGMA fullfsync. */
  int ckptFullSync       /* PRAGMA checkpoint_fullfync */
){
	unsigned flags = 0;
	kvs_env_get_config(p->pBt->env, KVS_CFG_FLAGS, &flags);
	if(level < 2) flags |= KVS_NOSYNC;
	else flags &= ~KVS_NOSYNC;
	int rc = kvs_env_set_config(p->pBt->env, KVS_CFG_FLAGS, &flags);
	LOG("done", 0);
	return SQLITE_OK;
}
#endif

/*
** Set both the "read version" (single byte at byte offset 18) and 
** "write version" (single byte at byte offset 19) fields in the database
** header to iVersion.
*/
int sqlite3BtreeSetVersion(Btree *pBtree, int iVersion){
  LOG("done",0);
  return SQLITE_OK;
}

void sqlite3BtreeCursorHints(BtCursor *pCsr, unsigned int mask) {
	/* could use BTREE_BULKLOAD */
}

/*
** Return TRUE if the given btree is set to safety level 1.  In other
** words, return TRUE if no sync() occurs on the disk files.
*/
int sqlite3BtreeSyncDisabled(Btree *p){
	unsigned flags = 0;
	LOG("done",0);
	kvs_env_get_config(p->pBt->env, KVS_CFG_FLAGS, &flags);
	return (flags & KVS_NOSYNC) != 0;
}

/*
** This routine sets the state to CURSOR_FAULT and the error
** code to errCode for every cursor on BtShared that pBtree
** references.
**
** Every cursor is tripped, including cursors that belong
** to other database connections that happen to be sharing
** the cache with pBtree.
**
** This is a no-op here since cursors in other transactions
** are fully isolated from the write transaction.
*/
void sqlite3BtreeTripAllCursors(Btree *pBtree, int errCode){
  LOG("done",0);
  /* no-op */
}

/*
** Write meta-information back into the database.  Meta[0] is
** read-only and may not be written.
*/
int sqlite3BtreeUpdateMeta(Btree *p, int idx, u32 iMeta){
	assert(idx > 0 && idx < NUMMETA);
	KVS_val key[1], val[1];
	KVS_VAL_STORAGE(key, KVS_VARINT_MAX*2);
	kvs_bind_uint64(key, TABLE_META);
	kvs_bind_uint64(key, idx);
	KVS_VAL_STORAGE(val, KVS_VARINT_MAX);
	kvs_bind_uint64(val, iMeta);
	int rc = kvs_put(p->curr_txn, key, val, 0);
	LOG("rc=%d, idx=%d, iMeta=%u",rc,idx,iMeta);
	return errmap(rc);
}

#ifndef SQLITE_OMIT_SHARED_CACHE
/*
** Enable or disable the shared pager and schema features.
**
** This routine has no effect on existing database connections.
** The shared cache setting effects only future calls to
** sqlite3_open(), sqlite3_open16(), or sqlite3_open_v2().
*/
int sqlite3_enable_shared_cache(int enable){
  sqlite3GlobalConfig.sharedCacheEnabled = enable;
  LOG("done",0);
  return SQLITE_OK;
}
#endif
