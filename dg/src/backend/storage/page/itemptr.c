/*-------------------------------------------------------------------------
 *
 * itemptr.c
 *	  POSTGRES disk item pointer code.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/page/itemptr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/itemptr.h"
#include "ty_small.h"

/*
 * We really want ItemPointerData to be exactly 6 bytes.
 */
StaticAssertDecl(sizeof(ItemPointerData) == 3 * sizeof(uint16),
                 "ItemPointerData struct is improperly padded");

/*
 * ItemPointerEquals
 *	Returns true if both item pointers point to the same item,
 *	 otherwise returns false.
 *
 * Note:
 *	Asserts that the disk item pointers are both valid!
 */
bool ItemPointerEquals(ItemPointer pointer1, ItemPointer pointer2) {
  if (ItemPointerGetBlockNumber(pointer1) ==
          ItemPointerGetBlockNumber(pointer2) &&
      ItemPointerGetOffsetNumber(pointer1) ==
          ItemPointerGetOffsetNumber(pointer2))
    return true;
  else
    return false;
}

/*
 * ItemPointerCompare
 *		Generic btree-style comparison for item pointers.
 */
int32 ItemPointerCompare(ItemPointer arg1, ItemPointer arg2) {
  /* the struct is ty's: the block, then the offset, with no validity check */
  return ty_tid_cmp((const ty_tid_t *)arg1, (const ty_tid_t *)arg2);
}

/*
 * ItemPointerInc
 *		Increment 'pointer' by 1 only paying attention to the
 *ItemPointer's type's range limits and not MaxOffsetNumber and
 *FirstOffsetNumber. This may result in 'pointer' becoming !OffsetNumberIsValid.
 *
 * If the pointer is already the maximum possible values permitted by the
 * range of the ItemPointer's types, then do nothing.
 */
void ItemPointerInc(ItemPointer pointer) {
  BlockNumber blk = ItemPointerGetBlockNumberNoCheck(pointer);
  OffsetNumber off = ItemPointerGetOffsetNumberNoCheck(pointer);

  if (off == PG_UINT16_MAX) {
    if (blk != InvalidBlockNumber) {
      off = 0;
      blk++;
    }
  } else
    off++;

  ItemPointerSet(pointer, blk, off);
}

/*
 * ItemPointerDec
 *		Decrement 'pointer' by 1 only paying attention to the
 *ItemPointer's type's range limits and not MaxOffsetNumber and
 *FirstOffsetNumber. This may result in 'pointer' becoming !OffsetNumberIsValid.
 *
 * If the pointer is already the minimum possible values permitted by the
 * range of the ItemPointer's types, then do nothing.  This does rely on
 * FirstOffsetNumber being 1 rather than 0.
 */
void ItemPointerDec(ItemPointer pointer) {
  BlockNumber blk = ItemPointerGetBlockNumberNoCheck(pointer);
  OffsetNumber off = ItemPointerGetOffsetNumberNoCheck(pointer);

  if (off == 0) {
    if (blk != 0) {
      off = PG_UINT16_MAX;
      blk--;
    }
  } else
    off--;

  ItemPointerSet(pointer, blk, off);
}
