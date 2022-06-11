/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tsdb.h"

typedef struct SCommitter SCommitter;

struct SCommitter {
  STsdb   *pTsdb;
  uint8_t *pBuf1;
  uint8_t *pBuf2;
  uint8_t *pBuf3;
  uint8_t *pBuf4;
  uint8_t *pBuf5;
  /* commit data */
  int32_t minutes;
  int8_t  precision;
  TSKEY   nextCommitKey;
  // commit file data
  int32_t          commitFid;
  TSKEY            minKey;
  TSKEY            maxKey;
  SDFileSetReader *pReader;
  SDFileSetWriter *pWriter;
  SArray          *aOBlockIdx;
  SArray          *aBlockIdx;
  // commit table data
  STbData   *pTbData;
  SBlockIdx *pBlockIdx;
  /* commit del */
  SDelFReader *pDelFReader;
  SDelFWriter *pDelFWriter;
  SDelIdx      oDelIdx;
  SDelIdx      nDelIdx;
  SDelData     oDelData;
  SDelData     nDelData;
  SDelIdxItem  delIdxItem;
  /* commit cache */
};

static int32_t tsdbStartCommit(STsdb *pTsdb, SCommitter *pCommitter);
static int32_t tsdbCommitData(SCommitter *pCommitter);
static int32_t tsdbCommitDel(SCommitter *pCommitter);
static int32_t tsdbCommitCache(SCommitter *pCommitter);
static int32_t tsdbEndCommit(SCommitter *pCommitter, int32_t eno);

int32_t tsdbBegin(STsdb *pTsdb) {
  int32_t code = 0;

  code = tsdbMemTableCreate(pTsdb, &pTsdb->mem);
  if (code) {
    goto _err;
  }

  return code;

_err:
  return code;
}

int32_t tsdbCommit(STsdb *pTsdb) {
  int32_t    code = 0;
  SCommitter commith = {0};
  int        fid;

  // start commit
  code = tsdbStartCommit(pTsdb, &commith);
  if (code) {
    goto _err;
  }

  // commit impl
  code = tsdbCommitData(&commith);
  if (code) {
    goto _err;
  }

  code = tsdbCommitDel(&commith);
  if (code) {
    goto _err;
  }

  code = tsdbCommitCache(&commith);
  if (code) {
    goto _err;
  }

  // end commit
  code = tsdbEndCommit(&commith, 0);
  if (code) {
    goto _err;
  }

  return code;

_err:
  tsdbError("vgId:%d failed to commit since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

// ===================================== NEW ======================================
static int32_t tsdbStartCommit(STsdb *pTsdb, SCommitter *pCommitter) {
  int32_t code = 0;

  ASSERT(pTsdb->mem && pTsdb->imem == NULL);
  // lock();
  pTsdb->imem = pTsdb->mem;
  pTsdb->mem = NULL;
  // unlock();

  pCommitter->pTsdb = pTsdb;

  return code;
}

static int32_t tsdbCommitDataStart(SCommitter *pCommitter);
static int32_t tsdbCommitDataImpl(SCommitter *pCommitter);
static int32_t tsdbCommitDataEnd(SCommitter *pCommitter);

static int32_t tsdbCommitData(SCommitter *pCommitter) {
  int32_t    code = 0;
  STsdb     *pTsdb = pCommitter->pTsdb;
  SMemTable *pMemTable = pTsdb->imem;

  // no data, just return
  if (pMemTable->nRow == 0) {
    tsdbDebug("vgId:%d no data to commit", TD_VID(pTsdb->pVnode));
    goto _exit;
  }

  // start
  code = tsdbCommitDataStart(pCommitter);
  if (code) {
    goto _err;
  }

  // commit
  code = tsdbCommitDataImpl(pCommitter);
  if (code) {
    goto _err;
  }

  // end
  code = tsdbCommitDataEnd(pCommitter);
  if (code) {
    goto _err;
  }

_exit:
  tsdbDebug("vgId:%d commit TSDB data, nRow:%" PRId64, TD_VID(pTsdb->pVnode), pMemTable->nRow);
  return code;

_err:
  tsdbError("vgId:%d failed to commit TSDB data since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

static int32_t tsdbCommitDelStart(SCommitter *pCommitter) {
  int32_t    code = 0;
  STsdb     *pTsdb = pCommitter->pTsdb;
  SMemTable *pMemTable = pTsdb->imem;
  SDelFile  *pDelFileR = NULL;  // TODO
  SDelFile  *pDelFileW = NULL;  // TODO

  // load old
  pCommitter->oDelIdx = (SDelIdx){0};
  if (pDelFileR) {
    code = tsdbDelFReaderOpen(&pCommitter->pDelFReader, pDelFileR, pTsdb, NULL);
    if (code) {
      goto _err;
    }

    code = tsdbReadDelIdx(pCommitter->pDelFReader, &pCommitter->oDelIdx, &pCommitter->pBuf1);
    if (code) {
      goto _err;
    }
  }

  // prepare new
  pCommitter->nDelIdx = (SDelIdx){0};
  code = tsdbDelFWriterOpen(&pCommitter->pDelFWriter, pDelFileW, pTsdb);
  if (code) {
    goto _err;
  }

_exit:
  tsdbDebug("vgId:%d commit del start", TD_VID(pTsdb->pVnode));
  return code;

_err:
  tsdbError("vgId:%d commit del start failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

static int32_t tsdbCommitTableDel(SCommitter *pCommitter);

static int32_t tsdbCommitDelImpl(SCommitter *pCommitter) {
  int32_t    code = 0;
  STsdb     *pTsdb = pCommitter->pTsdb;
  SMemTable *pMemTable = pTsdb->imem;
  int32_t    iTbData = 0;
  int32_t    nTbData = taosArrayGetSize(pMemTable->aTbData);
  int32_t    iDelIdx = 0;
  int32_t    nDelIdx = 0;  // TODO
  STbData   *pTbData = NULL;
  SDelIdx   *pDelIdx = NULL;
  SDelIdx    delIdx;

  // if (iTbData < nTbData) {
  //   pTbData = (STbData *)taosArrayGetP(pMemTable->aTbData, iTbData);
  // }
  // if (iDelIdx < nDelIdx) {
  //   // tIMapGet();
  //   pDelIdx = &delIdx;
  // }

  while (iTbData < nTbData || iDelIdx < nDelIdx) {
    if (pTbData && pDelIdx) {
    } else {
    }

    code = tsdbCommitTableDel(pCommitter);
    if (code) goto _err;
  }

  return code;

_err:
  tsdbError("vgId:%d commit del impl failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

static int32_t tsdbCommitDelEnd(SCommitter *pCommitter) {
  int32_t code = 0;

  code = tsdbWriteDelIdx(pCommitter->pDelFWriter, &pCommitter->nDelIdx, NULL);
  if (code) {
    goto _err;
  }

  code = tsdbUpdateDelFileHdr(pCommitter->pDelFWriter, NULL);
  if (code) {
    goto _err;
  }

  code = tsdbDelFWriterClose(pCommitter->pDelFWriter, 1);
  if (code) {
    goto _err;
  }

  if (pCommitter->pDelFReader) {
    code = tsdbDelFReaderClose(pCommitter->pDelFReader);
    if (code) goto _err;
  }

  return code;

_err:
  tsdbError("vgId:%d commit del end failed since %s", TD_VID(pCommitter->pTsdb->pVnode), tstrerror(code));
  return code;
}

static int32_t tsdbCommitDel(SCommitter *pCommitter) {
  int32_t    code = 0;
  STsdb     *pTsdb = pCommitter->pTsdb;
  SMemTable *pMemTable = pTsdb->imem;

  if (pMemTable->nDel == 0) {
    goto _exit;
  }

  // start
  code = tsdbCommitDelStart(pCommitter);
  if (code) {
    goto _err;
  }

  // impl
  code = tsdbCommitDelImpl(pCommitter);
  if (code) {
    goto _err;
  }

  // end
  code = tsdbCommitDelEnd(pCommitter);
  if (code) {
    goto _err;
  }

_exit:
  tsdbDebug("vgId:%d commit del data done, nDel:%" PRId64, TD_VID(pTsdb->pVnode), pMemTable->nDel);
  return code;

_err:
  tsdbError("vgId:%d commit del data failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

static int32_t tsdbCommitCache(SCommitter *pCommitter) {
  int32_t code = 0;
  // TODO
  return code;
}

static int32_t tsdbEndCommit(SCommitter *pCommitter, int32_t eno) {
  int32_t code = 0;
  // TODO
  return code;
}

static int32_t tsdbCommitDataStart(SCommitter *pCommitter) {
  int32_t    code = 0;
  STsdb     *pTsdb = pCommitter->pTsdb;
  SMemTable *pMemTable = pTsdb->imem;

  pCommitter->nextCommitKey = pMemTable->minKey.ts;

  return code;
}

static int32_t tsdbCommitFileData(SCommitter *pCommitter);

static int32_t tsdbCommitDataImpl(SCommitter *pCommitter) {
  int32_t code = 0;

  for (;;) {
    if (pCommitter->nextCommitKey == TSKEY_MAX) break;

    pCommitter->commitFid = TSDB_KEY_FID(pCommitter->nextCommitKey, pCommitter->minutes, pCommitter->precision);
    code = tsdbCommitFileData(pCommitter);
    if (code) {
      goto _err;
    }
  }

_exit:
  return code;

_err:
  return code;
}

static int32_t tsdbCommitDataEnd(SCommitter *pCommitter) {
  int32_t code = 0;
  // TODO
  return code;
}

static int32_t tsdbCommitFileDataStart(SCommitter *pCommitter);
static int32_t tsdbCommitFileDataImpl(SCommitter *pCommitter);
static int32_t tsdbCommitFileDataEnd(SCommitter *pCommitter);

static int32_t tsdbCommitFileData(SCommitter *pCommitter) {
  int32_t code = 0;

  // commit file data start
  code = tsdbCommitFileDataStart(pCommitter);
  if (code) {
    goto _err;
  }

  // commit file data impl
  code = tsdbCommitFileDataImpl(pCommitter);
  if (code) {
    goto _err;
  }

  // commit file data end
  code = tsdbCommitFileDataEnd(pCommitter);
  if (code) {
    goto _err;
  }

  return code;

_err:
  return code;
}

static int32_t tsdbCommitFileDataStart(SCommitter *pCommitter) {
  int32_t    code = 0;
  STsdb     *pTsdb = pCommitter->pTsdb;
  SDFileSet *pRSet = NULL;
  SDFileSet *pWSet = NULL;

  taosArrayClear(pCommitter->aOBlockIdx);
  taosArrayClear(pCommitter->aBlockIdx);

  // search pRSet (todo)

  // open file to read
  if (pRSet) {
    code = tsdbDFileSetReaderOpen(pCommitter->pReader, pTsdb, pRSet);
    if (code) goto _err;

    // create/open the pWSet (todo)
  } else {
    // create the pWSet (todo)
  }

  // open file for write
  code = tsdbDFileSetWriterOpen(pCommitter->pWriter, pTsdb, pWSet);
  if (code) goto _err;

  // read the SBlockIdx part for merge purpose
  code = tsdbLoadSBlockIdx(pCommitter->pReader, pCommitter->aOBlockIdx);
  if (code) goto _err;

_exit:
  return code;

_err:
  return code;
}

static int32_t tsdbCommitTableData(SCommitter *pCommitter);

static int32_t tsdbCommitFileDataImpl(SCommitter *pCommitter) {
  int32_t    code = 0;
  STsdb     *pTsdb = pCommitter->pTsdb;
  SMemTable *pMemTable = pTsdb->imem;
  int32_t    iBlockIdx = 0;
  int32_t    nBlockIdx = taosArrayGetSize(pCommitter->aOBlockIdx);
  int32_t    iTbData = 0;
  int32_t    nTbData = taosArrayGetSize(pMemTable->aTbData);
  SBlockIdx *pBlockIdx;
  STbData   *pTbData;

  while (iTbData < nTbData || iBlockIdx < nBlockIdx) {
    pTbData = NULL;
    pBlockIdx = NULL;

    if (iTbData < nTbData) {
      pTbData = (STbData *)taosArrayGetP(pMemTable->aTbData, iTbData);
    }
    if (iBlockIdx < nBlockIdx) {
      pBlockIdx = (SBlockIdx *)taosArrayGet(pCommitter->aOBlockIdx, iBlockIdx);
    }

    if (pTbData && pBlockIdx) {
      int32_t c = tTABLEIDCmprFn(pTbData, pBlockIdx);
      if (c == 0) {
        iTbData++;
        iBlockIdx++;
      } else if (c < 0) {
        iTbData++;
        pBlockIdx = NULL;
      } else {
        iBlockIdx++;
        pTbData = NULL;
      }
    } else {
      if (pTbData) {
        iTbData++;
      } else {
        iBlockIdx++;
      }
    }

    pCommitter->pTbData = pTbData;
    pCommitter->pBlockIdx = pBlockIdx;
    code = tsdbCommitTableData(pCommitter);
    if (code) {
      goto _err;
    }
  }

  return code;

_err:
  return code;
}

static int32_t tsdbCommitFileDataEnd(SCommitter *pCommitter) {
  int32_t code = 0;
  // TODO
  return code;
}

static int32_t tsdbCommitTableDataStart(SCommitter *pCommitter);
static int32_t tsdbCommitTableDataImpl(SCommitter *pCommitter);
static int32_t tsdbCommitTableDataEnd(SCommitter *pCommitter);

static int32_t tsdbCommitTableData(SCommitter *pCommitter) {
  int32_t code = 0;

  // start
  code = tsdbCommitTableDataStart(pCommitter);
  if (code) {
    goto _err;
  }

  // impl
  code = tsdbCommitTableDataImpl(pCommitter);
  if (code) {
    goto _err;
  }

  // end
  code = tsdbCommitTableDataEnd(pCommitter);
  if (code) {
    goto _err;
  }

_exit:
  return code;

_err:
  return code;
}

static int32_t tsdbCommitTableDataStart(SCommitter *pCommitter) {
  int32_t code = 0;
  // TODO
  return code;
}

static int32_t tsdbCommitTableDataImpl(SCommitter *pCommitter) {
  int32_t code = 0;
  // TODO
  return code;
}

static int32_t tsdbCommitTableDataEnd(SCommitter *pCommitter) {
  int32_t code = 0;
  // TODO
  return code;
}

static int32_t tsdbCommitTableDelStart(SCommitter *pCommitter) {
  int32_t code = 0;

  // load old
  pCommitter->oDelData = (SDelData){0};
  if (0) {
    code = tsdbReadDelData(pCommitter->pDelFReader, NULL /*TODO*/, &pCommitter->oDelData, &pCommitter->pBuf4);
    if (code) goto _err;
  }

  // prepare new
  pCommitter->nDelData = (SDelData){0};

  return code;

_err:
  tsdbError("vgId:%d commit table del start failed since %s", TD_VID(pCommitter->pTsdb->pVnode), tstrerror(code));
  return code;
}

static int32_t tsdbCommitTableDelImpl(SCommitter *pCommitter) {
  int32_t code = 0;
  SDelOp *pDelOp = NULL;

  for (; pDelOp; pDelOp = pDelOp->pNext) {
    SDelDataItem item = {.version = pDelOp->version, .sKey = pDelOp->sKey, .eKey = pDelOp->eKey};

    code = tDelDataPutItem(&pCommitter->nDelData, &item);
    if (code) goto _err;
  }

  return code;

_err:
  return code;
}

static int32_t tsdbCommitTableDelEnd(SCommitter *pCommitter) {
  int32_t code = 0;

  // write table del data
  code = tsdbWriteDelData(pCommitter->pDelFWriter, &pCommitter->nDelData, NULL);
  if (code) goto _err;

  // add SDelIdxItem
  code = tDelIdxPutItem(&pCommitter->nDelIdx, &pCommitter->delIdxItem);
  if (code) goto _err;

  return code;

_err:
  tsdbError("vgId:%d commit table del end failed since %s", TD_VID(pCommitter->pTsdb->pVnode), tstrerror(code));
  return code;
}

static int32_t tsdbCommitTableDel(SCommitter *pCommitter) {
  int32_t code = 0;

  // start
  code = tsdbCommitTableDelStart(pCommitter);
  if (code) goto _err;

  // impl
  code = tsdbCommitTableDelImpl(pCommitter);
  if (code) goto _err;

  // end
  code = tsdbCommitTableDelEnd(pCommitter);
  if (code) goto _err;

  return code;

_err:
  return code;
}

// // ===================================== OLD ======================================
#if 0
struct SCommitIter {
  STable      *pTable;
  STbDataIter *pIter;
};

#define TSDB_MAX_SUBBLOCKS               8
#define TSDB_DEFAULT_BLOCK_ROWS(maxRows) ((maxRows)*4 / 5)
#define TSDB_COMMIT_REPO(ch)             TSDB_READ_REPO(&(ch->readh))
#define TSDB_COMMIT_REPO_ID(ch)          REPO_ID(TSDB_READ_REPO(&(ch->readh)))
#define TSDB_COMMIT_WRITE_FSET(ch)       (&((ch)->wSet))
#define TSDB_COMMIT_TABLE(ch)            ((ch)->pTable)
#define TSDB_COMMIT_HEAD_FILE(ch)        TSDB_DFILE_IN_SET(TSDB_COMMIT_WRITE_FSET(ch), TSDB_FILE_HEAD)
#define TSDB_COMMIT_DATA_FILE(ch)        TSDB_DFILE_IN_SET(TSDB_COMMIT_WRITE_FSET(ch), TSDB_FILE_DATA)
#define TSDB_COMMIT_LAST_FILE(ch)        TSDB_DFILE_IN_SET(TSDB_COMMIT_WRITE_FSET(ch), TSDB_FILE_LAST)
#define TSDB_COMMIT_SMAD_FILE(ch)        TSDB_DFILE_IN_SET(TSDB_COMMIT_WRITE_FSET(ch), TSDB_FILE_SMAD)
#define TSDB_COMMIT_SMAL_FILE(ch)        TSDB_DFILE_IN_SET(TSDB_COMMIT_WRITE_FSET(ch), TSDB_FILE_SMAL)
#define TSDB_COMMIT_BUF(ch)              TSDB_READ_BUF(&((ch)->readh))
#define TSDB_COMMIT_COMP_BUF(ch)         TSDB_READ_COMP_BUF(&((ch)->readh))
#define TSDB_COMMIT_EXBUF(ch)            TSDB_READ_EXBUF(&((ch)->readh))
#define TSDB_COMMIT_DEFAULT_ROWS(ch)     TSDB_DEFAULT_BLOCK_ROWS(TSDB_COMMIT_REPO(ch)->pVnode->config.tsdbCfg.maxRows)
#define TSDB_COMMIT_TXN_VERSION(ch)      FS_TXN_VERSION(REPO_FS(TSDB_COMMIT_REPO(ch)))

static int     tsdbInitCommitH(SCommitter *pCommith, STsdb *pRepo);
static void    tsdbSeekCommitIter(SCommitter *pCommith, TSKEY key);
static int     tsdbNextCommitFid(SCommitter *pCommith);
static void    tsdbDestroyCommitH(SCommitter *pCommith);
static int32_t tsdbCreateCommitIters(SCommitter *pCommith);
static void    tsdbDestroyCommitIters(SCommitter *pCommith);
static int     tsdbCommitToFile(SCommitter *pCommith, SDFileSet *pSet, int fid);
static int     tsdbSetAndOpenCommitFile(SCommitter *pCommith, SDFileSet *pSet, int fid);
static int     tsdbCommitToTable(SCommitter *pCommith, int tid);
static bool    tsdbCommitIsSameFile(SCommitter *pCommith, int bidx);
static int     tsdbMoveBlkIdx(SCommitter *pCommith, SBlockIdx *pIdx);
static int     tsdbSetCommitTable(SCommitter *pCommith, STable *pTable);
static int     tsdbComparKeyBlock(const void *arg1, const void *arg2);
static int     tsdbWriteBlockInfo(SCommitter *pCommih);
static int     tsdbCommitMemData(SCommitter *pCommith, SCommitIter *pIter, TSKEY keyLimit, bool toData);
static int     tsdbMergeMemData(SCommitter *pCommith, SCommitIter *pIter, int bidx);
static int     tsdbMoveBlock(SCommitter *pCommith, int bidx);
static int  tsdbCommitAddBlock(SCommitter *pCommith, const SBlock *pSupBlock, const SBlock *pSubBlocks, int
nSubBlocks); static int  tsdbMergeBlockData(SCommitter *pCommith, SCommitIter *pIter, SDataCols *pDataCols, TSKEY
keyLimit,
                               bool isLastOneBlock);
static void tsdbResetCommitTable(SCommitter *pCommith);
static void tsdbCloseCommitFile(SCommitter *pCommith, bool hasError);
static bool tsdbCanAddSubBlock(SCommitter *pCommith, SBlock *pBlock, SMergeInfo *pInfo);
static void tsdbLoadAndMergeFromCache(STsdb *pTsdb, SDataCols *pDataCols, int *iter, SCommitIter *pCommitIter,
                                      SDataCols *pTarget, TSKEY maxKey, int maxRows, int8_t update);
static int  tsdbWriteBlockIdx(SDFile *pHeadf, SArray *pIdxA, void **ppBuf);
static int  tsdbApplyRtnOnFSet(STsdb *pRepo, SDFileSet *pSet, SRtn *pRtn);
static int  tsdbLoadDataFromCache(STsdb *pTsdb, STable *pTable, STbDataIter *pIter, TSKEY maxKey, int maxRowsToRead,
                                  SDataCols *pCols, TKEY *filterKeys, int nFilterKeys, bool keepDup,
                                  SMergeInfo *pMergeInfo);

static int32_t tsdbCommitData(SCommitter *pCommith) {
  int32_t    fid;
  SDFileSet *pSet = NULL;
  int32_t    code = 0;
  STsdb     *pTsdb = TSDB_COMMIT_REPO(pCommith);

  // Skip expired memory data and expired FSET
  tsdbSeekCommitIter(pCommith, pCommith->rtn.minKey);
  while ((pSet = tsdbFSIterNext(&(pCommith->fsIter)))) {
    if (pSet->fid < pCommith->rtn.minFid) {
      tsdbInfo("vgId:%d, FSET %d on level %d disk id %d expires, remove it", REPO_ID(pTsdb), pSet->fid,
               TSDB_FSET_LEVEL(pSet), TSDB_FSET_ID(pSet));
    } else {
      break;
    }
  }

  // commit
  fid = tsdbNextCommitFid(pCommith);
  while (true) {
    // Loop over both on disk and memory
    if (pSet == NULL && fid == TSDB_IVLD_FID) break;

    if (pSet && (fid == TSDB_IVLD_FID || pSet->fid < fid)) {
      // Only has existing FSET but no memory data to commit in this
      // existing FSET, only check if file in correct retention
      if (tsdbApplyRtnOnFSet(TSDB_COMMIT_REPO(pCommith), pSet, &(pCommith->rtn)) < 0) {
        tsdbDestroyCommitH(pCommith);
        return -1;
      }

      pSet = tsdbFSIterNext(&(pCommith->fsIter));
    } else {
      // Has memory data to commit
      SDFileSet *pCSet;
      int        cfid;

      if (pSet == NULL || pSet->fid > fid) {
        // Commit to a new FSET with fid: fid
        pCSet = NULL;
        cfid = fid;
      } else {
        // Commit to an existing FSET
        pCSet = pSet;
        cfid = pSet->fid;
        pSet = tsdbFSIterNext(&(pCommith->fsIter));
      }

      if (tsdbCommitToFile(pCommith, pCSet, cfid) < 0) {
        tsdbDestroyCommitH(pCommith);
        return -1;
      }

      fid = tsdbNextCommitFid(pCommith);
    }
  }

  return code;
}

static int32_t tsdbCommitDel(SCommitter *pCommith) {
  int32_t code = 0;
  // TODO
  return code;
}

static int32_t tsdbCommitCache(SCommitter *pCommith) {
  int32_t code = 0;
  // TODO
  return code;
}

static int tsdbApplyRtnOnFSet(STsdb *pRepo, SDFileSet *pSet, SRtn *pRtn) {
  SDiskID   did;
  SDFileSet nSet = {0};
  STsdbFS  *pfs = REPO_FS(pRepo);
  int       level;

  ASSERT(pSet->fid >= pRtn->minFid);

  level = tsdbGetFidLevel(pSet->fid, pRtn);

  if (tfsAllocDisk(pRepo->pVnode->pTfs, level, &did) < 0) {
    terrno = TSDB_CODE_TDB_NO_AVAIL_DISK;
    return -1;
  }

  if (did.level > TSDB_FSET_LEVEL(pSet)) {
    // Need to move the FSET to higher level
    tsdbInitDFileSet(pRepo, &nSet, did, pSet->fid, FS_TXN_VERSION(pfs));

    if (tsdbCopyDFileSet(pSet, &nSet) < 0) {
      tsdbError("vgId:%d, failed to copy FSET %d from level %d to level %d since %s", REPO_ID(pRepo), pSet->fid,
                TSDB_FSET_LEVEL(pSet), did.level, tstrerror(terrno));
      return -1;
    }

    if (tsdbUpdateDFileSet(pfs, &nSet) < 0) {
      return -1;
    }

    tsdbInfo("vgId:%d, FSET %d is copied from level %d disk id %d to level %d disk id %d", REPO_ID(pRepo), pSet->fid,
             TSDB_FSET_LEVEL(pSet), TSDB_FSET_ID(pSet), did.level, did.id);
  } else {
    // On a correct level
    if (tsdbUpdateDFileSet(pfs, pSet) < 0) {
      return -1;
    }
  }

  return 0;
}

static int32_t tsdbStartCommit(STsdb *pTsdb, SCommitter *pCHandle) {
  int32_t code = 0;

  tsdbInfo("vgId:%d, start to commit", REPO_ID(pTsdb));

  ASSERT(pTsdb->imem == NULL && pTsdb->mem);
  pTsdb->imem = pTsdb->mem;
  pTsdb->mem = NULL;
  if (tsdbInitCommitH(pCHandle, pTsdb) < 0) {
    return -1;
  }

  tsdbStartFSTxn(pTsdb, 0, 0);

  return code;
}

static int32_t tsdbEndCommit(SCommitter *pCHandle, int eno) {
  int32_t code = 0;
  STsdb  *pTsdb = TSDB_COMMIT_REPO(pCHandle);

  tsdbDestroyCommitH(pCHandle);
  tsdbEndFSTxn(pTsdb);
  tsdbMemTableDestroy(pTsdb->imem);
  pTsdb->imem = NULL;

  tsdbInfo("vgId:%d, commit over, %s", REPO_ID(pTsdb), (eno == TSDB_CODE_SUCCESS) ? "succeed" : "failed");

  return code;
}

static int tsdbInitCommitH(SCommitter *pCommith, STsdb *pRepo) {
  STsdbCfg *pCfg = REPO_CFG(pRepo);

  memset(pCommith, 0, sizeof(*pCommith));
  tsdbGetRtnSnap(pRepo, &(pCommith->rtn));

  TSDB_FSET_SET_CLOSED(TSDB_COMMIT_WRITE_FSET(pCommith));

  // Init read handle
  if (tsdbInitReadH(&(pCommith->readh), pRepo) < 0) {
    return -1;
  }

  // Init file iterator
  tsdbFSIterInit(&(pCommith->fsIter), REPO_FS(pRepo), TSDB_FS_ITER_FORWARD);

  if (tsdbCreateCommitIters(pCommith) < 0) {
    tsdbDestroyCommitH(pCommith);
    return -1;
  }

  pCommith->aBlkIdx = taosArrayInit(1024, sizeof(SBlockIdx));
  if (pCommith->aBlkIdx == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    tsdbDestroyCommitH(pCommith);
    return -1;
  }

  pCommith->aSupBlk = taosArrayInit(1024, sizeof(SBlock));
  if (pCommith->aSupBlk == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    tsdbDestroyCommitH(pCommith);
    return -1;
  }

  pCommith->aSubBlk = taosArrayInit(1024, sizeof(SBlock));
  if (pCommith->aSubBlk == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    tsdbDestroyCommitH(pCommith);
    return -1;
  }

  pCommith->pDataCols = tdNewDataCols(0, pCfg->maxRows);
  if (pCommith->pDataCols == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    tsdbDestroyCommitH(pCommith);
    return -1;
  }

  return 0;
}

// Skip all keys until key (not included)
static void tsdbSeekCommitIter(SCommitter *pCommith, TSKEY key) {
  for (int i = 0; i < pCommith->niters; i++) {
    SCommitIter *pIter = pCommith->iters + i;
    if (pIter->pTable == NULL || pIter->pIter == NULL) continue;

    tsdbLoadDataFromCache(TSDB_COMMIT_REPO(pCommith), pIter->pTable, pIter->pIter, key - 1, INT32_MAX, NULL, NULL, 0,
                          true, NULL);
  }
}

static int tsdbNextCommitFid(SCommitter *pCommith) {
  STsdb        *pRepo = TSDB_COMMIT_REPO(pCommith);
  STsdbKeepCfg *pCfg = REPO_KEEP_CFG(pRepo);
  int           fid = TSDB_IVLD_FID;

  for (int i = 0; i < pCommith->niters; i++) {
    SCommitIter *pIter = pCommith->iters + i;
    // if (pIter->pTable == NULL || pIter->pIter == NULL) continue;

    TSKEY nextKey = tsdbNextIterKey(pIter->pIter);
    if (nextKey == TSDB_DATA_TIMESTAMP_NULL) {
      continue;
    } else {
      int tfid = (int)(TSDB_KEY_FID(nextKey, pCfg->days, pCfg->precision));
      if (fid == TSDB_IVLD_FID || fid > tfid) {
        fid = tfid;
      }
    }
  }

  return fid;
}

static void tsdbDestroyCommitH(SCommitter *pCommith) {
  pCommith->pDataCols = tdFreeDataCols(pCommith->pDataCols);
  pCommith->aSubBlk = taosArrayDestroy(pCommith->aSubBlk);
  pCommith->aSupBlk = taosArrayDestroy(pCommith->aSupBlk);
  pCommith->aBlkIdx = taosArrayDestroy(pCommith->aBlkIdx);
  tsdbDestroyCommitIters(pCommith);
  tsdbDestroyReadH(&(pCommith->readh));
  tsdbCloseDFileSet(TSDB_COMMIT_WRITE_FSET(pCommith));
}

static int32_t tsdbCommitToFileStart(SCommitter *pCHandle, SDFileSet *pSet, int32_t fid) {
  int32_t       code = 0;
  STsdb        *pRepo = TSDB_COMMIT_REPO(pCHandle);
  STsdbKeepCfg *pCfg = REPO_KEEP_CFG(pRepo);

  ASSERT(pSet == NULL || pSet->fid == fid);

  pCHandle->fid = fid;
  pCHandle->pSet = pSet;
  pCHandle->isRFileSet = false;
  pCHandle->isDFileSame = false;
  pCHandle->isLFileSame = false;
  taosArrayClear(pCHandle->aBlkIdx);

  tsdbGetFidKeyRange(pCfg->days, pCfg->precision, fid, &(pCHandle->minKey), &(pCHandle->maxKey));

  code = tsdbSetAndOpenCommitFile(pCHandle, pSet, fid);

  return code;
}
static int32_t tsdbCommitToFileImpl(SCommitter *pCHandle) {
  int32_t code = 0;
  // TODO
  return code;
}
static int32_t tsdbCommitToFileEnd(SCommitter *pCommith) {
  int32_t code = 0;
  STsdb  *pRepo = TSDB_COMMIT_REPO(pCommith);

  if (tsdbWriteBlockIdx(TSDB_COMMIT_HEAD_FILE(pCommith), pCommith->aBlkIdx, (void **)(&(TSDB_COMMIT_BUF(pCommith))))
  <
      0) {
    tsdbError("vgId:%d, failed to write SBlockIdx part to FSET %d since %s", REPO_ID(pRepo), pCommith->fid,
              tstrerror(terrno));
    tsdbCloseCommitFile(pCommith, true);
    // revert the file change
    tsdbApplyDFileSetChange(TSDB_COMMIT_WRITE_FSET(pCommith), pCommith->pSet);
    return -1;
  }

  if (tsdbUpdateDFileSetHeader(&(pCommith->wSet)) < 0) {
    tsdbError("vgId:%d, failed to update FSET %d header since %s", REPO_ID(pRepo), pCommith->fid, tstrerror(terrno));
    tsdbCloseCommitFile(pCommith, true);
    // revert the file change
    tsdbApplyDFileSetChange(TSDB_COMMIT_WRITE_FSET(pCommith), pCommith->pSet);
    return -1;
  }

  // Close commit file
  tsdbCloseCommitFile(pCommith, false);

  if (tsdbUpdateDFileSet(REPO_FS(pRepo), &(pCommith->wSet)) < 0) {
    return -1;
  }

  return code;
}
static int32_t tsdbCommitToFile(SCommitter *pCommith, SDFileSet *pSet, int fid) {
  int32_t       code = 0;
  STsdb        *pRepo = TSDB_COMMIT_REPO(pCommith);
  STsdbKeepCfg *pCfg = REPO_KEEP_CFG(pRepo);

  // commit to file start
  code = tsdbCommitToFileStart(pCommith, pSet, fid);
  if (code) {
    goto _err;
  }

  // Loop to commit each table data in mem and file
  int mIter = 0, fIter = 0;
  int nBlkIdx = taosArrayGetSize(pCommith->readh.aBlkIdx);

  while (true) {
    SBlockIdx   *pIdx = NULL;
    SCommitIter *pIter = NULL;
    if (mIter < pCommith->niters) {
      pIter = pCommith->iters + mIter;
      if (fIter < nBlkIdx) {
        pIdx = taosArrayGet(pCommith->readh.aBlkIdx, fIter);
      }
    } else if (fIter < nBlkIdx) {
      pIdx = taosArrayGet(pCommith->readh.aBlkIdx, fIter);
    } else {
      break;
    }

    if (pIter && pIter->pTable && (!pIdx || (pIter->pTable->suid <= pIdx->suid || pIter->pTable->uid <= pIdx->uid)))
    {
      if (tsdbCommitToTable(pCommith, mIter) < 0) {
        tsdbCloseCommitFile(pCommith, true);
        // revert the file change
        tsdbApplyDFileSetChange(TSDB_COMMIT_WRITE_FSET(pCommith), pSet);
        return -1;
      }

      if (pIdx && (pIter->pTable->uid == pIdx->uid)) {
        ++fIter;
      }
      ++mIter;
    } else if (pIter && !pIter->pTable) {
      // When table already dropped during commit, pIter is not NULL but pIter->pTable is NULL.
      ++mIter;  // skip the table and do nothing
    } else if (pIdx) {
      if (tsdbMoveBlkIdx(pCommith, pIdx) < 0) {
        tsdbCloseCommitFile(pCommith, true);
        // revert the file change
        tsdbApplyDFileSetChange(TSDB_COMMIT_WRITE_FSET(pCommith), pSet);
        return -1;
      }
      ++fIter;
    }
  }

  // commit to file end
  code = tsdbCommitToFileEnd(pCommith);
  if (code) {
    goto _err;
  }

  return code;

_err:
  return code;
}

static int32_t tsdbCreateCommitIters(SCommitter *pCommith) {
  int32_t      code = 0;
  STsdb       *pRepo = TSDB_COMMIT_REPO(pCommith);
  SMemTable   *pMem = pRepo->imem;
  STbData     *pTbData;
  SCommitIter *pCommitIter;
  STSchema    *pTSchema = NULL;

  pCommith->niters = taosArrayGetSize(pMem->aTbData);
  pCommith->iters = (SCommitIter *)taosMemoryCalloc(pCommith->niters, sizeof(SCommitIter));
  if (pCommith->iters == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

  for (int32_t iIter = 0; iIter < pCommith->niters; iIter++) {
    pTbData = (STbData *)taosArrayGetP(pMem->aTbData, iIter);
    pCommitIter = &pCommith->iters[iIter];

    pTSchema = metaGetTbTSchema(REPO_META(pRepo), pTbData->uid, -1);
    if (pTSchema) {
      tsdbTbDataIterCreate(pTbData, NULL, 0, &pCommitIter->pIter);

      pCommitIter->pTable = (STable *)taosMemoryMalloc(sizeof(STable));
      pCommitIter->pTable->uid = pTbData->uid;
      pCommitIter->pTable->suid = pTbData->suid;
      pCommitIter->pTable->pSchema = pTSchema;
      pCommitIter->pTable->pCacheSchema = NULL;
    }
  }

  return code;

_err:
  return code;
}

static void tsdbDestroyCommitIters(SCommitter *pCommith) {
  if (pCommith->iters == NULL) return;

  for (int i = 1; i < pCommith->niters; i++) {
    tsdbTbDataIterDestroy(pCommith->iters[i].pIter);
    if (pCommith->iters[i].pTable) {
      tdFreeSchema(pCommith->iters[i].pTable->pSchema);
      tdFreeSchema(pCommith->iters[i].pTable->pCacheSchema);
      taosMemoryFreeClear(pCommith->iters[i].pTable);
    }
  }

  taosMemoryFree(pCommith->iters);
  pCommith->iters = NULL;
  pCommith->niters = 0;
}

static int tsdbSetAndOpenCommitFile(SCommitter *pCommith, SDFileSet *pSet, int fid) {
  SDiskID    did;
  STsdb     *pRepo = TSDB_COMMIT_REPO(pCommith);
  SDFileSet *pWSet = TSDB_COMMIT_WRITE_FSET(pCommith);

  if (tfsAllocDisk(REPO_TFS(pRepo), tsdbGetFidLevel(fid, &(pCommith->rtn)), &did) < 0) {
    terrno = TSDB_CODE_TDB_NO_AVAIL_DISK;
    return -1;
  }

  // Open read FSET
  if (pSet) {
    if (tsdbSetAndOpenReadFSet(&(pCommith->readh), pSet) < 0) {
      return -1;
    }

    pCommith->isRFileSet = true;

    if (tsdbLoadBlockIdx(&(pCommith->readh)) < 0) {
      tsdbCloseAndUnsetFSet(&(pCommith->readh));
      return -1;
    }

    tsdbDebug("vgId:%d, FSET %d at level %d disk id %d is opened to read to commit", REPO_ID(pRepo),
              TSDB_FSET_FID(pSet), TSDB_FSET_LEVEL(pSet), TSDB_FSET_ID(pSet));
  } else {
    pCommith->isRFileSet = false;
  }

  // Set and open commit FSET
  if (pSet == NULL || did.level > TSDB_FSET_LEVEL(pSet)) {
    // Create a new FSET to write data
    tsdbInitDFileSet(pRepo, pWSet, did, fid, FS_TXN_VERSION(REPO_FS(pRepo)));

    if (tsdbCreateDFileSet(pRepo, pWSet, true) < 0) {
      tsdbError("vgId:%d, failed to create FSET %d at level %d disk id %d since %s", REPO_ID(pRepo),
                TSDB_FSET_FID(pWSet), TSDB_FSET_LEVEL(pWSet), TSDB_FSET_ID(pWSet), tstrerror(terrno));
      if (pCommith->isRFileSet) {
        tsdbCloseAndUnsetFSet(&(pCommith->readh));
      }
      return -1;
    }

    pCommith->isDFileSame = false;
    pCommith->isLFileSame = false;

    tsdbDebug("vgId:%d, FSET %d at level %d disk id %d is created to commit", REPO_ID(pRepo), TSDB_FSET_FID(pWSet),
              TSDB_FSET_LEVEL(pWSet), TSDB_FSET_ID(pWSet));
  } else {
    did.level = TSDB_FSET_LEVEL(pSet);
    did.id = TSDB_FSET_ID(pSet);

    pCommith->wSet.fid = fid;
    pCommith->wSet.state = 0;

    // TSDB_FILE_HEAD
    SDFile *pWHeadf = TSDB_COMMIT_HEAD_FILE(pCommith);
    tsdbInitDFile(pRepo, pWHeadf, did, fid, FS_TXN_VERSION(REPO_FS(pRepo)), TSDB_FILE_HEAD);
    if (tsdbCreateDFile(pRepo, pWHeadf, true, TSDB_FILE_HEAD) < 0) {
      tsdbError("vgId:%d, failed to create file %s to commit since %s", REPO_ID(pRepo), TSDB_FILE_FULL_NAME(pWHeadf),
                tstrerror(terrno));

      if (pCommith->isRFileSet) {
        tsdbCloseAndUnsetFSet(&(pCommith->readh));
        return -1;
      }
    }

    // TSDB_FILE_DATA
    SDFile *pRDataf = TSDB_READ_DATA_FILE(&(pCommith->readh));
    SDFile *pWDataf = TSDB_COMMIT_DATA_FILE(pCommith);
    tsdbInitDFileEx(pWDataf, pRDataf);
    // if (tsdbOpenDFile(pWDataf, O_WRONLY) < 0) {
    if (tsdbOpenDFile(pWDataf, TD_FILE_WRITE) < 0) {
      tsdbError("vgId:%d, failed to open file %s to commit since %s", REPO_ID(pRepo), TSDB_FILE_FULL_NAME(pWDataf),
                tstrerror(terrno));

      tsdbCloseDFileSet(pWSet);
      tsdbRemoveDFile(pWHeadf);
      if (pCommith->isRFileSet) {
        tsdbCloseAndUnsetFSet(&(pCommith->readh));
        return -1;
      }
    }
    pCommith->isDFileSame = true;

    // TSDB_FILE_LAST
    SDFile *pRLastf = TSDB_READ_LAST_FILE(&(pCommith->readh));
    SDFile *pWLastf = TSDB_COMMIT_LAST_FILE(pCommith);
    if (pRLastf->info.size < 32 * 1024) {
      tsdbInitDFileEx(pWLastf, pRLastf);
      pCommith->isLFileSame = true;

      // if (tsdbOpenDFile(pWLastf, O_WRONLY) < 0) {
      if (tsdbOpenDFile(pWLastf, TD_FILE_WRITE) < 0) {
        tsdbError("vgId:%d, failed to open file %s to commit since %s", REPO_ID(pRepo), TSDB_FILE_FULL_NAME(pWLastf),
                  tstrerror(terrno));

        tsdbCloseDFileSet(pWSet);
        tsdbRemoveDFile(pWHeadf);
        if (pCommith->isRFileSet) {
          tsdbCloseAndUnsetFSet(&(pCommith->readh));
          return -1;
        }
      }
    } else {
      tsdbInitDFile(pRepo, pWLastf, did, fid, FS_TXN_VERSION(REPO_FS(pRepo)), TSDB_FILE_LAST);
      pCommith->isLFileSame = false;

      if (tsdbCreateDFile(pRepo, pWLastf, true, TSDB_FILE_LAST) < 0) {
        tsdbError("vgId:%d, failed to create file %s to commit since %s", REPO_ID(pRepo),
        TSDB_FILE_FULL_NAME(pWLastf),
                  tstrerror(terrno));

        tsdbCloseDFileSet(pWSet);
        (void)tsdbRemoveDFile(pWHeadf);
        if (pCommith->isRFileSet) {
          tsdbCloseAndUnsetFSet(&(pCommith->readh));
          return -1;
        }
      }
    }

    // TSDB_FILE_SMAD
    SDFile *pRSmadF = TSDB_READ_SMAD_FILE(&(pCommith->readh));
    SDFile *pWSmadF = TSDB_COMMIT_SMAD_FILE(pCommith);

    if (!taosCheckExistFile(TSDB_FILE_FULL_NAME(pRSmadF))) {
      tsdbDebug("vgId:%d, create data file %s as not exist", REPO_ID(pRepo), TSDB_FILE_FULL_NAME(pRSmadF));
      tsdbInitDFile(pRepo, pWSmadF, did, fid, FS_TXN_VERSION(REPO_FS(pRepo)), TSDB_FILE_SMAD);

      if (tsdbCreateDFile(pRepo, pWSmadF, true, TSDB_FILE_SMAD) < 0) {
        tsdbError("vgId:%d, failed to create file %s to commit since %s", REPO_ID(pRepo),
        TSDB_FILE_FULL_NAME(pWSmadF),
                  tstrerror(terrno));

        tsdbCloseDFileSet(pWSet);
        (void)tsdbRemoveDFile(pWHeadf);
        if (pCommith->isRFileSet) {
          tsdbCloseAndUnsetFSet(&(pCommith->readh));
          return -1;
        }
      }
    } else {
      tsdbInitDFileEx(pWSmadF, pRSmadF);
      if (tsdbOpenDFile(pWSmadF, O_RDWR) < 0) {
        tsdbError("vgId:%d, failed to open file %s to commit since %s", REPO_ID(pRepo), TSDB_FILE_FULL_NAME(pWSmadF),
                  tstrerror(terrno));

        tsdbCloseDFileSet(pWSet);
        tsdbRemoveDFile(pWHeadf);
        if (pCommith->isRFileSet) {
          tsdbCloseAndUnsetFSet(&(pCommith->readh));
          return -1;
        }
      }
    }

    // TSDB_FILE_SMAL
    SDFile *pRSmalF = TSDB_READ_SMAL_FILE(&(pCommith->readh));
    SDFile *pWSmalF = TSDB_COMMIT_SMAL_FILE(pCommith);

    if ((pCommith->isLFileSame) && taosCheckExistFile(TSDB_FILE_FULL_NAME(pRSmalF))) {
      tsdbInitDFileEx(pWSmalF, pRSmalF);
      if (tsdbOpenDFile(pWSmalF, O_RDWR) < 0) {
        tsdbError("vgId:%d, failed to open file %s to commit since %s", REPO_ID(pRepo), TSDB_FILE_FULL_NAME(pWSmalF),
                  tstrerror(terrno));

        tsdbCloseDFileSet(pWSet);
        tsdbRemoveDFile(pWHeadf);
        if (pCommith->isRFileSet) {
          tsdbCloseAndUnsetFSet(&(pCommith->readh));
          return -1;
        }
      }
    } else {
      tsdbDebug("vgId:%d, create data file %s as not exist", REPO_ID(pRepo), TSDB_FILE_FULL_NAME(pRSmalF));
      tsdbInitDFile(pRepo, pWSmalF, did, fid, FS_TXN_VERSION(REPO_FS(pRepo)), TSDB_FILE_SMAL);

      if (tsdbCreateDFile(pRepo, pWSmalF, true, TSDB_FILE_SMAL) < 0) {
        tsdbError("vgId:%d, failed to create file %s to commit since %s", REPO_ID(pRepo),
        TSDB_FILE_FULL_NAME(pWSmalF),
                  tstrerror(terrno));

        tsdbCloseDFileSet(pWSet);
        (void)tsdbRemoveDFile(pWHeadf);
        if (pCommith->isRFileSet) {
          tsdbCloseAndUnsetFSet(&(pCommith->readh));
          return -1;
        }
      }
    }
  }

  return 0;
}

// extern int32_t tsTsdbMetaCompactRatio;

static int tsdbWriteBlockInfoImpl(SDFile *pHeadf, STable *pTable, SArray *pSupA, SArray *pSubA, void **ppBuf,
                                  SBlockIdx *pIdx) {
  size_t      nSupBlocks;
  size_t      nSubBlocks;
  uint32_t    tlen;
  SBlockInfo *pBlkInfo;
  int64_t     offset;
  SBlock     *pBlock;

  memset(pIdx, 0, sizeof(*pIdx));

  nSupBlocks = taosArrayGetSize(pSupA);
  nSubBlocks = (pSubA == NULL) ? 0 : taosArrayGetSize(pSubA);

  if (nSupBlocks <= 0) {
    // No data (data all deleted)
    return 0;
  }

  tlen = (uint32_t)(sizeof(SBlockInfo) + sizeof(SBlock) * (nSupBlocks + nSubBlocks) + sizeof(TSCKSUM));
  if (tsdbMakeRoom(ppBuf, tlen) < 0) return -1;
  pBlkInfo = *ppBuf;

  pBlkInfo->delimiter = TSDB_FILE_DELIMITER;
  pBlkInfo->suid = pTable->suid;
  pBlkInfo->uid = pTable->uid;

  memcpy((void *)(pBlkInfo->blocks), taosArrayGet(pSupA, 0), nSupBlocks * sizeof(SBlock));
  if (nSubBlocks > 0) {
    memcpy((void *)(pBlkInfo->blocks + nSupBlocks), taosArrayGet(pSubA, 0), nSubBlocks * sizeof(SBlock));

    for (int i = 0; i < nSupBlocks; i++) {
      pBlock = pBlkInfo->blocks + i;

      if (pBlock->numOfSubBlocks > 1) {
        pBlock->offset += (sizeof(SBlockInfo) + sizeof(SBlock) * nSupBlocks);
      }
    }
  }

  taosCalcChecksumAppend(0, (uint8_t *)pBlkInfo, tlen);

  if (tsdbAppendDFile(pHeadf, (void *)pBlkInfo, tlen, &offset) < 0) {
    return -1;
  }

  tsdbUpdateDFileMagic(pHeadf, POINTER_SHIFT(pBlkInfo, tlen - sizeof(TSCKSUM)));

  // Set pIdx
  pBlock = taosArrayGetLast(pSupA);

  pIdx->suid = pTable->suid;
  pIdx->uid = pTable->uid;
  pIdx->hasLast = pBlock->last ? 1 : 0;
  pIdx->maxKey = pBlock->maxKey;
  pIdx->numOfBlocks = (uint32_t)nSupBlocks;
  pIdx->len = tlen;
  pIdx->offset = (uint32_t)offset;

  return 0;
}

static int tsdbWriteBlockIdx(SDFile *pHeadf, SArray *pIdxA, void **ppBuf) {
  SBlockIdx *pBlkIdx;
  size_t     nidx = taosArrayGetSize(pIdxA);
  int        tlen = 0, size;
  int64_t    offset;

  if (nidx <= 0) {
    // All data are deleted
    pHeadf->info.offset = 0;
    pHeadf->info.len = 0;
    return 0;
  }

  for (size_t i = 0; i < nidx; i++) {
    pBlkIdx = (SBlockIdx *)taosArrayGet(pIdxA, i);

    size = tsdbEncodeSBlockIdx(NULL, pBlkIdx);
    if (tsdbMakeRoom(ppBuf, tlen + size) < 0) return -1;

    void *ptr = POINTER_SHIFT(*ppBuf, tlen);
    tsdbEncodeSBlockIdx(&ptr, pBlkIdx);

    tlen += size;
  }

  tlen += sizeof(TSCKSUM);
  if (tsdbMakeRoom(ppBuf, tlen) < 0) return -1;
  taosCalcChecksumAppend(0, (uint8_t *)(*ppBuf), tlen);

  if (tsdbAppendDFile(pHeadf, *ppBuf, tlen, &offset) < tlen) {
    return -1;
  }

  tsdbUpdateDFileMagic(pHeadf, POINTER_SHIFT(*ppBuf, tlen - sizeof(TSCKSUM)));
  pHeadf->info.offset = (uint32_t)offset;
  pHeadf->info.len = tlen;

  return 0;
}

// =================== Commit Time-Series Data
static int tsdbCommitToTable(SCommitter *pCommith, int tid) {
  SCommitIter *pIter = pCommith->iters + tid;
  TSKEY        nextKey = tsdbNextIterKey(pIter->pIter);

  tsdbResetCommitTable(pCommith);

  // Set commit table
  if (tsdbSetCommitTable(pCommith, pIter->pTable) < 0) {
    return -1;
  }

  // No disk data and no memory data, just return
  if (pCommith->readh.pBlkIdx == NULL && (nextKey == TSDB_DATA_TIMESTAMP_NULL || nextKey > pCommith->maxKey)) {
    return 0;
  }

  // Must has disk data or has memory data
  int     nBlocks;
  int     bidx = 0;
  SBlock *pBlock;

  if (pCommith->readh.pBlkIdx) {
    if (tsdbLoadBlockInfo(&(pCommith->readh), NULL) < 0) {
      return -1;
    }

    nBlocks = pCommith->readh.pBlkIdx->numOfBlocks;
  } else {
    nBlocks = 0;
  }

  if (bidx < nBlocks) {
    pBlock = pCommith->readh.pBlkInfo->blocks + bidx;
  } else {
    pBlock = NULL;
  }

  while (true) {
    if (pBlock == NULL && (nextKey == TSDB_DATA_TIMESTAMP_NULL || nextKey > pCommith->maxKey)) break;

    if ((nextKey == TSDB_DATA_TIMESTAMP_NULL || nextKey > pCommith->maxKey) ||
        (pBlock && (!pBlock->last) && tsdbComparKeyBlock((void *)(&nextKey), pBlock) > 0)) {
      if (tsdbMoveBlock(pCommith, bidx) < 0) {
        return -1;
      }

      bidx++;
      if (bidx < nBlocks) {
        pBlock = pCommith->readh.pBlkInfo->blocks + bidx;
      } else {
        pBlock = NULL;
      }
    } else if (pBlock && (pBlock->last || tsdbComparKeyBlock((void *)(&nextKey), pBlock) == 0)) {
      // merge pBlock data and memory data
      if (tsdbMergeMemData(pCommith, pIter, bidx) < 0) {
        return -1;
      }

      bidx++;
      if (bidx < nBlocks) {
        pBlock = pCommith->readh.pBlkInfo->blocks + bidx;
      } else {
        pBlock = NULL;
      }
      nextKey = tsdbNextIterKey(pIter->pIter);
    } else {
      // Only commit memory data
      if (pBlock == NULL) {
        if (tsdbCommitMemData(pCommith, pIter, pCommith->maxKey, false) < 0) {
          return -1;
        }
      } else {
        if (tsdbCommitMemData(pCommith, pIter, pBlock->minKey.ts - 1, true) < 0) {
          return -1;
        }
      }
      nextKey = tsdbNextIterKey(pIter->pIter);
    }
  }

  if (tsdbWriteBlockInfo(pCommith) < 0) {
    tsdbError("vgId:%d, failed to write SBlockInfo part into file %s since %s", TSDB_COMMIT_REPO_ID(pCommith),
              TSDB_FILE_FULL_NAME(TSDB_COMMIT_HEAD_FILE(pCommith)), tstrerror(terrno));
    return -1;
  }

  return 0;
}

static int tsdbMoveBlkIdx(SCommitter *pCommith, SBlockIdx *pIdx) {
  SReadH   *pReadh = &pCommith->readh;
  STsdb    *pTsdb = TSDB_READ_REPO(pReadh);
  STSchema *pTSchema = NULL;
  int       nBlocks = pIdx->numOfBlocks;
  int       bidx = 0;

  tsdbResetCommitTable(pCommith);

  pReadh->pBlkIdx = pIdx;

  if (tsdbLoadBlockInfo(pReadh, NULL) < 0) {
    return -1;
  }

  STable table = {.suid = pIdx->suid, .uid = pIdx->uid, .pSchema = NULL};
  pCommith->pTable = &table;

  while (bidx < nBlocks) {
    if (!pTSchema && !tsdbCommitIsSameFile(pCommith, bidx)) {
      // Set commit table
      pTSchema = metaGetTbTSchema(REPO_META(pTsdb), pIdx->uid, -1);  // TODO: schema version
      if (!pTSchema) {
        terrno = TSDB_CODE_OUT_OF_MEMORY;
        return -1;
      }
      table.pSchema = pTSchema;
      if (tsdbSetCommitTable(pCommith, &table) < 0) {
        taosMemoryFreeClear(pTSchema);
        return -1;
      }
    }

    if (tsdbMoveBlock(pCommith, bidx) < 0) {
      tsdbError("vgId:%d, failed to move block into file %s since %s", TSDB_COMMIT_REPO_ID(pCommith),
                TSDB_FILE_FULL_NAME(TSDB_COMMIT_HEAD_FILE(pCommith)), tstrerror(terrno));
      taosMemoryFreeClear(pTSchema);
      return -1;
    }

    ++bidx;
  }

  if (tsdbWriteBlockInfo(pCommith) < 0) {
    tsdbError("vgId:%d, failed to write SBlockInfo part into file %s since %s", TSDB_COMMIT_REPO_ID(pCommith),
              TSDB_FILE_FULL_NAME(TSDB_COMMIT_HEAD_FILE(pCommith)), tstrerror(terrno));
    taosMemoryFreeClear(pTSchema);
    return -1;
  }

  taosMemoryFreeClear(pTSchema);
  return 0;
}

static int tsdbSetCommitTable(SCommitter *pCommith, STable *pTable) {
  STSchema *pSchema = tsdbGetTableSchemaImpl(TSDB_COMMIT_REPO(pCommith), pTable, false, false, -1);

  pCommith->pTable = pTable;

  if (tdInitDataCols(pCommith->pDataCols, pSchema) < 0) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return -1;
  }

  if (pCommith->isRFileSet) {
    if (tsdbSetReadTable(&(pCommith->readh), pTable) < 0) {
      return -1;
    }
  } else {
    pCommith->readh.pBlkIdx = NULL;
  }
  return 0;
}

static int tsdbComparKeyBlock(const void *arg1, const void *arg2) {
  TSKEY   key = *(TSKEY *)arg1;
  SBlock *pBlock = (SBlock *)arg2;

  if (key < pBlock->minKey.ts) {
    return -1;
  } else if (key > pBlock->maxKey.ts) {
    return 1;
  } else {
    return 0;
  }
}

/**
 * @brief Write SDataCols to data file.
 *
 * @param pRepo
 * @param pTable
 * @param pDFile
 * @param pDFileAggr
 * @param pDataCols The pDataCols would be generated from mem/imem directly with 2 bits bitmap or from tsdbRead
 * interface with 1 bit bitmap.
 * @param pBlock
 * @param isLast
 * @param isSuper
 * @param ppBuf
 * @param ppCBuf
 * @param ppExBuf
 * @return int
 */
static int tsdbWriteBlockImpl(STsdb *pRepo, STable *pTable, SDFile *pDFile, SDFile *pDFileAggr, SDataCols *pDataCols,
                              SBlock *pBlock, bool isLast, bool isSuper, void **ppBuf, void **ppCBuf, void **ppExBuf)
                              {
  STsdbCfg     *pCfg = REPO_CFG(pRepo);
  SBlockData   *pBlockData = NULL;
  SAggrBlkData *pAggrBlkData = NULL;
  STSchema     *pSchema = pTable->pSchema;
  int64_t       offset = 0, offsetAggr = 0;
  int           rowsToWrite = pDataCols->numOfRows;

  ASSERT(rowsToWrite > 0 && rowsToWrite <= pCfg->maxRows);
  ASSERT((!isLast) || rowsToWrite < pCfg->minRows);

  // Make buffer space
  if (tsdbMakeRoom(ppBuf, tsdbBlockStatisSize(pDataCols->numOfCols, SBlockVerLatest)) < 0) {
    return -1;
  }
  pBlockData = (SBlockData *)(*ppBuf);

  if (tsdbMakeRoom(ppExBuf, tsdbBlockAggrSize(pDataCols->numOfCols, SBlockVerLatest)) < 0) {
    return -1;
  }
  pAggrBlkData = (SAggrBlkData *)(*ppExBuf);

  // Get # of cols not all NULL(not including key column)
  col_id_t nColsNotAllNull = 0;
  col_id_t nColsOfBlockSma = 0;
  for (int ncol = 1; ncol < pDataCols->numOfCols; ++ncol) {  // ncol from 1, we skip the timestamp column
    STColumn    *pColumn = pSchema->columns + ncol;
    SDataCol    *pDataCol = pDataCols->cols + ncol;
    SBlockCol   *pBlockCol = pBlockData->cols + nColsNotAllNull;
    SAggrBlkCol *pAggrBlkCol = (SAggrBlkCol *)pAggrBlkData + nColsOfBlockSma;

    if (isAllRowsNull(pDataCol)) {  // all data to commit are NULL, just ignore it
      continue;
    }

    memset(pBlockCol, 0, sizeof(*pBlockCol));
    memset(pAggrBlkCol, 0, sizeof(*pAggrBlkCol));

    pBlockCol->colId = pDataCol->colId;
    pBlockCol->type = pDataCol->type;
    pAggrBlkCol->colId = pDataCol->colId;

    if (isSuper && IS_BSMA_ON(pColumn) && tDataTypes[pDataCol->type].statisFunc) {
#if 0
      (*tDataTypes[pDataCol->type].statisFunc)(pDataCol->pData, rowsToWrite, &(pBlockCol->min), &(pBlockCol->max),
                                               &(pBlockCol->sum), &(pBlockCol->minIndex), &(pBlockCol->maxIndex),
                                               &(pBlockCol->numOfNull));
#endif
      (*tDataTypes[pDataCol->type].statisFunc)(pDataCols->bitmapMode, pDataCol->pBitmap, pDataCol->pData,
      rowsToWrite,
                                               &(pAggrBlkCol->min), &(pAggrBlkCol->max), &(pAggrBlkCol->sum),
                                               &(pAggrBlkCol->minIndex), &(pAggrBlkCol->maxIndex),
                                               &(pAggrBlkCol->numOfNull));

      if (pAggrBlkCol->numOfNull == 0) {
        pBlockCol->blen = 0;
      } else {
        pBlockCol->blen = 1;
      }
      ++nColsOfBlockSma;
    } else if (tdIsBitmapBlkNorm(pDataCol->pBitmap, rowsToWrite, pDataCols->bitmapMode)) {
      // check if all rows normal
      pBlockCol->blen = 0;
    } else {
      pBlockCol->blen = 1;
    }

    ++nColsNotAllNull;
  }

  ASSERT(nColsNotAllNull >= 0 && nColsNotAllNull <= pDataCols->numOfCols);

  // Compress the data if neccessary
  int      tcol = 0;  // counter of not all NULL and written columns
  uint32_t toffset = 0;
  int32_t  tsize = (int32_t)tsdbBlockStatisSize(nColsNotAllNull, SBlockVerLatest);
  int32_t  lsize = tsize;
  uint32_t tsizeAggr = (uint32_t)tsdbBlockAggrSize(nColsOfBlockSma, SBlockVerLatest);
  int32_t  keyLen = 0;
  int32_t  nBitmaps = (int32_t)TD_BITMAP_BYTES(rowsToWrite);
  int32_t  sBitmaps = isSuper ? (int32_t)TD_BITMAP_BYTES_I(rowsToWrite) : nBitmaps;

  for (int ncol = 0; ncol < pDataCols->numOfCols; ++ncol) {
    // All not NULL columns finish
    if (ncol != 0 && tcol >= nColsNotAllNull) break;

    SDataCol  *pDataCol = pDataCols->cols + ncol;
    SBlockCol *pBlockCol = pBlockData->cols + tcol;

    if (ncol != 0 && (pDataCol->colId != pBlockCol->colId)) continue;

    int32_t flen;  // final length
    int32_t tlen = dataColGetNEleLen(pDataCol, rowsToWrite, pDataCols->bitmapMode);

#ifdef TD_SUPPORT_BITMAP
    int32_t tBitmaps = 0;
    int32_t tBitmapsLen = 0;
    if ((ncol != 0) && (pBlockCol->blen > 0)) {
      tBitmaps = isSuper ? sBitmaps : nBitmaps;
    }
#endif

    void *tptr, *bptr;

    // Make room
    if (tsdbMakeRoom(ppBuf, lsize + tlen + tBitmaps + 2 * COMP_OVERFLOW_BYTES + sizeof(TSCKSUM)) < 0) {
      return -1;
    }
    pBlockData = (SBlockData *)(*ppBuf);
    pBlockCol = pBlockData->cols + tcol;
    tptr = POINTER_SHIFT(pBlockData, lsize);

    if (pCfg->compression == TWO_STAGE_COMP && tsdbMakeRoom(ppCBuf, tlen + COMP_OVERFLOW_BYTES) < 0) {
      return -1;
    }

    // Compress or just copy
    if (pCfg->compression) {
#if 0
      flen = (*(tDataTypes[pDataCol->type].compFunc))((char *)pDataCol->pData, tlen, rowsToWrite + tBitmaps, tptr,
                                                      tlen + COMP_OVERFLOW_BYTES, pCfg->compression, *ppCBuf,
                                                      tlen + COMP_OVERFLOW_BYTES);
#endif
      flen = (*(tDataTypes[pDataCol->type].compFunc))((char *)pDataCol->pData, tlen, rowsToWrite, tptr,
                                                      tlen + COMP_OVERFLOW_BYTES, pCfg->compression, *ppCBuf,
                                                      tlen + COMP_OVERFLOW_BYTES);
      if (tBitmaps > 0) {
        bptr = POINTER_SHIFT(pBlockData, lsize + flen);
        if (isSuper && !tdDataColsIsBitmapI(pDataCols)) {
          tdMergeBitmap((uint8_t *)pDataCol->pBitmap, rowsToWrite, (uint8_t *)pDataCol->pBitmap);
        }
        tBitmapsLen =
            tsCompressTinyint((char *)pDataCol->pBitmap, tBitmaps, tBitmaps, bptr, tBitmaps + COMP_OVERFLOW_BYTES,
                              pCfg->compression, *ppCBuf, tBitmaps + COMP_OVERFLOW_BYTES);
        TASSERT((tBitmapsLen > 0) && (tBitmapsLen <= (tBitmaps + COMP_OVERFLOW_BYTES)));
        flen += tBitmapsLen;
      }
    } else {
      flen = tlen;
      memcpy(tptr, pDataCol->pData, flen);
      if (tBitmaps > 0) {
        bptr = POINTER_SHIFT(pBlockData, lsize + flen);
        if (isSuper && !tdDataColsIsBitmapI(pDataCols)) {
          tdMergeBitmap((uint8_t *)pDataCol->pBitmap, rowsToWrite, (uint8_t *)pDataCol->pBitmap);
        }
        memcpy(bptr, pDataCol->pBitmap, tBitmaps);
        tBitmapsLen = tBitmaps;
        flen += tBitmapsLen;
      }
    }

    // Add checksum
    ASSERT(flen > 0);
    ASSERT(tBitmapsLen <= 1024);
    flen += sizeof(TSCKSUM);
    taosCalcChecksumAppend(0, (uint8_t *)tptr, flen);
    tsdbUpdateDFileMagic(pDFile, POINTER_SHIFT(tptr, flen - sizeof(TSCKSUM)));

    if (ncol != 0) {
      pBlockCol->offset = toffset;
      pBlockCol->len = flen;  // data + bitmaps
      pBlockCol->blen = tBitmapsLen;
      ++tcol;
    } else {
      keyLen = flen;
    }

    toffset += flen;
    lsize += flen;
  }

  pBlockData->delimiter = TSDB_FILE_DELIMITER;
  pBlockData->uid = pTable->uid;
  pBlockData->numOfCols = nColsNotAllNull;

  taosCalcChecksumAppend(0, (uint8_t *)pBlockData, tsize);
  tsdbUpdateDFileMagic(pDFile, POINTER_SHIFT(pBlockData, tsize - sizeof(TSCKSUM)));

  // Write the whole block to file
  if (tsdbAppendDFile(pDFile, (void *)pBlockData, lsize, &offset) < lsize) {
    return -1;
  }

  uint32_t aggrStatus = nColsOfBlockSma > 0 ? 1 : 0;
  if (aggrStatus > 0) {
    taosCalcChecksumAppend(0, (uint8_t *)pAggrBlkData, tsizeAggr);
    tsdbUpdateDFileMagic(pDFileAggr, POINTER_SHIFT(pAggrBlkData, tsizeAggr - sizeof(TSCKSUM)));

    // Write the whole block to file
    if (tsdbAppendDFile(pDFileAggr, (void *)pAggrBlkData, tsizeAggr, &offsetAggr) < tsizeAggr) {
      return -1;
    }
  }

  // Update pBlock membership variables
  pBlock->last = isLast;
  pBlock->offset = offset;
  pBlock->algorithm = pCfg->compression;
  pBlock->numOfRows = rowsToWrite;
  pBlock->len = lsize;
  pBlock->keyLen = keyLen;
  pBlock->numOfSubBlocks = isSuper ? 1 : 0;
  pBlock->numOfCols = nColsNotAllNull;
  pBlock->numOfBSma = nColsOfBlockSma;
  pBlock->minKey.ts = dataColsKeyFirst(pDataCols);
  pBlock->maxKey.ts = dataColsKeyLast(pDataCols);
  pBlock->aggrStat = aggrStatus;
  pBlock->blkVer = SBlockVerLatest;
  pBlock->aggrOffset = (uint64_t)offsetAggr;

  tsdbDebug("vgId:%d, uid:%" PRId64 " a block of data is written to file %s, offset %" PRId64
            " numOfRows %d len %d numOfCols %" PRId16 " keyFirst %" PRId64 " keyLast %" PRId64,
            REPO_ID(pRepo), pTable->uid, TSDB_FILE_FULL_NAME(pDFile), offset, rowsToWrite, pBlock->len,
            pBlock->numOfCols, pBlock->minKey.ts, pBlock->maxKey.ts);

  return 0;
}

static int tsdbWriteBlock(SCommitter *pCommith, SDFile *pDFile, SDataCols *pDataCols, SBlock *pBlock, bool isLast,
                          bool isSuper) {
  return tsdbWriteBlockImpl(TSDB_COMMIT_REPO(pCommith), TSDB_COMMIT_TABLE(pCommith), pDFile,
                            isLast ? TSDB_COMMIT_SMAL_FILE(pCommith) : TSDB_COMMIT_SMAD_FILE(pCommith), pDataCols,
                            pBlock, isLast, isSuper, (void **)(&(TSDB_COMMIT_BUF(pCommith))),
                            (void **)(&(TSDB_COMMIT_COMP_BUF(pCommith))), (void **)(&(TSDB_COMMIT_EXBUF(pCommith))));
}

static int tsdbWriteBlockInfo(SCommitter *pCommih) {
  SDFile   *pHeadf = TSDB_COMMIT_HEAD_FILE(pCommih);
  SBlockIdx blkIdx;
  STable   *pTable = TSDB_COMMIT_TABLE(pCommih);

  if (tsdbWriteBlockInfoImpl(pHeadf, pTable, pCommih->aSupBlk, pCommih->aSubBlk, (void
  **)(&(TSDB_COMMIT_BUF(pCommih))),
                             &blkIdx) < 0) {
    return -1;
  }

  if (blkIdx.numOfBlocks == 0) {
    return 0;
  }

  if (taosArrayPush(pCommih->aBlkIdx, (void *)(&blkIdx)) == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return -1;
  }

  return 0;
}

static int tsdbCommitMemData(SCommitter *pCommith, SCommitIter *pIter, TSKEY keyLimit, bool toData) {
  STsdb     *pRepo = TSDB_COMMIT_REPO(pCommith);
  STsdbCfg  *pCfg = REPO_CFG(pRepo);
  SMergeInfo mInfo;
  int32_t    defaultRows = TSDB_COMMIT_DEFAULT_ROWS(pCommith);
  SDFile    *pDFile;
  bool       isLast;
  SBlock     block;

  while (true) {
    tsdbLoadDataFromCache(TSDB_COMMIT_REPO(pCommith), pIter->pTable, pIter->pIter, keyLimit, defaultRows,
                          pCommith->pDataCols, NULL, 0, pCfg->update, &mInfo);

    if (pCommith->pDataCols->numOfRows <= 0) break;

    if (toData || pCommith->pDataCols->numOfRows >= pCfg->minRows) {
      pDFile = TSDB_COMMIT_DATA_FILE(pCommith);
      isLast = false;
    } else {
      pDFile = TSDB_COMMIT_LAST_FILE(pCommith);
      isLast = true;
    }

    if (tsdbWriteBlock(pCommith, pDFile, pCommith->pDataCols, &block, isLast, true) < 0) return -1;

    if (tsdbCommitAddBlock(pCommith, &block, NULL, 0) < 0) {
      return -1;
    }
  }

  return 0;
}

static int tsdbMergeMemData(SCommitter *pCommith, SCommitIter *pIter, int bidx) {
  STsdb     *pRepo = TSDB_COMMIT_REPO(pCommith);
  STsdbCfg  *pCfg = REPO_CFG(pRepo);
  int        nBlocks = pCommith->readh.pBlkIdx->numOfBlocks;
  SBlock    *pBlock = pCommith->readh.pBlkInfo->blocks + bidx;
  TSKEY      keyLimit;
  int16_t    colId = PRIMARYKEY_TIMESTAMP_COL_ID;
  SMergeInfo mInfo;
  SBlock     subBlocks[TSDB_MAX_SUBBLOCKS];
  SBlock     block, supBlock;
  SDFile    *pDFile;

  if (bidx == nBlocks - 1) {
    keyLimit = pCommith->maxKey;
  } else {
    keyLimit = pBlock[1].minKey.ts - 1;
  }

  STbDataIter titer = *(pIter->pIter);
  if (tsdbLoadBlockDataCols(&(pCommith->readh), pBlock, NULL, &colId, 1, false) < 0) return -1;

  tsdbLoadDataFromCache(TSDB_COMMIT_REPO(pCommith), pIter->pTable, &titer, keyLimit, INT32_MAX, NULL,
                        pCommith->readh.pDCols[0]->cols[0].pData, pCommith->readh.pDCols[0]->numOfRows, pCfg->update,
                        &mInfo);

  if (mInfo.nOperations == 0) {
    // no new data to insert (all updates denied)
    if (tsdbMoveBlock(pCommith, bidx) < 0) {
      return -1;
    }
    *(pIter->pIter) = titer;
  } else if (pBlock->numOfRows + mInfo.rowsInserted - mInfo.rowsDeleteSucceed == 0) {
    // Ignore the block
    ASSERT(0);
    *(pIter->pIter) = titer;
  } else if (tsdbCanAddSubBlock(pCommith, pBlock, &mInfo)) {
    // Add a sub-block
    tsdbLoadDataFromCache(TSDB_COMMIT_REPO(pCommith), pIter->pTable, pIter->pIter, keyLimit, INT32_MAX,
                          pCommith->pDataCols, pCommith->readh.pDCols[0]->cols[0].pData,
                          pCommith->readh.pDCols[0]->numOfRows, pCfg->update, &mInfo);
    if (pBlock->last) {
      pDFile = TSDB_COMMIT_LAST_FILE(pCommith);
    } else {
      pDFile = TSDB_COMMIT_DATA_FILE(pCommith);
    }

    if (tsdbWriteBlock(pCommith, pDFile, pCommith->pDataCols, &block, pBlock->last, false) < 0) return -1;

    if (pBlock->numOfSubBlocks == 1) {
      subBlocks[0] = *pBlock;
      subBlocks[0].numOfSubBlocks = 0;
    } else {
      memcpy(subBlocks, POINTER_SHIFT(pCommith->readh.pBlkInfo, pBlock->offset),
             sizeof(SBlock) * pBlock->numOfSubBlocks);
    }
    subBlocks[pBlock->numOfSubBlocks] = block;
    supBlock = *pBlock;
    supBlock.minKey.ts = mInfo.keyFirst;
    supBlock.maxKey.ts = mInfo.keyLast;
    supBlock.numOfSubBlocks++;
    supBlock.numOfRows = pBlock->numOfRows + mInfo.rowsInserted - mInfo.rowsDeleteSucceed;
    supBlock.offset = taosArrayGetSize(pCommith->aSubBlk) * sizeof(SBlock);

    if (tsdbCommitAddBlock(pCommith, &supBlock, subBlocks, supBlock.numOfSubBlocks) < 0) return -1;
  } else {
    if (tsdbLoadBlockData(&(pCommith->readh), pBlock, NULL) < 0) return -1;
    if (tsdbMergeBlockData(pCommith, pIter, pCommith->readh.pDCols[0], keyLimit, bidx == (nBlocks - 1)) < 0) return
    -1;
  }

  return 0;
}

static bool tsdbCommitIsSameFile(SCommitter *pCommith, int bidx) {
  SBlock *pBlock = pCommith->readh.pBlkInfo->blocks + bidx;
  if (pBlock->last) {
    return pCommith->isLFileSame;
  }
  return pCommith->isDFileSame;
}

static int tsdbMoveBlock(SCommitter *pCommith, int bidx) {
  SBlock *pBlock = pCommith->readh.pBlkInfo->blocks + bidx;
  SDFile *pDFile;
  SBlock  block;
  bool    isSameFile;

  ASSERT(pBlock->numOfSubBlocks > 0);

  if (pBlock->last) {
    pDFile = TSDB_COMMIT_LAST_FILE(pCommith);
    isSameFile = pCommith->isLFileSame;
  } else {
    pDFile = TSDB_COMMIT_DATA_FILE(pCommith);
    isSameFile = pCommith->isDFileSame;
  }

  if (isSameFile) {
    if (pBlock->numOfSubBlocks == 1) {
      if (tsdbCommitAddBlock(pCommith, pBlock, NULL, 0) < 0) {
        return -1;
      }
    } else {
      block = *pBlock;
      block.offset = sizeof(SBlock) * taosArrayGetSize(pCommith->aSubBlk);

      if (tsdbCommitAddBlock(pCommith, &block, POINTER_SHIFT(pCommith->readh.pBlkInfo, pBlock->offset),
                             pBlock->numOfSubBlocks) < 0) {
        return -1;
      }
    }
  } else {
    if (tsdbLoadBlockData(&(pCommith->readh), pBlock, NULL) < 0) return -1;
    if (tsdbWriteBlock(pCommith, pDFile, pCommith->readh.pDCols[0], &block, pBlock->last, true) < 0) return -1;
    if (tsdbCommitAddBlock(pCommith, &block, NULL, 0) < 0) return -1;
  }

  return 0;
}

static int tsdbCommitAddBlock(SCommitter *pCommith, const SBlock *pSupBlock, const SBlock *pSubBlocks, int
nSubBlocks) {
  if (taosArrayPush(pCommith->aSupBlk, pSupBlock) == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return -1;
  }

  if (pSubBlocks && taosArrayAddBatch(pCommith->aSubBlk, pSubBlocks, nSubBlocks) == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return -1;
  }

  return 0;
}

static int tsdbMergeBlockData(SCommitter *pCommith, SCommitIter *pIter, SDataCols *pDataCols, TSKEY keyLimit,
                              bool isLastOneBlock) {
  STsdb    *pRepo = TSDB_COMMIT_REPO(pCommith);
  STsdbCfg *pCfg = REPO_CFG(pRepo);
  SBlock    block;
  SDFile   *pDFile;
  bool      isLast;
  int32_t   defaultRows = TSDB_COMMIT_DEFAULT_ROWS(pCommith);

  int biter = 0;
  while (true) {
    tsdbLoadAndMergeFromCache(TSDB_COMMIT_REPO(pCommith), pCommith->readh.pDCols[0], &biter, pIter,
    pCommith->pDataCols,
                              keyLimit, defaultRows, pCfg->update);

    if (pCommith->pDataCols->numOfRows == 0) break;

    if (isLastOneBlock) {
      if (pCommith->pDataCols->numOfRows < pCfg->minRows) {
        pDFile = TSDB_COMMIT_LAST_FILE(pCommith);
        isLast = true;
      } else {
        pDFile = TSDB_COMMIT_DATA_FILE(pCommith);
        isLast = false;
      }
    } else {
      pDFile = TSDB_COMMIT_DATA_FILE(pCommith);
      isLast = false;
    }

    if (tsdbWriteBlock(pCommith, pDFile, pCommith->pDataCols, &block, isLast, true) < 0) return -1;
    if (tsdbCommitAddBlock(pCommith, &block, NULL, 0) < 0) return -1;
  }

  return 0;
}

static void tsdbLoadAndMergeFromCache(STsdb *pTsdb, SDataCols *pDataCols, int *iter, SCommitIter *pCommitIter,
                                      SDataCols *pTarget, TSKEY maxKey, int maxRows, int8_t update) {
  TSKEY     key1 = INT64_MAX;
  TSKEY     key2 = INT64_MAX;
  TSKEY     lastKey = TSKEY_INITIAL_VAL;
  STSchema *pSchema = NULL;

  ASSERT(maxRows > 0 && dataColsKeyLast(pDataCols) <= maxKey);
  tdResetDataCols(pTarget);

  pTarget->bitmapMode = pDataCols->bitmapMode;
  // TODO: filter Multi-Version
  // TODO: support delete function
  while (true) {
    key1 = (*iter >= pDataCols->numOfRows) ? INT64_MAX : dataColsKeyAt(pDataCols, *iter);
    STSRow *row = tsdbNextIterRow(pCommitIter->pIter);
    if (row == NULL || TD_ROW_KEY(row) > maxKey) {
      key2 = INT64_MAX;
    } else {
      key2 = TD_ROW_KEY(row);
    }

    if (key1 == INT64_MAX && key2 == INT64_MAX) break;

    if (key1 < key2) {
      if (lastKey != TSKEY_INITIAL_VAL) {
        ++pTarget->numOfRows;
      }
      for (int i = 0; i < pDataCols->numOfCols; ++i) {
        // TODO: dataColAppendVal may fail
        SCellVal sVal = {0};
        if (tdGetColDataOfRow(&sVal, pDataCols->cols + i, *iter, pDataCols->bitmapMode) < 0) {
          TASSERT(0);
        }
        tdAppendValToDataCol(pTarget->cols + i, sVal.valType, sVal.val, pTarget->numOfRows, pTarget->maxPoints,
                             pTarget->bitmapMode, false);
      }

      lastKey = key1;
      ++(*iter);
    } else if (key1 > key2) {
      if (pSchema == NULL || schemaVersion(pSchema) != TD_ROW_SVER(row)) {
        pSchema = tsdbGetTableSchemaImpl(pTsdb, pCommitIter->pTable, false, false, TD_ROW_SVER(row));
        ASSERT(pSchema != NULL);
      }

      if (key2 == lastKey) {
        if (TD_SUPPORT_UPDATE(update)) {
          tdAppendSTSRowToDataCol(row, pSchema, pTarget, true);
        }
      } else {
        if (lastKey != TSKEY_INITIAL_VAL) {
          ++pTarget->numOfRows;
        }
        tdAppendSTSRowToDataCol(row, pSchema, pTarget, false);
        lastKey = key2;
      }

      tsdbTbDataIterNext(pCommitIter->pIter);
    } else {
      if (lastKey != key1) {
        if (lastKey != TSKEY_INITIAL_VAL) {
          ++pTarget->numOfRows;
        }
        lastKey = key1;
      }

      // copy disk data
      for (int i = 0; i < pDataCols->numOfCols; ++i) {
        SCellVal sVal = {0};
        // no duplicated TS keys in pDataCols from file
        if (tdGetColDataOfRow(&sVal, pDataCols->cols + i, *iter, pDataCols->bitmapMode) < 0) {
          TASSERT(0);
        }
        // TODO: tdAppendValToDataCol may fail
        tdAppendValToDataCol(pTarget->cols + i, sVal.valType, sVal.val, pTarget->numOfRows, pTarget->maxPoints,
                             pTarget->bitmapMode, false);
      }

      if (TD_SUPPORT_UPDATE(update)) {
        // copy mem data(Multi-Version)
        if (pSchema == NULL || schemaVersion(pSchema) != TD_ROW_SVER(row)) {
          pSchema = tsdbGetTableSchemaImpl(pTsdb, pCommitIter->pTable, false, false, TD_ROW_SVER(row));
          ASSERT(pSchema != NULL);
        }

        // TODO: merge with Multi-Version
        tdAppendSTSRowToDataCol(row, pSchema, pTarget, true);
      }
      ++(*iter);
      tsdbTbDataIterNext(pCommitIter->pIter);
    }

    if (pTarget->numOfRows >= (maxRows - 1)) break;
  }

  if (lastKey != TSKEY_INITIAL_VAL) {
    ++pTarget->numOfRows;
  }
}

static void tsdbResetCommitTable(SCommitter *pCommith) {
  taosArrayClear(pCommith->aSubBlk);
  taosArrayClear(pCommith->aSupBlk);
  pCommith->pTable = NULL;
}

static void tsdbCloseCommitFile(SCommitter *pCommith, bool hasError) {
  if (pCommith->isRFileSet) {
    tsdbCloseAndUnsetFSet(&(pCommith->readh));
  }

  if (!hasError) {
    TSDB_FSET_FSYNC(TSDB_COMMIT_WRITE_FSET(pCommith));
  }
  tsdbCloseDFileSet(TSDB_COMMIT_WRITE_FSET(pCommith));
}

static bool tsdbCanAddSubBlock(SCommitter *pCommith, SBlock *pBlock, SMergeInfo *pInfo) {
  STsdb    *pRepo = TSDB_COMMIT_REPO(pCommith);
  STsdbCfg *pCfg = REPO_CFG(pRepo);
  int       mergeRows = pBlock->numOfRows + pInfo->rowsInserted - pInfo->rowsDeleteSucceed;

  ASSERT(mergeRows > 0);

  if (pBlock->numOfSubBlocks < TSDB_MAX_SUBBLOCKS && pInfo->nOperations <= pCfg->maxRows) {
    if (pBlock->last) {
      if (pCommith->isLFileSame && mergeRows < pCfg->minRows) return true;
    } else {
      if (pCommith->isDFileSame && mergeRows <= pCfg->maxRows) return true;
    }
  }

  return false;
}

static int tsdbAppendTableRowToCols(STsdb *pTsdb, STable *pTable, SDataCols *pCols, STSchema **ppSchema, STSRow *row,
                                    bool merge) {
  if (pCols) {
    if (*ppSchema == NULL || schemaVersion(*ppSchema) != TD_ROW_SVER(row)) {
      *ppSchema = tsdbGetTableSchemaImpl(pTsdb, pTable, false, false, TD_ROW_SVER(row));
      if (*ppSchema == NULL) {
        ASSERT(false);
        return -1;
      }
    }

    tdAppendSTSRowToDataCol(row, *ppSchema, pCols, merge);
  }

  return 0;
}

static int tsdbLoadDataFromCache(STsdb *pTsdb, STable *pTable, STbDataIter *pIter, TSKEY maxKey, int maxRowsToRead,
                                 SDataCols *pCols, TKEY *filterKeys, int nFilterKeys, bool keepDup,
                                 SMergeInfo *pMergeInfo) {
  ASSERT(maxRowsToRead > 0 && nFilterKeys >= 0);
  if (pIter == NULL) return 0;
  STSchema *pSchema = NULL;
  TSKEY     rowKey = 0;
  TSKEY     fKey = 0;
  // only fetch lastKey from mem data as file data not used in this function actually
  TSKEY      lastKey = TSKEY_INITIAL_VAL;
  bool       isRowDel = false;
  int        filterIter = 0;
  STSRow    *row = NULL;
  SMergeInfo mInfo;

  // TODO: support Multi-Version(the rows with the same TS keys in memory can't be merged if its version refered by
  // query handle)

  if (pMergeInfo == NULL) pMergeInfo = &mInfo;

  memset(pMergeInfo, 0, sizeof(*pMergeInfo));
  pMergeInfo->keyFirst = INT64_MAX;
  pMergeInfo->keyLast = INT64_MIN;
  if (pCols) tdResetDataCols(pCols);

  row = tsdbNextIterRow(pIter);
  if (row == NULL || TD_ROW_KEY(row) > maxKey) {
    rowKey = INT64_MAX;
    isRowDel = false;
  } else {
    rowKey = TD_ROW_KEY(row);
    isRowDel = TD_ROW_IS_DELETED(row);
  }

  if (filterIter >= nFilterKeys) {
    fKey = INT64_MAX;
  } else {
    fKey = tdGetKey(filterKeys[filterIter]);
  }
  // 1. fkey - no dup since merged up to maxVersion of each query handle by tsdbLoadBlockDataCols
  // 2. rowKey - would dup since Multi-Version supported
  while (true) {
    if (fKey == INT64_MAX && rowKey == INT64_MAX) break;

    if (fKey < rowKey) {
      pMergeInfo->keyFirst = TMIN(pMergeInfo->keyFirst, fKey);
      pMergeInfo->keyLast = TMAX(pMergeInfo->keyLast, fKey);

      filterIter++;
      if (filterIter >= nFilterKeys) {
        fKey = INT64_MAX;
      } else {
        fKey = tdGetKey(filterKeys[filterIter]);
      }
#if 1
    } else if (fKey > rowKey) {
      if (isRowDel) {
        // TODO: support delete function
        pMergeInfo->rowsDeleteFailed++;
      } else {
        if (pMergeInfo->rowsInserted - pMergeInfo->rowsDeleteSucceed >= maxRowsToRead) break;
        if (pCols && pMergeInfo->nOperations >= pCols->maxPoints) break;

        if (lastKey != rowKey) {
          pMergeInfo->rowsInserted++;
          pMergeInfo->nOperations++;
          pMergeInfo->keyFirst = TMIN(pMergeInfo->keyFirst, rowKey);
          pMergeInfo->keyLast = TMAX(pMergeInfo->keyLast, rowKey);
          if (pCols) {
            if (lastKey != TSKEY_INITIAL_VAL) {
              ++pCols->numOfRows;
            }
            tsdbAppendTableRowToCols(pTsdb, pTable, pCols, &pSchema, row, false);
          }
          lastKey = rowKey;
        } else {
          if (keepDup) {
            tsdbAppendTableRowToCols(pTsdb, pTable, pCols, &pSchema, row, true);
          } else {
            // discard
          }
        }
      }

      tsdbTbDataIterNext(pIter);
      row = tsdbNextIterRow(pIter);
      if (row == NULL || TD_ROW_KEY(row) > maxKey) {
        rowKey = INT64_MAX;
        isRowDel = false;
      } else {
        rowKey = TD_ROW_KEY(row);
        isRowDel = TD_ROW_IS_DELETED(row);
      }
    } else {           // fkey == rowKey
      if (isRowDel) {  // TODO: support delete function(How to stands for delete in file? rowVersion = -1?)
        ASSERT(!keepDup);
        if (pCols && pMergeInfo->nOperations >= pCols->maxPoints) break;
        pMergeInfo->rowsDeleteSucceed++;
        pMergeInfo->nOperations++;
        tsdbAppendTableRowToCols(pTsdb, pTable, pCols, &pSchema, row, false);
      } else {
        if (keepDup) {
          if (pCols && pMergeInfo->nOperations >= pCols->maxPoints) break;
          if (lastKey != rowKey) {
            pMergeInfo->rowsUpdated++;
            pMergeInfo->nOperations++;
            pMergeInfo->keyFirst = TMIN(pMergeInfo->keyFirst, rowKey);
            pMergeInfo->keyLast = TMAX(pMergeInfo->keyLast, rowKey);
            if (pCols) {
              if (lastKey != TSKEY_INITIAL_VAL) {
                ++pCols->numOfRows;
              }
              tsdbAppendTableRowToCols(pTsdb, pTable, pCols, &pSchema, row, false);
            }
            lastKey = rowKey;
          } else {
            tsdbAppendTableRowToCols(pTsdb, pTable, pCols, &pSchema, row, true);
          }
        } else {
          pMergeInfo->keyFirst = TMIN(pMergeInfo->keyFirst, fKey);
          pMergeInfo->keyLast = TMAX(pMergeInfo->keyLast, fKey);
        }
      }

      tsdbTbDataIterNext(pIter);
      row = tsdbNextIterRow(pIter);
      if (row == NULL || TD_ROW_KEY(row) > maxKey) {
        rowKey = INT64_MAX;
        isRowDel = false;
      } else {
        rowKey = TD_ROW_KEY(row);
        isRowDel = TD_ROW_IS_DELETED(row);
      }

      filterIter++;
      if (filterIter >= nFilterKeys) {
        fKey = INT64_MAX;
      } else {
        fKey = tdGetKey(filterKeys[filterIter]);
      }
    }
#endif
  }
  if (pCols && (lastKey != TSKEY_INITIAL_VAL)) {
    ++pCols->numOfRows;
  }

  return 0;
}
#endif