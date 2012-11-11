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
/*	CFWindowsMessageQueue.h
	Copyright (c) 1999-2003, Apple, Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_CFWINDOWSMESSAGEQUEUE__)
#define __COREFOUNDATION_CFWINDOWSMESSAGEQUEUE__ 1

#if defined(__WIN32__)

#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFRunLoop.h>
#include <windows.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct __CFWindowsMessageQueue * CFWindowsMessageQueueRef;

CF_EXPORT CFTypeID	CFWindowsMessageQueueGetTypeID(void);

CF_EXPORT CFWindowsMessageQueueRef	CFWindowsMessageQueueCreate(CFAllocatorRef allocator, DWORD mask);

CF_EXPORT DWORD		CFWindowsMessageQueueGetMask(CFWindowsMessageQueueRef wmq);
CF_EXPORT void		CFWindowsMessageQueueInvalidate(CFWindowsMessageQueueRef wmq);
CF_EXPORT Boolean	CFWindowsMessageQueueIsValid(CFWindowsMessageQueueRef wmq);

CF_EXPORT CFRunLoopSourceRef	CFWindowsMessageQueueCreateRunLoopSource(CFAllocatorRef allocator, CFWindowsMessageQueueRef wmq, CFIndex order);

#if defined(__cplusplus)
}
#endif

#endif /* __WIN32__ */

#endif /* ! __COREFOUNDATION_CFWINDOWSMESSAGEQUEUE__ */

