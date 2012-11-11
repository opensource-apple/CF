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
/*	CFUtilitiesPriv.h
	Copyright (c) 1998-2005, Apple, Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_CFUTILITIES__)
#define __COREFOUNDATION_CFUTILITIES__ 1

#include <CoreFoundation/CFBase.h>

#if defined(__cplusplus)
extern "C" {
#endif

CF_EXPORT uint32_t CFLog2(uint64_t x);

CF_EXPORT void CFMergeSortArray(void *list, CFIndex count, CFIndex elementSize, CFComparatorFunction comparator, void *context);
CF_EXPORT void CFQSortArray(void *list, CFIndex count, CFIndex elementSize, CFComparatorFunction comparator, void *context);

/* _CFExecutableLinkedOnOrAfter(releaseVersionName) will return YES if the current executable seems to be linked on or after the specified release. Example: If you specify CFSystemVersionPuma (10.1), you will get back true for executables linked on Puma or Jaguar(10.2), but false for those linked on Cheetah (10.0) or any of its software updates (10.0.x). You will also get back false for any app whose version info could not be figured out.
    This function caches its results, so no need to cache at call sites.

  Note that for non-MACH this function always returns true.
*/
typedef enum {
    CFSystemVersionCheetah = 0,		/* 10.0 */
    CFSystemVersionPuma = 1,		/* 10.1 */
    CFSystemVersionJaguar = 2,          /* 10.2 */
    CFSystemVersionPanther = 3,         /* 10.3 */
    CFSystemVersionPinot = 3,           /* Deprecated name for Panther */
    CFSystemVersionTiger = 4,           /* 10.4 */
    CFSystemVersionMerlot = 4,          /* Deprecated name for Tiger */
    CFSystemVersionChablis = 5,         /* Post-Tiger */
    CFSystemVersionMax                  /* This should bump up when new entries are added */
} CFSystemVersion;

CF_EXPORT Boolean _CFExecutableLinkedOnOrAfter(CFSystemVersion version);
    


#if defined(__cplusplus)
}
#endif

#endif /* ! __COREFOUNDATION_CFUTILITIES__ */

