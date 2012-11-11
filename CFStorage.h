/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
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

/*	CFStorage.h
	Copyright (c) 1999-2009, Apple Inc. All rights reserved.
*/
/*!
        @header CFStorage
CFStorage stores an array of arbitrary-sized values. There are no callbacks;
all that is provided about the values is the size, and the appropriate number
of bytes are copied in and out of the CFStorage.

CFStorage uses a balanced tree to store the values, and is most appropriate
for situations where potentially a large number values (more than a hundred
bytes' worth) will be stored and there will be a lot of editing (insertions and deletions).

Getting to an item is O(log n), although caching the last result often reduces this
to a constant time.

The overhead of CFStorage is 48 bytes. There is no per item overhead; the 
non-leaf nodes in the tree cost 20 bytes each, and the worst case extra
capacity (unused space in the leaves) is 12%, typically much less.

Because CFStorage does not necessarily use a single block of memory to store the values,
when you ask for a value, you get back the pointer to the value and optionally
the range of other values that are consecutive and thus reachable as if the
storage was a single block.
*/

#if !defined(__COREFOUNDATION_CFSTORAGE__)
#define __COREFOUNDATION_CFSTORAGE__ 1

#include <CoreFoundation/CFBase.h>

CF_EXTERN_C_BEGIN

/*!
        @typedef CFStorageRef
	This is the type of a reference to a CFStorage instance.
*/
typedef struct __CFStorage *CFStorageRef;

/*!
	@typedef CFStorageApplierFunction
	Type of the callback function used by the apply functions of
		CFStorage.
	@param value The current value from the storage.
	@param context The user-defined context parameter given to the apply
		function.
*/
typedef void (*CFStorageApplierFunction)(const void *val, void *context);

/*!
        @function CFStorageGetTypeID
        Returns the type identifier of all CFStorage instances.
*/
CF_EXPORT CFTypeID CFStorageGetTypeID(void);

/*!
        @function CFStorageCreate
        Creates a new mutable storage with elements of the given size.
	@param alloc The CFAllocator which should be used to allocate
		memory for the set and its storage for values. This
		parameter may be NULL in which case the current default
		CFAllocator is used. If this reference is not a valid
		CFAllocator, the behavior is undefined.
	@param valueSizeInBytes The size in bytes of each of the elements 
                to be stored in the storage.  If this value is zero or
                negative, the result is undefined.
	@result A reference to the new CFStorage instance.
*/
CF_EXPORT CFStorageRef CFStorageCreate(CFAllocatorRef alloc, CFIndex valueSizeInBytes);

/*!
	@function CFStorageInsertValues
	Allocates space for range.length values at location range.location.  Use
        CFStorageReplaceValues() to set those values.
	@param storage The storage to which the values are to be inserted.
                If this parameter is not a valid CFStorage, the behavior is undefined.
	@param range The range of values within the storage to delete. If the
		range location or end point (defined by the location plus
		length minus 1) are outside the index space of the storage (0
		to N inclusive, where N is the count of the storage), the
		behavior is undefined. If the range length is negative, the
		behavior is undefined. The range may be empty (length 0),
		in which case the no values are inserted.
*/
CF_EXPORT void CFStorageInsertValues(CFStorageRef storage, CFRange range);

/*!
	@function CFStorageDeleteValues
	Deletes the values of the storage in the specified range.
	@param storage The storage from which the values are to be deleted.
                If this parameter is not a valid CFStorage, the behavior is undefined.
	@param range The range of values within the storage to delete. If the
		range location or end point (defined by the location plus
		length minus 1) are outside the index space of the storage (0
		to N inclusive, where N is the count of the storage), the
		behavior is undefined. If the range length is negative, the
		behavior is undefined. The range may be empty (length 0),
		in which case the no values are deleted.
*/
CF_EXPORT void CFStorageDeleteValues(CFStorageRef storage, CFRange range);

/*!
	@function CFStorageGetCount
	Returns the number of values currently in the storage.
	@param storage The storage to be queried. If this parameter is not a valid
		CFStorage, the behavior is undefined.
	@result The number of values in the storage.
*/
CF_EXPORT CFIndex CFStorageGetCount(CFStorageRef storage);

/*!
        @function CFStorageGetValueAtIndex
        Returns a pointer to the specified value.  The pointer is mutable and may be used to
        get or set the value.
	@param storage The storage to be queried. If this parameter is not a
		valid CFStorage, the behavior is undefined.
	@param idx The index of the value to retrieve. If the index is
		outside the index space of the storage (0 to N-1 inclusive,
		where N is the count of the storage), the behavior is
		undefined.
        @param validConsecutiveValueRange This parameter is a C pointer to a CFRange.
                If NULL is specified, this argument is ignored; otherwise, the range
                is set to the range of values that may be accessed via an offset from the result pointer.
                The range location is set to the index of the lowest consecutive
                value and the range length is set to the count of consecutive values.
	@result The value with the given index in the storage.
*/
CF_EXPORT void *CFStorageGetValueAtIndex(CFStorageRef storage, CFIndex idx, CFRange *validConsecutiveValueRange);

/*!
        @function CFStorageGetValues
	Fills the buffer with values from the storage.
	@param storage The storage to be queried. If this parameter is not a
		valid CFStorage, the behavior is undefined.
	@param range The range of values within the storage to retrieve. If
		the range location or end point (defined by the location
		plus length minus 1) are outside the index space of the
		storage (0 to N-1 inclusive, where N is the count of the
		storage), the behavior is undefined. If the range length is
		negative, the behavior is undefined. The range may be empty
		(length 0), in which case no values are put into the buffer.
	@param values A C array of to be filled with values from the storage. 
                The values in the C array are ordered
		in the same order in which they appear in the storage. If this
		parameter is not a valid pointer to a C array of at least
		range.length pointers, the behavior is undefined.
*/
CF_EXPORT void CFStorageGetValues(CFStorageRef storage, CFRange range, void *values);

/*!
	@function CFStorageApplyFunction
	Calls a function once for each value in the set.
	@param storage The storage to be operated upon. If this parameter is not
		a valid CFStorage, the behavior is undefined.
	@param range The range of values within the storage to operate on. If the
		range location or end point (defined by the location plus
		length minus 1) are outside the index space of the storage (0
		to N inclusive, where N is the count of the storage), the
		behavior is undefined. If the range length is negative, the
		behavior is undefined. The range may be empty (length 0),
		in which case the no values are operated on.
	@param applier The callback function to call once for each value in
		the given storage. If this parameter is not a
		pointer to a function of the correct prototype, the behavior
		is undefined. If there are values in the storage which the
		applier function does not expect or cannot properly apply
		to, the behavior is undefined. 
	@param context A pointer-sized user-defined value, which is passed
		as the second parameter to the applier function, but is
		otherwise unused by this function. If the context is not
		what is expected by the applier function, the behavior is
		undefined.
*/
CF_EXPORT void CFStorageApplyFunction(CFStorageRef storage, CFRange range, CFStorageApplierFunction applier, void *context);

/*!
        @function CFStorageReplaceValues
	Replaces a range of values in the storage.
	@param storage The storage from which the specified values are to be
		removed. If this parameter is not a valid CFStorage,
		the behavior is undefined.
	@param range The range of values within the storage to replace. If the
		range location or end point (defined by the location plus
		length minus 1) are outside the index space of the storage (0
		to N inclusive, where N is the count of the storage), the
		behavior is undefined. If the range length is negative, the
		behavior is undefined. The range may be empty (length 0),
		in which case the new values are merely inserted at the
		range location.
	@param values A C array of the values to be copied into the storage. 
                The new values in the storage are ordered in the same order 
                in which they appear in this C array. This parameter may be NULL 
                if the range length is 0.  This C array is not changed or freed by
                this function. If this parameter is not a valid pointer to a C array of at least
		range length pointers, the behavior is undefined.
*/
CF_EXPORT void CFStorageReplaceValues(CFStorageRef storage, CFRange range, const void *values);

/* Private stuff...
*/
CF_EXPORT CFIndex __CFStorageGetCapacity(CFStorageRef storage);
CF_EXPORT CFIndex __CFStorageGetValueSize(CFStorageRef storage);


CF_EXTERN_C_END

#endif /* ! __COREFOUNDATION_CFSTORAGE__ */

