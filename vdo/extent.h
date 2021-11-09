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
 */

#ifndef EXTENT_H
#define EXTENT_H

#include "completion.h"
#include "types.h"
#include "vio.h"

/**
 * A chain of vios which are part of the same request. An extent contains
 * a chain of at least 'count' vios. The 'next' pointer of the last vio
 * in the extent (as indicated by the count) may not be NULL, but it is not
 * part of the extent. A vio may belong to a single extent.
 **/
struct vdo_extent {
	/* The completion for asynchronous extent processing */
	struct vdo_completion completion;
	/* The number of vios in the extent */
	block_count_t count;
	/* The number of completed vios in the extent */
	block_count_t complete_count;
	/* The vios in the extent */
	struct vio *vios[];
};

/**
 * Convert a generic vdo_completion to a vdo_extent.
 *
 * @param completion The completion to convert
 *
 * @return The completion as an extent
 **/
static inline struct vdo_extent *
vdo_completion_as_extent(struct vdo_completion *completion)
{
	assert_vdo_completion_type(completion->type, VDO_EXTENT_COMPLETION);
	return container_of(completion, struct vdo_extent, completion);
}

/**
 * Convert a vdo_extent to a vdo_completion.
 *
 * @param extent The extent to convert
 *
 * @return The extent as a vdo_completion
 **/
static inline struct vdo_completion *
vdo_extent_as_completion(struct vdo_extent *extent)
{
	return &extent->completion;
}

int __must_check create_vdo_extent(struct vdo *vdo,
				   enum vio_type vio_type,
				   enum vio_priority priority,
				   block_count_t block_count,
				   char *data,
				   struct vdo_extent **extent_ptr);

void free_vdo_extent(struct vdo_extent *extent);

void read_partial_vdo_metadata_extent(struct vdo_extent *extent,
				      physical_block_number_t start_block,
				      block_count_t count);

/**
 * Read metadata from the underlying storage.
 *
 * @param extent       The extent to read
 * @param start_block  The physical block number of the first block
 *                     in the extent
 **/
static inline void vdo_read_metadata_extent(struct vdo_extent *extent,
					    physical_block_number_t start_block)
{
	read_partial_vdo_metadata_extent(extent, start_block, extent->count);
}

void write_partial_vdo_metadata_extent(struct vdo_extent *extent,
				       physical_block_number_t start_block,
				       block_count_t count);
/**
 * Write metadata to the underlying storage.
 *
 * @param extent       The extent to write
 * @param start_block  The physical block number of the first block in the
 *                     extent
 **/

static inline void vdo_write_metadata_extent(struct vdo_extent *extent,
					     physical_block_number_t start_block)
{
	write_partial_vdo_metadata_extent(extent, start_block, extent->count);
}

#endif /* EXTENT_H */
