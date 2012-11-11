/*
 * Copyright (c) 2009 Apple Inc. All rights reserved.
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
/*	CFWindowsMessageQueue.h
	Copyright (c) 1999-2009, Apple Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_CFWINDOWSMESSAGEQUEUE__)
#define __COREFOUNDATION_CFWINDOWSMESSAGEQUEUE__ 1

#if DEPLOYMENT_TARGET_WINDOWS

#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFRunLoop.h>


typedef struct __CFWindowsMessageQueue * CFWindowsMessageQueueRef;

CF_EXPORT CFTypeID	CFWindowsMessageQueueGetTypeID(void);

CF_EXPORT CFWindowsMessageQueueRef	CFWindowsMessageQueueCreate(CFAllocatorRef allocator, uint32_t /* DWORD */ mask);

CF_EXPORT uint32_t	CFWindowsMessageQueueGetMask(CFWindowsMessageQueueRef wmq);
CF_EXPORT void		CFWindowsMessageQueueInvalidate(CFWindowsMessageQueueRef wmq);
CF_EXPORT Boolean	CFWindowsMessageQueueIsValid(CFWindowsMessageQueueRef wmq);

CF_EXPORT CFRunLoopSourceRef	CFWindowsMessageQueueCreateRunLoopSource(CFAllocatorRef allocator, CFWindowsMessageQueueRef wmq, CFIndex order);

#endif

#endif /* ! __COREFOUNDATION_CFWINDOWSMESSAGEQUEUE__ */

