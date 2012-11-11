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
/*	CFWindowsMessageQueue.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#if defined(__WIN32__)

#include "CFWindowsMessageQueue.h"
#include "CFInternal.h"

extern unsigned long __CFRunLoopGetWindowsMessageQueueMask(CFRunLoopRef rl, CFStringRef mode);
extern void __CFRunLoopSetWindowsMessageQueueMask(CFRunLoopRef rl, unsigned long mask, CFStringRef mode);

struct __CFWindowsMessageQueue {
    CFRuntimeBase _base;
    CFAllocatorRef _allocator;
    CFSpinLock_t _lock;
    DWORD _mask;
    CFRunLoopSourceRef _source;
    CFMutableArrayRef _runLoops;
};

/* Bit 3 in the base reserved bits is used for invalid state */

CF_INLINE Boolean __CFWindowsMessageQueueIsValid(CFWindowsMessageQueueRef wmq) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)wmq)->_info, 3, 3);
}

CF_INLINE void __CFWindowsMessageQueueSetValid(CFWindowsMessageQueueRef wmq) {
    __CFBitfieldSetValue(((CFRuntimeBase *)wmq)->_info, 3, 3, 1);
}

CF_INLINE void __CFWindowsMessageQueueUnsetValid(CFWindowsMessageQueueRef wmq) {
    __CFBitfieldSetValue(((CFRuntimeBase *)wmq)->_info, 3, 3, 0);
}

CF_INLINE void __CFWindowsMessageQueueLock(CFWindowsMessageQueueRef wmq) {
    __CFSpinLock(&(wmq->_lock));
}

CF_INLINE void __CFWindowsMessageQueueUnlock(CFWindowsMessageQueueRef wmq) {
    __CFSpinUnlock(&(wmq->_lock));
}

CFTypeID CFWindowsMessageQueueGetTypeID(void) {
    return __kCFWindowsMessageQueueTypeID;
}

Boolean __CFWindowsMessageQueueEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFWindowsMessageQueueRef wmq1 = (CFWindowsMessageQueueRef)cf1;
    CFWindowsMessageQueueRef wmq2 = (CFWindowsMessageQueueRef)cf2;
    return (wmq1 == wmq2);
}

CFHashCode __CFWindowsMessageQueueHash(CFTypeRef cf) {
    CFWindowsMessageQueueRef wmq = (CFWindowsMessageQueueRef)cf;
    return (CFHashCode)wmq;
}

CFStringRef __CFWindowsMessageQueueCopyDescription(CFTypeRef cf) {
#warning CF: this and many other CopyDescription functions are probably
#warning CF: broken, in that some of these fields being printed out can
#warning CF: be NULL, when the object is in the invalid state
    CFWindowsMessageQueueRef wmq = (CFWindowsMessageQueueRef)cf;
    CFMutableStringRef result;
    result = CFStringCreateMutable(CFGetAllocator(wmq), 0);
    __CFWindowsMessageQueueLock(wmq);
#warning CF: here, and probably everywhere with a per-instance lock,
#warning CF: the locked state will always be true because we lock,
#warning CF: and you cannot call description if the object is locked;
#warning CF: probably should not lock description, and call it unsafe
    CFStringAppendFormat(result, NULL, CFSTR("<CFWindowsMessageQueue 0x%x [0x%x]>{locked = %s, valid = %s, mask = 0x%x,\n    run loops = %@}"), (UInt32)cf, (UInt32)CFGetAllocator(wmq), (wmq->_lock ? "Yes" : "No"), (__CFWindowsMessageQueueIsValid(wmq) ? "Yes" : "No"), (UInt32)wmq->_mask, wmq->_runLoops);
    __CFWindowsMessageQueueUnlock(wmq);
    return result;
}

CFAllocatorRef __CFWindowsMessageQueueGetAllocator(CFTypeRef cf) {
    CFWindowsMessageQueueRef wmq = (CFWindowsMessageQueueRef)cf;
    return wmq->_allocator;
}

void __CFWindowsMessageQueueDeallocate(CFTypeRef cf) {
    CFWindowsMessageQueueRef wmq = (CFWindowsMessageQueueRef)cf;
    CFAllocatorRef allocator = CFGetAllocator(wmq);
    CFAllocatorDeallocate(allocator, wmq);
    CFRelease(allocator);
}

CFWindowsMessageQueueRef CFWindowsMessageQueueCreate(CFAllocatorRef allocator, DWORD mask) {
    CFWindowsMessageQueueRef memory;
    UInt32 size;
    size = sizeof(struct __CFWindowsMessageQueue);
    allocator = (NULL == allocator) ? CFRetain(__CFGetDefaultAllocator()) : CFRetain(allocator);
    memory = CFAllocatorAllocate(allocator, size, 0);
    if (NULL == memory) {
	CFRelease(allocator);
	return NULL;
    }
    __CFGenericInitBase(memory, NULL, __kCFWindowsMessageQueueTypeID);
    memory->_allocator = allocator;
    __CFWindowsMessageQueueSetValid(memory);
    memory->_lock = 0;
    memory->_mask = mask;
    memory->_source = NULL;
    memory->_runLoops = CFArrayCreateMutable(allocator, 0, NULL);
    return memory;
}

void CFWindowsMessageQueueInvalidate(CFWindowsMessageQueueRef wmq) {
    __CFGenericValidateType(wmq, __kCFWindowsMessageQueueTypeID);
    CFRetain(wmq);
    __CFWindowsMessageQueueLock(wmq);
    if (__CFWindowsMessageQueueIsValid(wmq)) {
	SInt32 idx;
	__CFWindowsMessageQueueUnsetValid(wmq);
	for (idx = CFArrayGetCount(wmq->_runLoops); idx--;) {
	    CFRunLoopWakeUp((CFRunLoopRef)CFArrayGetValueAtIndex(wmq->_runLoops, idx));
	}
	CFRelease(wmq->_runLoops);
	wmq->_runLoops = NULL;
	if (NULL != wmq->_source) {
	    CFRunLoopSourceInvalidate(wmq->_source);
	    CFRelease(wmq->_source);
	    wmq->_source = NULL;
	}
    }
    __CFWindowsMessageQueueUnlock(wmq);
    CFRelease(wmq);
}

Boolean CFWindowsMessageQueueIsValid(CFWindowsMessageQueueRef wmq) {
    __CFGenericValidateType(wmq, __kCFWindowsMessageQueueTypeID);
    return __CFWindowsMessageQueueIsValid(wmq);
}

DWORD CFWindowsMessageQueueGetMask(CFWindowsMessageQueueRef wmq) {
    __CFGenericValidateType(wmq, __kCFWindowsMessageQueueTypeID);
    return wmq->_mask;
}

static void __CFWindowsMessageQueueSchedule(void *info, CFRunLoopRef rl, CFStringRef mode) {
    CFWindowsMessageQueueRef wmq = info;
    __CFWindowsMessageQueueLock(wmq);
    if (__CFWindowsMessageQueueIsValid(wmq)) {
	unsigned long mask;
	CFArrayAppendValue(wmq->_runLoops, rl);
	mask = __CFRunLoopGetWindowsMessageQueueMask(rl, mode);
	mask |= wmq->_mask;
	__CFRunLoopSetWindowsMessageQueueMask(rl, mask, mode);
    }
    __CFWindowsMessageQueueUnlock(wmq);
}

static void __CFWindowsMessageQueueCancel(void *info, CFRunLoopRef rl, CFStringRef mode) {
    CFWindowsMessageQueueRef wmq = info;
    __CFWindowsMessageQueueLock(wmq);
#warning CF: should fix up run loop modes mask here, if not done
#warning CF: previously by the invalidation, where it should also
#warning CF: be done
    if (NULL != wmq->_runLoops) {
	SInt32 idx = CFArrayGetFirstIndexOfValue(wmq->_runLoops, CFRangeMake(0, CFArrayGetCount(wmq->_runLoops)), rl);
	if (0 <= idx) CFArrayRemoveValueAtIndex(wmq->_runLoops, idx);
    }
    __CFWindowsMessageQueueUnlock(wmq);
}

static void __CFWindowsMessageQueuePerform(void *info) {
    CFWindowsMessageQueueRef wmq = info;
    MSG msg;
    __CFWindowsMessageQueueLock(wmq);
    if (!__CFWindowsMessageQueueIsValid(wmq)) {
	__CFWindowsMessageQueueUnlock(wmq);
	return;
    }
    __CFWindowsMessageQueueUnlock(wmq);
    if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE | PM_NOYIELD)) {
	TranslateMessage(&msg);
	DispatchMessage(&msg);
    }
}

CFRunLoopSourceRef CFWindowsMessageQueueCreateRunLoopSource(CFAllocatorRef allocator, CFWindowsMessageQueueRef wmq, CFIndex order) {
    CFRunLoopSourceRef result = NULL;
    __CFWindowsMessageQueueLock(wmq);
    if (NULL == wmq->_source) {
	CFRunLoopSourceContext context;
	context.version = 0;
	context.info = (void *)wmq;
	context.retain = (const void *(*)(const void *))CFRetain;
	context.release = (void (*)(const void *))CFRelease;
	context.copyDescription = (CFStringRef (*)(const void *))__CFWindowsMessageQueueCopyDescription;
	context.equal = (Boolean (*)(const void *, const void *))__CFWindowsMessageQueueEqual;
	context.hash = (CFHashCode (*)(const void *))__CFWindowsMessageQueueHash;
	context.schedule = __CFWindowsMessageQueueSchedule;
	context.cancel = __CFWindowsMessageQueueCancel;
	context.perform = __CFWindowsMessageQueuePerform;
	wmq->_source = CFRunLoopSourceCreate(allocator, order, &context);
    }
    CFRetain(wmq->_source);	/* This retain is for the receiver */
    result = wmq->_source;
    __CFWindowsMessageQueueUnlock(wmq);
    return result;
}

#endif

