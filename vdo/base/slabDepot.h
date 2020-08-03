/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabDepot.h#37 $
 */

#ifndef SLAB_DEPOT_H
#define SLAB_DEPOT_H

#include "adminState.h"
#include "atomic.h"
#include "completion.h"
#include "fixedLayout.h"
#include "journalPoint.h"
#include "slabDepotFormat.h"
#include "statistics.h"
#include "types.h"
#include "waitQueue.h"

/**
 * A slab_depot is responsible for managing all of the slabs and block
 * allocators of a VDO. It has a single array of slabs in order to eliminate
 * the need for additional math in order to compute which physical zone a PBN
 * is in. It also has a block_allocator per zone.
 *
 * Load operations are required to be performed on a single thread. Normal
 * operations are assumed to be performed in the appropriate zone. Allocations
 * and reference count updates must be done from the thread of their physical
 * zone. Requests to commit slab journal tail blocks from the recovery journal
 * must be done on the journal zone thread. Save operations are required to be
 * launched from the same thread as the original load operation.
 **/

typedef enum {
	NORMAL_LOAD,
	RECOVERY_LOAD,
	REBUILD_LOAD
} slab_depot_load_type;

/**
 * Calculate the number of slabs a depot would have.
 *
 * @param depot  The depot
 *
 * @return The number of slabs
 **/
slab_count_t __must_check calculate_slab_count(struct slab_depot *depot);

/**
 * Make a slab depot and configure it with the state read from the super block.
 *
 * @param [in]  state               The slab depot state from the super block
 * @param [in]  thread_config       The thread config of the VDO
 * @param [in]  nonce               The nonce of the VDO
 * @param [in]  layer               The physical layer below this depot
 * @param [in]  summary_partition   The partition which holds the slab summary
 * @param [in]  read_only_notifier  The context for entering read-only mode
 * @param [in]  recovery_journal    The recovery journal of the VDO
 * @param [in]  vdo_state           A pointer to the VDO's atomic state
 * @param [out] depot_ptr           A pointer to hold the depot
 *
 * @return A success or error code
 **/
int __must_check
decode_slab_depot(struct slab_depot_state_2_0 state,
		  const struct thread_config *thread_config,
		  nonce_t nonce,
		  PhysicalLayer *layer,
		  struct partition *summary_partition,
		  struct read_only_notifier *read_only_notifier,
		  struct recovery_journal *recovery_journal,
		  Atomic32 *vdo_state,
		  struct slab_depot **depot_ptr);

/**
 * Destroy a slab depot and null out the reference to it.
 *
 * @param depot_ptr  The reference to the depot to destroy
 **/
void free_slab_depot(struct slab_depot **depot_ptr);

/**
 * Record the state of a slab depot for encoding into the super block.
 *
 * @param depot   The depot to encode
 *
 * @return The depot state
 **/
struct slab_depot_state_2_0 __must_check
record_slab_depot(const struct slab_depot *depot);

/**
 * Allocate the RefCounts for all slabs in the depot. This method may be called
 * only before entering normal operation from the load thread.
 *
 * @param depot  The depot whose RefCounts need allocation
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check allocate_slab_ref_counts(struct slab_depot *depot);

/**
 * Get the block allocator for a specified physical zone from a depot.
 *
 * @param depot        The depot
 * @param zone_number  The physical zone
 *
 * @return The block allocator for the specified zone
 **/
struct block_allocator * __must_check
get_block_allocator_for_zone(struct slab_depot *depot,
			     zone_count_t zone_number);

/**
 * Get the number of the slab that contains a specified block.
 *
 * @param depot            The slab depot
 * @param pbn              The physical block number
 * @param slab_number_ptr  A pointer to hold the slab number
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check get_slab_number(const struct slab_depot *depot,
				 physical_block_number_t pbn,
				 slab_count_t *slab_number_ptr);

/**
 * Get the slab object for the slab that contains a specified block. Will put
 * the VDO in read-only mode if the PBN is not a valid data block nor the zero
 * block.
 *
 * @param depot  The slab depot
 * @param pbn    The physical block number
 *
 * @return The slab containing the block, or NULL if the block number is the
 *         zero block or otherwise out of range
 **/
struct vdo_slab * __must_check
get_slab(const struct slab_depot *depot, physical_block_number_t pbn);

/**
 * Get the slab journal for the slab that contains a specified block.
 *
 * @param depot  The slab depot
 * @param pbn    The physical block number within the block depot partition
 *               of any block in the slab
 *
 * @return The slab journal of the slab containing the block, or NULL if the
 *         block number is for the zero block or otherwise out of range
 **/
struct slab_journal * __must_check
get_slab_journal(const struct slab_depot *depot, physical_block_number_t pbn);

/**
 * Determine how many new references a block can acquire. This method must be
 * called from the the physical zone thread of the PBN.
 *
 * @param depot  The slab depot
 * @param pbn    The physical block number that is being queried
 *
 * @return the number of available references
 **/
uint8_t __must_check
get_increment_limit(struct slab_depot *depot, physical_block_number_t pbn);

/**
 * Determine whether the given PBN refers to a data block.
 *
 * @param depot  The depot
 * @param pbn    The physical block number to ask about
 *
 * @return <code>True</code> if the PBN corresponds to a data block
 **/
bool __must_check
is_physical_data_block(const struct slab_depot *depot,
		       physical_block_number_t pbn);

/**
 * Get the total number of data blocks allocated across all the slabs in the
 * depot, which is the total number of blocks with a non-zero reference count.
 * This may be called from any thread.
 *
 * @param depot  The slab depot
 *
 * @return The total number of blocks with a non-zero reference count
 **/
block_count_t __must_check
get_depot_allocated_blocks(const struct slab_depot *depot);

/**
 * Get the total of the statistics from all the block allocators in the depot.
 *
 * @param depot  The slab depot
 *
 * @return The statistics from all block allocators in the depot
 **/
struct block_allocator_statistics __must_check
get_depot_block_allocator_statistics(const struct slab_depot *depot);

/**
 * Get the total number of data blocks in all the slabs in the depot. This may
 * be called from any thread.
 *
 * @param depot  The slab depot
 *
 * @return The total number of data blocks in all slabs
 **/
block_count_t __must_check
get_depot_data_blocks(const struct slab_depot *depot);

/**
 * Get the total number of free blocks remaining in all the slabs in the
 * depot, which is the total number of blocks that have a zero reference
 * count. This may be called from any thread.
 *
 * @param depot  The slab depot
 *
 * @return The total number of blocks with a zero reference count
 **/
block_count_t __must_check
get_depot_free_blocks(const struct slab_depot *depot);

/**
 * Get the total number of slabs in the depot
 *
 * @param depot  The slab depot
 *
 * @return The total number of slabs
 **/
slab_count_t __must_check get_depot_slab_count(const struct slab_depot *depot);

/**
 * Get the total number of unrecovered slabs in the depot, which is the total
 * number of unrecovered slabs from all zones. This may be called from any
 * thread.
 *
 * @param depot  The slab depot
 *
 * @return The total number of slabs that are unrecovered
 **/
slab_count_t __must_check
get_depot_unrecovered_slab_count(const struct slab_depot *depot);

/**
 * Get the aggregated slab journal statistics for the depot.
 *
 * @param depot  The slab depot
 *
 * @return The aggregated statistics for all slab journals in the depot
 **/
struct slab_journal_statistics __must_check
get_depot_slab_journal_statistics(const struct slab_depot *depot);

/**
 * Get the cumulative RefCounts statistics for the depot.
 *
 * @param depot  The slab depot
 *
 * @return The cumulative statistics for all RefCounts in the depot
 **/
struct ref_counts_statistics __must_check
get_depot_ref_counts_statistics(const struct slab_depot *depot);

/**
 * Asynchronously load any slab depot state that isn't included in the
 * SuperBlock component. This method may be called only before entering normal
 * operation from the load thread.
 *
 * @param depot        The depot to load
 * @param operation    The type of load to perform
 * @param parent       The completion to finish when the load is complete
 * @param context      Additional context for the load operation; may be NULL
 **/
void load_slab_depot(struct slab_depot *depot,
		     AdminStateCode operation,
		     struct vdo_completion *parent,
		     void *context);

/**
 * Prepare the slab depot to come online and start allocating blocks. This
 * method may be called only before entering normal operation from the load
 * thread. It must be called before allocation may proceed.
 *
 * @param depot       The depot to prepare
 * @param load_type  The load type
 * @param parent      The completion to finish when the operation is complete
 **/
void prepare_to_allocate(struct slab_depot *depot,
			 slab_depot_load_type load_type,
			 struct vdo_completion *parent);

/**
 * Update the slab depot to reflect its new size in memory. This size is saved
 * to disk as part of the super block.
 *
 * @param depot  The depot to update
 **/
void update_slab_depot_size(struct slab_depot *depot);

/**
 * Allocate new memory needed for a resize of a slab depot to the given size.
 *
 * @param depot     The depot to prepare to resize
 * @param new_size  The number of blocks in the new depot
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
prepare_to_grow_slab_depot(struct slab_depot *depot, block_count_t new_size);

/**
 * Use the new slabs allocated for resize.
 *
 * @param depot   The depot
 * @param parent  The object to notify when complete
 **/
void use_new_slabs(struct slab_depot *depot, struct vdo_completion *parent);

/**
 * Abandon any new slabs in this depot, freeing them as needed.
 *
 * @param depot  The depot
 **/
void abandon_new_slabs(struct slab_depot *depot);

/**
 * Drain all slab depot I/O. If saving, or flushing, all dirty depot metadata
 * will be written out. If saving or suspending, the depot will be left in a
 * suspended state.
 *
 * @param depot      The depot to drain
 * @param operation  The drain operation (flush, rebuild, suspend, or save)
 * @param parent     The completion to finish when the drain is complete
 **/
void drain_slab_depot(struct slab_depot *depot,
		      AdminStateCode operation,
		      struct vdo_completion *parent);

/**
 * Resume a suspended slab depot.
 *
 * @param depot   The depot to resume
 * @param parent  The completion to finish when the depot has resumed
 **/
void resume_slab_depot(struct slab_depot *depot, struct vdo_completion *parent);

/**
 * Commit all dirty tail blocks which are locking a given recovery journal
 * block. This method must be called from the journal zone thread.
 *
 * @param depot                  The depot
 * @param recovery_block_number  The sequence number of the recovery journal
 *                               block whose locks should be released
 **/
void
commit_oldest_slab_journal_tail_blocks(struct slab_depot *depot,
				       sequence_number_t recovery_block_number);

/**
 * Get the slab_config of a depot.
 *
 * @param depot  The slab depot
 *
 * @return The slab configuration of the specified depot
 **/
const struct slab_config * __must_check
get_slab_config(const struct slab_depot *depot);

/**
 * Get the slab summary.
 *
 * @param depot  The slab depot
 *
 * @return The slab summary
 **/
struct slab_summary * __must_check
get_slab_summary(const struct slab_depot *depot);

/**
 * Get the portion of the slab summary for a given physical zone.
 *
 * @param depot  The slab depot
 * @param zone   The zone
 *
 * @return The portion of the slab summary for the specified zone
 **/
struct slab_summary_zone * __must_check
get_slab_summary_for_zone(const struct slab_depot *depot, zone_count_t zone);

/**
 * Scrub all unrecovered slabs.
 *
 * @param depot         The depot to scrub
 * @param parent        The object to notify when scrubbing has been launched
 *                      for all zones
 **/
void scrub_all_unrecovered_slabs(struct slab_depot *depot,
				 struct vdo_completion *parent);

/**
 * Check whether there are outstanding unrecovered slabs.
 *
 * @param depot  The slab depot
 *
 * @return Whether there are outstanding unrecovered slabs
 **/
bool has_unrecovered_slabs(struct slab_depot *depot);

/**
 * Get the physical size to which this depot is prepared to grow.
 *
 * @param depot  The slab depot
 *
 * @return The new number of blocks the depot will be grown to, or 0 if the
 *         depot is not prepared to grow
 **/
block_count_t __must_check get_new_depot_size(const struct slab_depot *depot);

/**
 * Dump the slab depot, in a thread-unsafe fashion.
 *
 * @param depot  The slab depot
 **/
void dump_slab_depot(const struct slab_depot *depot);

#endif // SLAB_DEPOT_H
