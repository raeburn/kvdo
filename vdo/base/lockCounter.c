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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/lockCounter.c#5 $
 */

#include "lockCounter.h"

#include "atomic.h"
#include "memoryAlloc.h"

/**
 * A lock_counter is intended to keep all of the locks for the blocks in the
 * recovery journal. The per-zone counters are all kept in a single array which
 * is arranged by zone (i.e. zone 0's lock 0 is at index 0, zone 0's lock 1 is
 * at index 1, and zone 1's lock 0 is at index 'locks'.  This arrangement is
 * intended to minimize cache-line contention for counters from different
 * zones.
 *
 * The locks are implemented as a single object instead of as a lock counter
 * per lock both to afford this opportunity to reduce cache line contention and
 * also to eliminate the need to have a completion per lock.
 *
 * Lock sets are laid out with the set for recovery journal first, followed by
 * the logical zones, and then the physical zones.
 **/
struct lock_counter {
	/** The completion for notifying the owner of a lock release */
	struct vdo_completion completion;
	/** The number of logical zones which may hold locks */
	ZoneCount logical_zones;
	/** The number of physical zones which may hold locks */
	ZoneCount physical_zones;
	/** The number of locks */
	BlockCount locks;
	/** Whether the lock release notification is in flight */
	AtomicBool notifying;
	/** The number of logical zones which hold each lock */
	Atomic32 *logical_zone_counts;
	/** The number of physical zones which hold each lock */
	Atomic32 *physical_zone_counts;
	/** The per-zone, per-lock counts for the journal zone */
	uint16_t *journal_counters;
	/** The per-zone, per-lock decrement counts for the journal zone */
	Atomic32 *journal_decrement_counts;
	/** The per-zone, per-lock reference counts for logical zones */
	uint16_t *logical_counters;
	/** The per-zone, per-lock reference counts for physical zones */
	uint16_t *physical_counters;
};

/**********************************************************************/
int make_lock_counter(PhysicalLayer *layer, void *parent, vdo_action callback,
		      ThreadID threadID, ZoneCount logical_zones,
		      ZoneCount physical_zones, BlockCount locks,
		      struct lock_counter **lock_counter_ptr)
{
	struct lock_counter *lock_counter;

	int result = ALLOCATE(1, struct lock_counter, __func__, &lock_counter);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ALLOCATE(locks, uint16_t, __func__,
			  &lock_counter->journal_counters);
	if (result != VDO_SUCCESS) {
		free_lock_counter(&lock_counter);
		return result;
	}

	result = ALLOCATE(locks, Atomic32, __func__,
			  &lock_counter->journal_decrement_counts);
	if (result != VDO_SUCCESS) {
		free_lock_counter(&lock_counter);
		return result;
	}

	result = ALLOCATE(locks * logical_zones, uint16_t, __func__,
			  &lock_counter->logical_counters);
	if (result != VDO_SUCCESS) {
		free_lock_counter(&lock_counter);
		return result;
	}

	result = ALLOCATE(locks, Atomic32, __func__,
			  &lock_counter->logical_zone_counts);
	if (result != VDO_SUCCESS) {
		free_lock_counter(&lock_counter);
		return result;
	}

	result = ALLOCATE(locks * physical_zones, uint16_t, __func__,
			  &lock_counter->physical_counters);
	if (result != VDO_SUCCESS) {
		free_lock_counter(&lock_counter);
		return result;
	}

	result = ALLOCATE(locks, Atomic32, __func__,
			  &lock_counter->physical_zone_counts);
	if (result != VDO_SUCCESS) {
		free_lock_counter(&lock_counter);
		return result;
	}

	result = initialize_enqueueable_completion(&lock_counter->completion,
						   LOCK_COUNTER_COMPLETION,
						   layer);
	if (result != VDO_SUCCESS) {
		free_lock_counter(&lock_counter);
		return result;
	}

	set_callback_with_parent(&lock_counter->completion, callback, threadID,
			         parent);
	lock_counter->logical_zones = logical_zones;
	lock_counter->physical_zones = physical_zones;
	lock_counter->locks = locks;
	*lock_counter_ptr = lock_counter;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_lock_counter(struct lock_counter **lock_counter_ptr)
{
	if (*lock_counter_ptr == NULL) {
		return;
	}

	struct lock_counter *lock_counter = *lock_counter_ptr;
	destroy_enqueueable(&lock_counter->completion);
	freeVolatile(lock_counter->physical_zone_counts);
	freeVolatile(lock_counter->logical_zone_counts);
	freeVolatile(lock_counter->journal_decrement_counts);
	FREE(lock_counter->journal_counters);
	FREE(lock_counter->logical_counters);
	FREE(lock_counter->physical_counters);
	FREE(lock_counter);
	*lock_counter_ptr = NULL;
}

/**
 * Get a pointer to the zone count for a given lock on a given zone.
 *
 * @param counter      The lock counter
 * @param lock_number  The lock to get
 * @param zone_type    The zone type whose count is desired
 *
 * @return A pointer to the zone count for the given lock and zone
 **/
static inline Atomic32 *get_zone_count_ptr(struct lock_counter *counter,
					   BlockCount lock_number,
					   ZoneType zone_type)
{
	return ((zone_type == ZONE_TYPE_LOGICAL)
			? &counter->logical_zone_counts[lock_number]
			: &counter->physical_zone_counts[lock_number]);
}

/**
 * Get the zone counter for a given lock on a given zone.
 *
 * @param counter      The lock counter
 * @param lock_number  The lock to get
 * @param zone_type    The zone type whose count is desired
 * @param zone_id      The zone index whose count is desired
 *
 * @return The counter for the given lock and zone
 **/
static inline uint16_t *get_counter(struct lock_counter *counter,
				    BlockCount lock_number, ZoneType zone_type,
				    ZoneCount zone_id)
{
	BlockCount zone_counter = (counter->locks * zone_id) + lock_number;
	if (zone_type == ZONE_TYPE_JOURNAL) {
		return &counter->journal_counters[zone_counter];
	}

	if (zone_type == ZONE_TYPE_LOGICAL) {
		return &counter->logical_counters[zone_counter];
	}

	return &counter->physical_counters[zone_counter];
}

/**
 * Check whether the journal zone is locked for a given lock.
 *
 * @param counter      The lock_counter
 * @param lock_number  The lock to check
 *
 * @return <code>true</code> if the journal zone is locked
 **/
static bool is_journal_zone_locked(struct lock_counter *counter,
				   BlockCount lock_number)
{
	uint16_t journal_value =
		*(get_counter(counter, lock_number, ZONE_TYPE_JOURNAL, 0));
	uint32_t decrements =
		atomicLoad32(&(counter->journal_decrement_counts[lock_number]));
	ASSERT_LOG_ONLY((decrements <= journal_value),
			"journal zone lock counter must not underflow");

	return (journal_value != decrements);
}

/**********************************************************************/
bool is_locked(struct lock_counter *lock_counter, BlockCount lock_number,
	       ZoneType zone_type)
{
	ASSERT_LOG_ONLY((zone_type != ZONE_TYPE_JOURNAL),
			"is_locked() called for non-journal zone");
	return (is_journal_zone_locked(lock_counter, lock_number)
		|| (atomicLoad32(get_zone_count_ptr(lock_counter,
						    lock_number, zone_type))
		    != 0));
}

/**
 * Check that we are on the journal thread.
 *
 * @param counter  The lock_counter
 * @param caller   The name of the caller (for logging)
 **/
static void assert_on_journal_thread(struct lock_counter *counter,
				     const char *caller)
{
	ASSERT_LOG_ONLY((getCallbackThreadID() ==
			 counter->completion.callbackThreadID),
			"%s() called from journal zone", caller);
}

/**********************************************************************/
void initialize_lock_count(struct lock_counter *counter, BlockCount lock_number,
			   uint16_t value)
{
	assert_on_journal_thread(counter, __func__);
	uint16_t *journal_value =
		get_counter(counter, lock_number, ZONE_TYPE_JOURNAL, 0);
	Atomic32 *decrement_count =
		&(counter->journal_decrement_counts[lock_number]);
	ASSERT_LOG_ONLY((*journal_value == atomicLoad32(decrement_count)),
			"count to be initialized not in use");

	*journal_value = value;
	atomicStore32(decrement_count, 0);
}

/**********************************************************************/
void acquire_lock_count_reference(struct lock_counter *counter,
				  BlockCount lock_number, ZoneType zone_type,
				  ZoneCount zone_id)
{
	ASSERT_LOG_ONLY((zone_type != ZONE_TYPE_JOURNAL),
			"invalid lock count increment from journal zone");

	uint16_t *current_value =
		get_counter(counter, lock_number, zone_type, zone_id);
	ASSERT_LOG_ONLY(*current_value < UINT16_MAX,
			"increment of lock counter must not overflow");

	if (*current_value == 0) {
		// This zone is acquiring this lock for the first time.
		atomicAdd32(get_zone_count_ptr(counter, lock_number,
					       zone_type), 1);
	}
	*current_value += 1;
}

/**
 * Decrement a non-atomic counter.
 *
 * @param counter      The lock_counter
 * @param lock_number  Which lock to decrement
 * @param zone_type    The type of the zone releasing the reference
 * @param zone_id      The ID of the zone releasing the reference
 *
 * @return The new value of the counter
 **/
static uint16_t release_reference(struct lock_counter *counter,
				  BlockCount lock_number, ZoneType zone_type,
				  ZoneCount zone_id)
{
	uint16_t *current_value =
		get_counter(counter, lock_number, zone_type, zone_id);
	ASSERT_LOG_ONLY((*current_value >= 1),
			"decrement of lock counter must not underflow");

	*current_value -= 1;
	return *current_value;
}

/**
 * Attempt to notify the owner of this lock_counter that some lock has been
 * released for some zone type. Will do nothing if another notification is
 * already in progress.
 *
 * @param counter  The lock_counter
 **/
static void attempt_notification(struct lock_counter *counter)
{
	if (compareAndSwapBool(&counter->notifying, false, true)) {
		reset_completion(&counter->completion);
		invoke_callback(&counter->completion);
	}
}

/**********************************************************************/
void release_lock_count_reference(struct lock_counter *counter,
				  BlockCount lock_number, ZoneType zone_type,
				  ZoneCount zone_id)
{
	ASSERT_LOG_ONLY((zone_type != ZONE_TYPE_JOURNAL),
			"invalid lock count decrement from journal zone");
	if (release_reference(counter, lock_number, zone_type, zone_id) != 0) {
		return;
	}

	if (atomicAdd32(get_zone_count_ptr(counter, lock_number, zone_type), -1)
	    == 0) {
		// This zone was the last lock holder of its type, so try to
		// notify the owner.
		attempt_notification(counter);
	}
}

/**********************************************************************/
void release_journal_zone_reference(struct lock_counter *counter,
				    BlockCount lock_number)
{
	assert_on_journal_thread(counter, __func__);
	release_reference(counter, lock_number, ZONE_TYPE_JOURNAL, 0);
	if (!is_journal_zone_locked(counter, lock_number)) {
		// The journal zone is not locked, so try to notify the owner.
		attempt_notification(counter);
	}
}

/**********************************************************************/
void
release_journal_zone_reference_from_other_zone(struct lock_counter *counter,
					       BlockCount lock_number)
{
	atomicAdd32(&(counter->journal_decrement_counts[lock_number]), 1);
}

/**********************************************************************/
void acknowledge_unlock(struct lock_counter *counter)
{
	atomicStoreBool(&counter->notifying, false);
}
