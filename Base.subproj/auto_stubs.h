/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*	auto_stubs.h
	Copyright 2005, Apple, Inc. All rights reserved.
*/


#include <stdlib.h>
#include <malloc/malloc.h>

/* Stubs for functions in libauto used by CoreFoundation. */

typedef malloc_zone_t auto_zone_t;
typedef enum { AUTO_OBJECT_SCANNED, AUTO_OBJECT_UNSCANNED, AUTO_MEMORY_SCANNED, AUTO_MEMORY_UNSCANNED } auto_memory_type_t;

CF_INLINE auto_zone_t *auto_zone(void) { return NULL; }
CF_INLINE void* auto_zone_allocate_object(auto_zone_t *zone, size_t size, auto_memory_type_t type, boolean_t rc, boolean_t clear) { return NULL; }
CF_INLINE const void *auto_zone_base_pointer(auto_zone_t *zone, const void *ptr) { return NULL; }
CF_INLINE void auto_zone_retain(auto_zone_t *zone, void *ptr) {}
CF_INLINE unsigned int auto_zone_release(auto_zone_t *zone, void *ptr) { return 0; }
CF_INLINE unsigned int auto_zone_retain_count(auto_zone_t *zone, const void *ptr) { return 0; }
CF_INLINE void auto_zone_set_layout_type(auto_zone_t *zone, void *ptr, auto_memory_type_t type) {}
CF_INLINE void auto_zone_write_barrier_range(auto_zone_t *zone, void *address, size_t size) {}

