/*
 * Copyright (c) 2008 Apple Inc. All rights reserved.
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
	Copyright 2005-2007, Apple Inc. All rights reserved.
*/

#if !defined(AUTO_STUBS_H)
#define AUTO_STUBS_H 1

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <malloc/malloc.h>
#include <objc/objc.h>

/* Stubs for functions in libauto. */

typedef malloc_zone_t auto_zone_t;

enum { AUTO_TYPE_UNKNOWN = -1, AUTO_UNSCANNED = 1, AUTO_OBJECT = 2, AUTO_MEMORY_SCANNED = 0, AUTO_MEMORY_UNSCANNED = AUTO_UNSCANNED, AUTO_OBJECT_SCANNED = AUTO_OBJECT, AUTO_OBJECT_UNSCANNED = AUTO_OBJECT | AUTO_UNSCANNED };
typedef unsigned long auto_memory_type_t;

CF_INLINE void *auto_zone(void) { return 0; }
CF_INLINE void *auto_zone_allocate_object(void *zone, size_t size, auto_memory_type_t type, boolean_t rc, boolean_t clear) { return 0; }
CF_INLINE const void *auto_zone_base_pointer(void *zone, const void *ptr) { return 0; }
CF_INLINE void auto_zone_retain(void *zone, void *ptr) {}
CF_INLINE unsigned int auto_zone_release(void *zone, void *ptr) { return 0; }
CF_INLINE unsigned int auto_zone_retain_count(void *zone, const void *ptr) { return 0; }
CF_INLINE void auto_zone_set_layout_type(void *zone, void *ptr, auto_memory_type_t type) {}
CF_INLINE void auto_zone_write_barrier_range(void *zone, void *address, size_t size) {}
CF_INLINE boolean_t auto_zone_is_finalized(void *zone, const void *ptr) { return 0; }
CF_INLINE size_t auto_zone_size(void *zone, const void *ptr) { return 0; }
CF_INLINE void auto_register_weak_reference(void *zone, const void *referent, void **referrer, uintptr_t *counter, void **listHead, void **listElement) {}
CF_INLINE void auto_unregister_weak_reference(void *zone, const void *referent, void **referrer) {}
CF_INLINE void auto_zone_register_thread(void *zone) {}
CF_INLINE void auto_zone_unregister_thread(void *zone) {}
CF_INLINE boolean_t auto_zone_is_valid_pointer(void *zone, const void *ptr) { return 0; }
CF_INLINE auto_memory_type_t auto_zone_get_layout_type(auto_zone_t *zone, void *ptr) { return AUTO_UNSCANNED; }

CF_INLINE void objc_collect_if_needed(unsigned long options) {}
CF_INLINE BOOL objc_collecting_enabled(void) { return 0; }
CF_INLINE id objc_allocate_object(Class cls, int extra) { return 0; }
CF_INLINE id objc_assign_strongCast(id val, id *dest) { return (*dest = val); }
CF_INLINE id objc_assign_global(id val, id *dest) { return (*dest = val); }
CF_INLINE id objc_assign_ivar(id val, id dest, unsigned int offset) { id *d = (id *)((char *)dest + offset); return (*d = val); }
CF_INLINE void *objc_memmove_collectable(void *dst, const void *src, size_t size) { return memmove(dst, src, size); }
CF_INLINE BOOL objc_is_finalized(void *ptr) { return 0; }

#endif /* ! AUTO_STUBS_H */

