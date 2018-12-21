/*
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/bio.h#3 $
 */

#ifndef BIO_H
#define BIO_H

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/version.h>

#include "kernelTypes.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0)
#define USE_BI_ITER 1
#endif

/**
 * Copy the bio data to a char array.
 *
 * @param bio      The bio to copy the data from
 * @param dataPtr  The local array to copy the data to
 **/
void bioCopyDataIn(struct bio *bio, char *dataPtr);

/**
 * Copy a char array to the bio data.
 *
 * @param bio      The bio to copy the data to
 * @param dataPtr  The local array to copy the data from
 **/
void bioCopyDataOut(struct bio *bio, char *dataPtr);

/**
 * Set the bi_rw or equivalent field of a bio to a particular data
 * operation. Intended to be called only by setBioOperationRead() etc.
 *
 * @param bio        The bio to modify
 * @param operation  The operation to set it to
 **/
void setBioOperation(struct bio *bio, unsigned int operation);

/**********************************************************************/
static inline void setBioOperationRead(struct bio *bio)
{
  setBioOperation(bio, READ);
}

/**********************************************************************/
static inline void setBioOperationWrite(struct bio *bio)
{
  setBioOperation(bio, WRITE);
}

/**********************************************************************/
static inline void clearBioOperationAndFlags(struct bio *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  bio->bi_opf = 0;
#else
  bio->bi_rw = 0;
#endif
}

/**********************************************************************/
static inline void copyBioOperationAndFlags(struct bio *to, struct bio *from)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  to->bi_opf = from->bi_opf;
#else
  to->bi_rw = from->bi_rw;
#endif
}

/**********************************************************************/
static inline void setBioOperationFlag(struct bio *bio, unsigned int flag)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  bio->bi_opf |= flag;
#else
  bio->bi_rw |= flag;
#endif
}

/**********************************************************************/
static inline void clearBioOperationFlag(struct bio *bio, unsigned int flag)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  bio->bi_opf &= ~flag;
#else
  bio->bi_rw &= ~flag;
#endif
}

/**********************************************************************/
static inline void setBioOperationFlagPreflush(struct bio *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  setBioOperationFlag(bio, REQ_PREFLUSH);
#else
  // Preflushes and empty flushes are not currently distinguished.
  setBioOperation(bio, WRITE_FLUSH);
#endif
}

/**********************************************************************/
static inline void setBioOperationFlagSync(struct bio *bio)
{
  setBioOperationFlag(bio, REQ_SYNC);
}

/**********************************************************************/
static inline void clearBioOperationFlagSync(struct bio *bio)
{
  clearBioOperationFlag(bio, REQ_SYNC);
}

/**********************************************************************/
static inline void setBioOperationFlagFua(struct bio *bio)
{
  setBioOperationFlag(bio, REQ_FUA);
}

/**********************************************************************/
static inline void clearBioOperationFlagFua(struct bio *bio)
{
  clearBioOperationFlag(bio, REQ_FUA);
}

/**********************************************************************/
static inline bool isDiscardBio(struct bio *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  return (bio != NULL) && (bio_op(bio) == REQ_OP_DISCARD);
#else
  return (bio != NULL) && ((bio->bi_rw & REQ_DISCARD) != 0);
#endif
}

/**********************************************************************/
static inline bool isFlushBio(struct bio *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  return (bio_op(bio) == REQ_OP_FLUSH) || ((bio->bi_opf & REQ_PREFLUSH) != 0);
#else
  return (bio->bi_rw & REQ_FLUSH) != 0;
#endif
}

/**********************************************************************/
static inline bool isFUABio(struct bio *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  return (bio->bi_opf & REQ_FUA) != 0;
#else
  return (bio->bi_rw & REQ_FUA) != 0;
#endif
}

/**********************************************************************/
static inline bool isReadBio(struct bio *bio)
{
  return bio_data_dir(bio) == READ;
}

/**********************************************************************/
static inline bool isWriteBio(struct bio *bio)
{
  return bio_data_dir(bio) == WRITE;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
/**
 * Get the error from the bio.
 *
 * @param bio  The bio
 *
 * @return the bio's error if any
 **/
static inline int getBioResult(struct bio *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,13,0)
  return blk_status_to_errno(bio->bi_status);
#else
  return bio->bi_error;
#endif
}
#endif // newer than 4.4

/**
 * Set the block device for a bio.
 *
 * @param bio     The bio to modify
 * @param device  The new block device for the bio
 **/
static inline void setBioBlockDevice(struct bio          *bio,
                                     struct block_device *device)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
  bio_set_dev(bio, device);
#else
  bio->bi_bdev = device;
#endif
}

/**
 * Get a bio's size.
 *
 * @param bio  The bio
 *
 * @return the bio's size
 **/
static inline unsigned int getBioSize(struct bio *bio)
{
#ifdef USE_BI_ITER
  return bio->bi_iter.bi_size;
#else
  return bio->bi_size;
#endif
}

/**
 * Set the bio's sector.
 *
 * @param bio     The bio
 * @param sector  The sector
 **/
static inline void setBioSector(struct bio *bio, sector_t sector)
{
#ifdef USE_BI_ITER
  bio->bi_iter.bi_sector = sector;
#else
  bio->bi_sector = sector;
#endif
}

/**
 * Get the bio's sector.
 *
 * @param bio  The bio
 *
 * @return the sector
 **/
static inline sector_t getBioSector(struct bio *bio)
{
#ifdef USE_BI_ITER
  return bio->bi_iter.bi_sector;
#else
  return bio->bi_sector;
#endif
}

/**
 * Tell the kernel we've completed processing of this bio.
 *
 * @param bio    The bio to complete
 * @param error  A system error code, or 0 for success
 **/
static inline void completeBio(struct bio *bio, int error)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,13,0)
  bio->bi_status = errno_to_blk_status(error);
  bio_endio(bio);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  bio->bi_error = error;
  bio_endio(bio);
#else
  bio_endio(bio, error);
#endif
}

/**
 * Frees up a bio structure
 *
 * @param bio    The bio to free
 * @param layer  The layer the bio was created in
 **/
void freeBio(struct bio *bio, KernelLayer *layer);

/**
 * Count the statistics for the bios.  This is used for calls into VDO and
 * for calls out of VDO.
 *
 * @param bioStats  Statistics structure to update
 * @param bio       The bio
 **/
void countBios(AtomicBioStats *bioStats, struct bio *bio);

/**
 * Reset a bio so it can be used again. May only be used on a VDO-allocated
 * bio, as it assumes the bio wraps a 4k buffer that is 4k aligned.
 *
 * @param bio    The bio to reset
 * @param layer  The physical layer
 **/
void resetBio(struct bio *bio, KernelLayer *layer);

/**
 * Set a bio's data to all zeroes.
 *
 * @param [in] bio  The bio
 **/
void bioZeroData(struct bio *bio);

/**
 * Create a new bio structure for kernel buffer storage.
 *
 * @param [in]  layer   The physical layer
 * @param [in]  data    The buffer (can be NULL)
 * @param [out] bioPtr  A pointer to hold new bio
 *
 * @return VDO_SUCCESS or an error
 **/
int createBio(KernelLayer *layer, char *data, struct bio **bioPtr);

/**
 * Prepare a bio to issue a flush to the device below.
 *
 * @param bio            The flush bio
 * @param context        The context for the callback
 * @param device         The device to flush
 * @param endIOCallback  The function to call when the flush is complete
 **/
void prepareFlushBIO(struct bio          *bio,
                     void                *context,
                     struct block_device *device,
                     bio_end_io_t        *endIOCallback);

/**
 * Perform IO with a bio, waiting for completion and returning its result.
 * The bio must already have its sector, block device, and operation set.
 *
 * @param bio  The bio to do IO with
 *
 * @return The bio result
 **/
static inline int submitBioAndWait(struct bio *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
  submit_bio_wait(bio);
  int result = getBioResult(bio);
#else
  int result = submit_bio_wait(bio->bi_rw, bio);
#endif
  return result;
}

#endif /* BIO_H */
