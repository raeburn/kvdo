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
 * $Id: //eng/uds-releases/krusty/src/uds/cachedChapterIndex.c#3 $
 */

#include "cachedChapterIndex.h"

#include "memoryAlloc.h"

/**********************************************************************/
int initialize_cached_chapter_index(struct cached_chapter_index *chapter,
				    const Geometry *geometry)
{
	chapter->virtual_chapter = UINT64_MAX;
	chapter->index_pages_count = geometry->indexPagesPerChapter;

	int result = ALLOCATE(chapter->index_pages_count,
			      DeltaIndexPage,
			      __func__,
			      &chapter->index_pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ALLOCATE(chapter->index_pages_count,
			  struct volume_page,
			  "sparse index VolumePages",
			  &chapter->volume_pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	unsigned int i;
	for (i = 0; i < chapter->index_pages_count; i++) {
		result = initializeVolumePage(geometry,
					      &chapter->volume_pages[i]);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
void destroy_cached_chapter_index(struct cached_chapter_index *chapter)
{
	if (chapter->volume_pages != NULL) {
		unsigned int i;
		for (i = 0; i < chapter->index_pages_count; i++) {
			destroyVolumePage(&chapter->volume_pages[i]);
		}
	}
	FREE(chapter->index_pages);
	FREE(chapter->volume_pages);
}

/**********************************************************************/
int cache_chapter_index(struct cached_chapter_index *chapter,
			uint64_t virtual_chapter,
			const Volume *volume)
{
	// Mark the cached chapter as unused in case the update fails midway.
	chapter->virtual_chapter = UINT64_MAX;

	// Read all the page data and initialize the entire DeltaIndexPage
	// array. (It's not safe for the zone threads to do it lazily--they'll
	// race.)
	int result = readChapterIndexFromVolume(volume,
						virtual_chapter,
						chapter->volume_pages,
						chapter->index_pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Reset all chapter counter values to zero.
	chapter->counters.search_hits = 0;
	chapter->counters.search_misses = 0;
	chapter->counters.consecutive_misses = 0;

	// Mark the entry as valid--it's now in the cache.
	chapter->virtual_chapter = virtual_chapter;
	chapter->skip_search = false;

	return UDS_SUCCESS;
}

/**********************************************************************/
int search_cached_chapter_index(struct cached_chapter_index *chapter,
				const Geometry *geometry,
				const IndexPageMap *index_page_map,
				const UdsChunkName *name,
				int *record_page_ptr)
{
	// Find the index_page_number in the chapter that would have the chunk
	// name.
	unsigned int physical_chapter =
		mapToPhysicalChapter(geometry, chapter->virtual_chapter);
	unsigned int index_page_number;
	int result = findIndexPageNumber(
		index_page_map, name, physical_chapter, &index_page_number);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return searchChapterIndexPage(&chapter->index_pages[index_page_number],
				      geometry,
				      name,
				      record_page_ptr);
}
