/*
 * Copyright Red Hat
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slab.c#55 $
 */

#include "slab.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "adminState.h"
#include "blockAllocatorInternals.h"
#include "completion.h"
#include "constants.h"
#include "numUtils.h"
#include "pbnLock.h"
#include "recoveryJournal.h"
#include "refCounts.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "slabJournalFormat.h"
#include "slabJournalInternals.h"
#include "slabSummary.h"

/**********************************************************************/
int make_slab(physical_block_number_t slab_origin,
	      struct block_allocator *allocator,
	      physical_block_number_t translation,
	      struct recovery_journal *recovery_journal,
	      slab_count_t slab_number,
	      bool is_new,
	      struct vdo_slab **slab_ptr)
{
	const struct slab_config *slab_config =
		get_slab_config(allocator->depot);

	struct vdo_slab *slab;
	int result = ALLOCATE(1, struct vdo_slab, __func__, &slab);
	if (result != VDO_SUCCESS) {
		return result;
	}

	slab->allocator = allocator;
	slab->start = slab_origin;
	slab->end = slab->start + slab_config->slab_blocks;
	slab->slab_number = slab_number;
	INIT_LIST_HEAD(&slab->allocq_entry);

	slab->ref_counts_origin =
		slab_origin + slab_config->data_blocks + translation;
	slab->journal_origin =
		(get_slab_journal_start_block(slab_config, slab_origin)
		 + translation);

	result = make_slab_journal(allocator, slab, recovery_journal,
				   &slab->journal);
	if (result != VDO_SUCCESS) {
		free_slab(&slab);
		return result;
	}

	if (is_new) {
		slab->state.current_state = ADMIN_STATE_NEW;
		result = allocate_ref_counts_for_slab(slab);
		if (result != VDO_SUCCESS) {
			free_slab(&slab);
			return result;
		}
	}

	*slab_ptr = slab;
	return VDO_SUCCESS;
}

/**********************************************************************/
int allocate_ref_counts_for_slab(struct vdo_slab *slab)
{
	struct block_allocator *allocator = slab->allocator;
	const struct slab_config *slab_config
		= get_slab_config(allocator->depot);

	int result = ASSERT(slab->reference_counts == NULL,
			    "vdo_slab %u doesn't allocate refcounts twice",
			    slab->slab_number);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return make_ref_counts(slab_config->data_blocks,
			       slab,
			       slab->ref_counts_origin,
			       allocator->read_only_notifier,
			       &slab->reference_counts);
}

/**********************************************************************/
void free_slab(struct vdo_slab **slab_ptr)
{
	struct vdo_slab *slab = *slab_ptr;
	if (slab == NULL) {
		return;
	}

	list_del(&slab->allocq_entry);
	free_slab_journal(&slab->journal);
	free_ref_counts(&slab->reference_counts);
	FREE(slab);
	*slab_ptr = NULL;
}

/**********************************************************************/
zone_count_t get_slab_zone_number(struct vdo_slab *slab)
{
	return slab->allocator->zone_number;
}

/**********************************************************************/
void mark_slab_replaying(struct vdo_slab *slab)
{
	if (slab->status == SLAB_REBUILT) {
		slab->status = SLAB_REPLAYING;
	}
}

/**********************************************************************/
void mark_slab_unrecovered(struct vdo_slab *slab)
{
	slab->status = SLAB_REQUIRES_SCRUBBING;
}

/**********************************************************************/
block_count_t get_slab_free_block_count(const struct vdo_slab *slab)
{
	return get_unreferenced_block_count(slab->reference_counts);
}

/**********************************************************************/
int modify_slab_reference_count(struct vdo_slab *slab,
				const struct journal_point *journal_point,
				struct reference_operation operation)
{
	bool free_status_changed;
	int result;

	if (slab == NULL) {
		return VDO_SUCCESS;
	}

	/*
	 * If the slab is unrecovered, preserve the refCount state and let
	 * scrubbing correct the refCount. Note that the slab journal has
	 * already captured all refCount updates.
	 */
	if (is_unrecovered_slab(slab)) {
		sequence_number_t entry_lock = journal_point->sequence_number;
		adjust_slab_journal_block_reference(slab->journal,
						    entry_lock,
						    -1);
		return VDO_SUCCESS;
	}

	result = adjust_reference_count(slab->reference_counts, operation,
					journal_point, &free_status_changed);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (free_status_changed) {
		adjust_vdo_free_block_count(slab,
					    !is_increment_operation(operation.type));
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
void open_slab(struct vdo_slab *slab)
{
	reset_search_cursor(slab->reference_counts);
	if (is_slab_journal_blank(slab->journal)) {
		WRITE_ONCE(slab->allocator->statistics.slabs_opened,
			   slab->allocator->statistics.slabs_opened + 1);
		dirty_all_reference_blocks(slab->reference_counts);
	} else {
		WRITE_ONCE(slab->allocator->statistics.slabs_reopened,
			   slab->allocator->statistics.slabs_reopened + 1);
	}
}

/**********************************************************************/
int acquire_provisional_reference(struct vdo_slab *slab,
				  physical_block_number_t pbn,
				  struct pbn_lock *lock)
{
	int result;

	if (vdo_pbn_lock_has_provisional_reference(lock)) {
		return VDO_SUCCESS;
	}

	result =
		provisionally_reference_block(slab->reference_counts, pbn, lock);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (vdo_pbn_lock_has_provisional_reference(lock)) {
		adjust_vdo_free_block_count(slab, false);
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int slab_block_number_from_pbn(struct vdo_slab *slab,
			       physical_block_number_t physical_block_number,
			       slab_block_number *slab_block_number_ptr)
{
	uint64_t slab_block_number;
	if (physical_block_number < slab->start) {
		return VDO_OUT_OF_RANGE;
	}

	slab_block_number = physical_block_number - slab->start;
	if (slab_block_number
	    >= get_slab_config(slab->allocator->depot)->data_blocks) {
		return VDO_OUT_OF_RANGE;
	}

	*slab_block_number_ptr = slab_block_number;
	return VDO_SUCCESS;
}

/**********************************************************************/
bool should_save_fully_built_slab(const struct vdo_slab *slab)
{
	// Write out the ref_counts if the slab has written them before, or it
	// has any non-zero reference counts, or there are any slab journal
	// blocks.
	block_count_t data_blocks =
		get_slab_config(slab->allocator->depot)->data_blocks;
	return (must_load_ref_counts(slab->allocator->summary,
				     slab->slab_number)
		|| (get_slab_free_block_count(slab) != data_blocks)
		|| !is_slab_journal_blank(slab->journal));
}

/**
 * Initiate a slab action.
 *
 * Implements vdo_admin_initiator.
 **/
static void initiate_slab_action(struct admin_state *state)
{
	struct vdo_slab *slab = container_of(state, struct vdo_slab, state);

	if (is_vdo_state_draining(state)) {
		enum admin_state_code operation =
			get_vdo_admin_state_code(state);
		if (operation == ADMIN_STATE_SCRUBBING) {
			slab->status = SLAB_REBUILDING;
		}

		drain_slab_journal(slab->journal);

		if (slab->reference_counts != NULL) {
			drain_ref_counts(slab->reference_counts);
		}

		check_if_slab_drained(slab);
		return;
	}

	if (is_vdo_state_loading(state)) {
		decode_slab_journal(slab->journal);
		return;
	}

	if (is_vdo_state_resuming(state)) {
		queue_vdo_slab(slab);
		finish_vdo_resuming(state);
		return;
	}

	finish_vdo_operation_with_result(state, VDO_INVALID_ADMIN_STATE);
}

/**********************************************************************/
void start_slab_action(struct vdo_slab *slab,
		       enum admin_state_code operation,
		       struct vdo_completion *parent)
{
	start_vdo_operation_with_waiter(&slab->state, operation, parent,
					initiate_slab_action);
}

/**********************************************************************/
void notify_slab_journal_is_loaded(struct vdo_slab *slab, int result)
{
	if ((result == VDO_SUCCESS) && is_vdo_state_clean_load(&slab->state)) {
		// Since this is a normal or new load, we don't need the memory
		// to read and process the recovery journal, so we can allocate
		// reference counts now.
		result = allocate_ref_counts_for_slab(slab);
	}

	finish_vdo_loading_with_result(&slab->state, result);
}

/**********************************************************************/
bool is_slab_open(struct vdo_slab *slab)
{
	return (!is_vdo_state_quiescing(&slab->state) &&
		!is_vdo_state_quiescent(&slab->state));
}

/**********************************************************************/
bool is_slab_draining(struct vdo_slab *slab)
{
	return is_vdo_state_draining(&slab->state);
}

/**********************************************************************/
void check_if_slab_drained(struct vdo_slab *slab)
{
	if (is_vdo_state_draining(&slab->state) &&
	    !is_slab_journal_active(slab->journal) &&
	    ((slab->reference_counts == NULL) ||
	     !are_ref_counts_active(slab->reference_counts))) {
		int result = (is_read_only(slab->allocator->read_only_notifier)
				      ? VDO_READ_ONLY
				      : VDO_SUCCESS);
		finish_vdo_draining_with_result(&slab->state, result);
	}
}

/**********************************************************************/
void notify_ref_counts_are_drained(struct vdo_slab *slab, int result)
{
	finish_vdo_draining_with_result(&slab->state, result);
}

/**********************************************************************/
bool is_slab_resuming(struct vdo_slab *slab)
{
	return is_vdo_state_resuming(&slab->state);
}

/**********************************************************************/
void finish_scrubbing_slab(struct vdo_slab *slab)
{
	slab->status = SLAB_REBUILT;
	queue_vdo_slab(slab);
	reopen_slab_journal(slab->journal);
}

/**********************************************************************/
static const char *status_to_string(enum slab_rebuild_status status)
{
	switch (status) {
	case SLAB_REBUILT:
		return "REBUILT";
	case SLAB_REQUIRES_SCRUBBING:
		return "SCRUBBING";
	case SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING:
		return "PRIORITY_SCRUBBING";
	case SLAB_REBUILDING:
		return "REBUILDING";
	case SLAB_REPLAYING:
		return "REPLAYING";
	default:
		return "UNKNOWN";
	}
}

/**********************************************************************/
void dump_slab(const struct vdo_slab *slab)
{
	if (slab->reference_counts != NULL) {
		// Terse because there are a lot of slabs to dump and syslog is
		// lossy.
		log_info("slab %u: P%u, %llu free", slab->slab_number,
			 slab->priority, get_slab_free_block_count(slab));
	} else {
		log_info("slab %u: status %s", slab->slab_number,
			 status_to_string(slab->status));
	}

	dump_slab_journal(slab->journal);

	if (slab->reference_counts != NULL) {
		dump_ref_counts(slab->reference_counts);
	} else {
		log_info("refCounts is null");
	}
}
