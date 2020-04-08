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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/indexLayoutLinuxKernel.c#4 $
 */

#include "indexLayout.h"
#include "indexLayoutParser.h"
#include "memoryAlloc.h"

/*****************************************************************************/
int make_index_layout(const char *name,
		      bool new_layout,
		      const UdsConfiguration config,
		      struct index_layout **layout_ptr)
{
	char *dev = NULL;
	uint64_t offset = 0;
	uint64_t size = 0;

	LayoutParameter parameter_table[] = {
		{ "dev", LP_STRING | LP_DEFAULT, { .str = &dev } },
		{ "offset", LP_UINT64, { .num = &offset } },
		{ "size", LP_UINT64, { .num = &size } },
	};
	size_t num_parameters =
		sizeof(parameter_table) / sizeof(*parameter_table);

	char *params = NULL;
	int result =
		duplicateString(name, "make_index_layout parameters", &params);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// note dev will be set to memory owned by params
	result = parseLayoutString(params, parameter_table, num_parameters);
	if (result != UDS_SUCCESS) {
		FREE(params);
		return result;
	}

	IOFactory *factory = NULL;
	result = makeIOFactory(dev, &factory);
	FREE(params);
	if (result != UDS_SUCCESS) {
		return result;
	}
	struct index_layout *layout;
	result = make_index_layout_from_factory(
		factory, offset, size, new_layout, config, &layout);
	putIOFactory(factory);
	if (result != UDS_SUCCESS) {
		return result;
	}
	*layout_ptr = layout;
	return UDS_SUCCESS;
}
