/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
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
/*
 *  CFUnicodeDecomposition.h
 *  CoreFoundation
 *
 *  Created by aki on Wed Oct 03 2001.
 *  Copyright (c) 2001-2003, Apple Inc. All rights reserved.
 *
 */

#if !defined(__COREFOUNDATION_CFUNICODEDECOMPOSITION__)
#define __COREFOUNDATION_CFUNICODEDECOMPOSITION__ 1

#if !defined(KERNEL)
#define KERNEL 0
#endif

#if KERNEL
#include "CFKernelTypes.h"
#else // KERNEL
#include "CFUniChar.h"
#endif /* KERNEL */

#if defined(__cplusplus)
extern "C" {
#endif

#if !KERNEL
CF_INLINE bool CFUniCharIsDecomposableCharacter(UTF32Char character, bool isHFSPlusCanonical) {
    if (isHFSPlusCanonical && !isHFSPlusCanonical) return false;	// hack to get rid of "unused" warning
    if (character < 0x80) return false;
    return CFUniCharIsMemberOf(character, kCFUniCharHFSPlusDecomposableCharacterSet);
}
#endif /* !KERNEL */

CF_EXPORT uint32_t CFUniCharDecomposeCharacter(UTF32Char character, UTF32Char *convertedChars, uint32_t maxBufferLength);
#if !KERNEL
CF_EXPORT uint32_t CFUniCharCompatibilityDecompose(UTF32Char *convertedChars, uint32_t length, uint32_t maxBufferLength);
#endif /* !KERNEL */

CF_EXPORT bool CFUniCharDecompose(const UTF16Char *src, uint32_t length, uint32_t *consumedLength, void *dst, uint32_t maxLength, uint32_t *filledLength, bool needToReorder, uint32_t dstFormat, bool isHFSPlus);

CF_EXPORT void CFUniCharPrioritySort(UTF32Char *characters, uint32_t length);

#if defined(__cplusplus)
}
#endif

#endif /* ! __COREFOUNDATION_CFUNICODEDECOMPOSITION__ */

