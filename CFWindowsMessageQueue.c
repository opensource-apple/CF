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
/*	CFWindowsMessageQueue.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#if defined(__WIN32__)

#include "CFWindowsMessageQueue.h"
#include "CFInternal.h"

extern DWORD __CFRunLoopGetWindowsMessageQueueMask(CFRunLoopRef rl, CFStringRef mode);
extern void __CFRunLoopSetWindowsMessageQueueMask(CFRunLoopRef rl, DWORD mask, CFStringRef mode);

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
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)wmq)->_cfinfo[CF_INFO_BITS], 3, 3);
}

CF_INLINE void __CFWindowsMessageQueueSetValid(CFWindowsMessageQueueRef wmq) {
    __CFBitfieldSetValue(((CFRuntimeBase *)wmq)->_cfinfo[CF_INFO_BITS], 3, 3, 1);
}

CF_INLINE void __CFWindowsMessageQueueUnsetValid(CFWindowsMessageQueueRef wmq) {
    __CFBitfieldSetValue(((CFRuntimeBase *)wmq)->_cfinfo[CF_INFO_BITS], 3, 3, 0);
}

CF_INLINE void __CFWindowsMessageQueueLock(CFWindowsMessageQueueRef wmq) {
    __CFSpinLock(&(wmq->_lock));
}

CF_INLINE void __CFWindowsMessageQueueUnlock(CFWindowsMessageQueueRef wmq) {
    __CFSpinUnlock(&(wmq->_lock));
}

static Boolean __CFWindowsMessageQueueEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFWindowsMessageQueueRef wmq1 = (CFWindowsMessageQueueRef)cf1;
    CFWindowsMessageQueueRef wmq2 = (CFWindowsMessageQueueRef)cf2;
    return (wmq1 == wmq2);
}

static CFHashCode __CFWindowsMessageQueueHash(CFTypeRef cf) {
    CFWindowsMessageQueueRef wmq = (CFWindowsMessageQueueRef)cf;
    return (CFHashCode)wmq;
}

static CFStringRef __CFWindowsMessageQueueCopyDescription(CFTypeRef cf) {
/* Some commentary, possibly as out of date as much of the rest of the file was
#warning CF: this and many other CopyDescription functions are probably
#warning CF: broken, in that some of these fields being printed out can
#warning CF: be NULL, when the object is in the invalid state
*/
    CFWindowsMessageQueueRef wmq = (CFWindowsMessageQueueRef)cf;
    CFMutableStringRef result;
    result = CFStringCreateMutable(CFGetAllocator(wmq), 0);
    __CFWindowsMessageQueueLock(wmq);
/* More commentary, which we don't really need to see with every build
#warning CF: here, and probably everywhere with a per-instance lock,
#warning CF: the locked state will always be true because we lock,
#warning CF: and you cannot call description if the object is locked;
#warning CF: probably should not lock description, and call it unsafe
*/
    CFStringAppendFormat(result, NULL, CFSTR("<CFWindowsMessageQueue %p [%p]>{locked = %s, valid = %s, mask = 0x%x,\n    run loops = %@}"), cf, CFGetAllocator(wmq), (wmq->_lock.LockCount ? "Yes" : "No"), (__CFWindowsMessageQueueIsValid(wmq) ? "Yes" : "No"), (UInt32)wmq->_mask, wmq->_runLoops);
    __CFWindowsMessageQueueUnlock(wmq);
    return result;
}

CFAllocatorRef __CFWindowsMessageQueueGetAllocator(CFTypeRef cf) {
    CFWindowsMessageQueueRef wmq = (CFWindowsMessageQueueRef)cf;
    return wmq->_allocator;
}

static void __CFWindowsMessageQueueDeallocate(CFTypeRef cf) {
    CFWindowsMessageQueueRef wmq = (CFWindowsMessageQueueRef)cf;
    CFAllocatorRef allocator = CFGetAllocator(wmq);
    CFAllocatorDeallocate(allocator, wmq);
    CFRelease(allocator);
    DeleteCriticalSection(&(wmq->_lock));
}

static CFTypeID __kCFWindowsMessageQueueTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFWindowsMessageQueueClass = {
    0,
    "CFWindowsMessageQueue",
    NULL,	// init
    NULL,	// copy
    __CFWindowsMessageQueueDeallocate,
    __CFWindowsMessageQueueEqual,
    __CFWindowsMessageQueueHash,
    NULL,	//
    __CFWindowsMessageQueueCopyDescription
};

__private_extern__ void __CFWindowsMessageQueueInitialize(void) {
    __kCFWindowsMessageQueueTypeID = _CFRuntimeRegisterClass(&__CFWindowsMessageQueueClass);
}

CFTypeID CFWindowsMessageQueueGetTypeID(void) {
    return __kCFWindowsMessageQueueTypeID;
}

CFWindowsMessageQueueRef CFWindowsMessageQueueCreate(CFAllocatorRef allocator, DWORD mask) {
    CFWindowsMessageQueueRef memory;
    UInt32 size = sizeof(struct __CFWindowsMessageQueue) - sizeof(CFRuntimeBase);
    memory = (CFWindowsMessageQueueRef)_CFRuntimeCreateInstance(allocator, __kCFWindowsMessageQueueTypeID, size, NULL);
    if (NULL == memory) {
        return NULL;
    }
    __CFWindowsMessageQueueSetValid(memory);

    CF_SPINLOCK_INIT_FOR_STRUCTS(memory->_lock);
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
    CFWindowsMessageQueueRef wmq = (CFWindowsMessageQueueRef)info;
    __CFWindowsMessageQueueLock(wmq);
    if (__CFWindowsMessageQueueIsValid(wmq)) {
	uint32_t mask;
	CFArrayAppendValue(wmq->_runLoops, rl);
	mask = __CFRunLoopGetWindowsMessageQueueMask(rl, mode);
	mask |= wmq->_mask;
	__CFRunLoopSetWindowsMessageQueueMask(rl, mask, mode);
    }
    __CFWindowsMessageQueueUnlock(wmq);
}

static void __CFWindowsMessageQueueCancel(void *info, CFRunLoopRef rl, CFStringRef mode) {
    CFWindowsMessageQueueRef wmq = (CFWindowsMessageQueueRef)info;
    __CFWindowsMessageQueueLock(wmq);
#if defined (__WIN32__)
//#warning CF: should fix up run loop modes mask here, if not done
//#warning CF: previously by the invalidation, where it should also
//#warning CF: be done
#else
#warning CF: should fix up run loop modes mask here, if not done
#warning CF: previously by the invalidation, where it should also
#warning CF: be done
#endif //__WIN32__
    if (NULL != wmq->_runLoops) {
	SInt32 idx = CFArrayGetFirstIndexOfValue(wmq->_runLoops, CFRangeMake(0, CFArrayGetCount(wmq->_runLoops)), rl);
	if (0 <= idx) CFArrayRemoveValueAtIndex(wmq->_runLoops, idx);
    }
    __CFWindowsMessageQueueUnlock(wmq);
}

static void __CFWindowsMessageQueuePerform(void *info) {
    CFWindowsMessageQueueRef wmq = (CFWindowsMessageQueueRef)info;
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
	context.retain = CFRetain;
	context.release = CFRelease;
	context.copyDescription = __CFWindowsMessageQueueCopyDescription;
	context.equal = __CFWindowsMessageQueueEqual;
	context.hash = __CFWindowsMessageQueueHash;
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

