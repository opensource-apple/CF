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
/*	CFRunLoop.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFSet.h>
#include <CoreFoundation/CFBag.h>
#include "CFInternal.h"
#include <math.h>
#include <limits.h>
#if defined(__MACH__)
#include <mach/mach.h>
#include <mach/clock_types.h>
#include <mach/clock.h>
#else
#include <windows.h>
#endif

extern bool CFDictionaryGetKeyIfPresent(CFDictionaryRef dict, const void *key, const void **actualkey);

#if defined(__MACH__)
extern mach_port_name_t mk_timer_create(void);
extern kern_return_t mk_timer_destroy(mach_port_name_t name);
extern kern_return_t mk_timer_arm(mach_port_name_t name, AbsoluteTime expire_time);
extern kern_return_t mk_timer_cancel(mach_port_name_t name, AbsoluteTime *result_time);

CF_INLINE AbsoluteTime __CFUInt64ToAbsoluteTime(int64_t x) {
    AbsoluteTime a;
    a.hi = x >> 32;
    a.lo = x & (int64_t)0xFFFFFFFF;
    return a;
}
#endif

#if defined(__MACH__)
static uint32_t __CFSendTrivialMachMessage(mach_port_t port, uint32_t msg_id, CFOptionFlags options, uint32_t timeout) {
    kern_return_t result;
    mach_msg_header_t header;
    header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    header.msgh_size = sizeof(mach_msg_header_t);
    header.msgh_remote_port = port;
    header.msgh_local_port = MACH_PORT_NULL;
    header.msgh_id = msg_id;
    result = mach_msg(&header, MACH_SEND_MSG|options, header.msgh_size, 0, MACH_PORT_NULL, timeout, MACH_PORT_NULL);
    return result;
}

static kern_return_t __CFClearPortSet(mach_port_t task, mach_port_t portSet) {
    kern_return_t ret;
    mach_port_name_array_t array;
    mach_msg_type_number_t idx, number;

    ret = mach_port_get_set_status(task, portSet, &array, &number);
    if (KERN_SUCCESS != ret) return ret;
    for (idx = 0; idx < number; idx++) {
	ret = mach_port_extract_member(task, array[idx], portSet);
	if (KERN_SUCCESS != ret) {
	    vm_deallocate(task, (vm_address_t)array, number * sizeof(mach_port_name_t));
	    return ret;
	}
    }
    vm_deallocate(task, (vm_address_t)array, number * sizeof(mach_port_name_t));
    return KERN_SUCCESS;
}
#endif

/* unlock a run loop and modes before doing callouts/sleeping */
/* never try to take the run loop lock with a mode locked */
/* be very careful of common subexpression elimination and compacting code, particular across locks and unlocks! */
/* run loop mode structures should never be deallocated, even if they become empty */

static CFTypeID __kCFRunLoopModeTypeID = _kCFRuntimeNotATypeID;
static CFTypeID __kCFRunLoopTypeID = _kCFRuntimeNotATypeID;
static CFTypeID __kCFRunLoopSourceTypeID = _kCFRuntimeNotATypeID;
static CFTypeID __kCFRunLoopObserverTypeID = _kCFRuntimeNotATypeID;
static CFTypeID __kCFRunLoopTimerTypeID = _kCFRuntimeNotATypeID;

typedef struct __CFRunLoopMode *CFRunLoopModeRef;

struct __CFRunLoopMode {
    CFRuntimeBase _base;
    CFSpinLock_t _lock;	/* must have the run loop locked before locking this */
    CFStringRef _name;
    Boolean _stopped;
    char _padding[3];
    CFMutableSetRef _sources;
    CFMutableSetRef _observers;
    CFMutableSetRef _timers;
    CFMutableArrayRef _submodes; // names of the submodes
#if defined(__MACH__)
    mach_port_t _portSet;
#endif
#if defined(__WIN32__)
    DWORD _msgQMask;
#endif
};

CF_INLINE void __CFRunLoopModeLock(CFRunLoopModeRef rlm) {
    __CFSpinLock(&(rlm->_lock));
}

CF_INLINE void __CFRunLoopModeUnlock(CFRunLoopModeRef rlm) {
    __CFSpinUnlock(&(rlm->_lock));
}

static Boolean __CFRunLoopModeEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFRunLoopModeRef rlm1 = (CFRunLoopModeRef)cf1;
    CFRunLoopModeRef rlm2 = (CFRunLoopModeRef)cf2;
    return CFEqual(rlm1->_name, rlm2->_name);
}

static CFHashCode __CFRunLoopModeHash(CFTypeRef cf) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)cf;
    return CFHash(rlm->_name);
}

static CFStringRef __CFRunLoopModeCopyDescription(CFTypeRef cf) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)cf;
    CFMutableStringRef result;
    result = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    CFStringAppendFormat(result, NULL, CFSTR("<CFRunLoopMode %p [%p]>{name = %@, locked = %s, "), rlm, CFGetAllocator(rlm), rlm->_name, rlm->_lock ? "true" : "false");
#if defined(__MACH__)
    CFStringAppendFormat(result, NULL, CFSTR("port set = %p,"), rlm->_portSet);
#endif
#if defined(__WIN32__)
    CFStringAppendFormat(result, NULL, CFSTR("MSGQ mask = %p,"), rlm->_msgQMask);
#endif
    CFStringAppendFormat(result, NULL, CFSTR("\n\tsources = %@,\n\tobservers == %@,\n\ttimers = %@\n},\n"), rlm->_sources, rlm->_observers, rlm->_timers);
    return result;
}

static void __CFRunLoopModeDeallocate(CFTypeRef cf) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)cf;
    if (NULL != rlm->_sources) CFRelease(rlm->_sources);
    if (NULL != rlm->_observers) CFRelease(rlm->_observers);
    if (NULL != rlm->_timers) CFRelease(rlm->_timers);
    if (NULL != rlm->_submodes) CFRelease(rlm->_submodes);
    CFRelease(rlm->_name);
#if defined(__MACH__)
    __CFClearPortSet(mach_task_self(), rlm->_portSet);
    mach_port_destroy(mach_task_self(), rlm->_portSet);
#endif
}

struct __CFRunLoop {
    CFRuntimeBase _base;
    CFSpinLock_t _lock;			/* locked for accessing mode list */
#if defined(__MACH__)
    mach_port_t _waitPort;
#endif
#if defined(__WIN32__)
    HANDLE _waitPort;
#endif
    volatile CFIndex *_stopped;
    CFMutableSetRef _commonModes;
    CFMutableSetRef _commonModeItems;
    CFRunLoopModeRef _currentMode;
    CFMutableSetRef _modes;
};

/* Bit 0 of the base reserved bits is used for stopped state */
/* Bit 1 of the base reserved bits is used for sleeping state */
/* Bit 2 of the base reserved bits is used for deallocating state */

CF_INLINE Boolean __CFRunLoopIsStopped(CFRunLoopRef rl) {
    return (rl->_stopped && rl->_stopped[2]) ? true : false;
}

CF_INLINE void __CFRunLoopSetStopped(CFRunLoopRef rl) {
    if (rl->_stopped) rl->_stopped[2] = 0x53544F50;	// 'STOP'
}

CF_INLINE void __CFRunLoopUnsetStopped(CFRunLoopRef rl) {
    if (rl->_stopped) rl->_stopped[2] = 0x0;
}

CF_INLINE Boolean __CFRunLoopIsSleeping(CFRunLoopRef rl) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rl)->_info, 1, 1);
}

CF_INLINE void __CFRunLoopSetSleeping(CFRunLoopRef rl) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rl)->_info, 1, 1, 1);
}

CF_INLINE void __CFRunLoopUnsetSleeping(CFRunLoopRef rl) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rl)->_info, 1, 1, 0);
}

CF_INLINE Boolean __CFRunLoopIsDeallocating(CFRunLoopRef rl) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rl)->_info, 2, 2);
}

CF_INLINE void __CFRunLoopSetDeallocating(CFRunLoopRef rl) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rl)->_info, 2, 2, 1);
}

CF_INLINE void __CFRunLoopLock(CFRunLoopRef rl) {
    __CFSpinLock(&(((CFRunLoopRef)rl)->_lock));
}

CF_INLINE void __CFRunLoopUnlock(CFRunLoopRef rl) {
    __CFSpinUnlock(&(((CFRunLoopRef)rl)->_lock));
}

static CFStringRef __CFRunLoopCopyDescription(CFTypeRef cf) {
    CFRunLoopRef rl = (CFRunLoopRef)cf;
    CFMutableStringRef result;
    result = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    CFStringAppendFormat(result, NULL, CFSTR("<CFRunLoop %p [%p]>{locked = %s, wait port = 0x%x, stopped = %s,\ncurrent mode = %@,\n"), cf, CFGetAllocator(cf), rl->_lock ? "true" : "false", rl->_waitPort, (rl->_stopped && *(rl->_stopped)) ? "true" : "false", rl->_currentMode ? rl->_currentMode->_name : CFSTR("(none)"));
    CFStringAppendFormat(result, NULL, CFSTR("common modes = %@,\ncommon mode items = %@,\nmodes = %@}\n"), rl->_commonModes, rl->_commonModeItems, rl->_modes);
    return result;
}

/* call with rl locked */
static CFRunLoopModeRef __CFRunLoopFindMode(CFRunLoopRef rl, CFStringRef modeName, Boolean create) {
    CFRunLoopModeRef rlm;
    struct __CFRunLoopMode srlm;
    srlm._base._isa = __CFISAForTypeID(__kCFRunLoopModeTypeID);
    srlm._base._info = 0;
    _CFRuntimeSetInstanceTypeID(&srlm, __kCFRunLoopModeTypeID);
    srlm._name = modeName;
    rlm = (CFRunLoopModeRef)CFSetGetValue(rl->_modes, &srlm);
    if (NULL != rlm) {
	__CFRunLoopModeLock(rlm);
	return rlm;
    }
    if (!create) {
	return NULL;
    }
    rlm = (CFRunLoopModeRef)_CFRuntimeCreateInstance(CFGetAllocator(rl), __kCFRunLoopModeTypeID, sizeof(struct __CFRunLoopMode) - sizeof(CFRuntimeBase), NULL);
    if (NULL == rlm) {
	return NULL;
    }
    rlm->_lock = 0;
    rlm->_name = CFStringCreateCopy(CFGetAllocator(rlm), modeName);
    rlm->_stopped = false;
    rlm->_sources = NULL;
    rlm->_observers = NULL;
    rlm->_timers = NULL;
    rlm->_submodes = NULL;
#if defined(__MACH__)
    {
	kern_return_t ret;
	ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &(rlm->_portSet));
	if (KERN_SUCCESS == ret) {
	    ret = mach_port_insert_member(mach_task_self(), rl->_waitPort, rlm->_portSet);
	}
	if (KERN_SUCCESS != ret) HALT;
    }
#endif
#if defined(__WIN32__)
    rlm->_msgQMask = 0;
#endif
    CFSetAddValue(rl->_modes, rlm);
    CFRelease(rlm);
    __CFRunLoopModeLock(rlm);	/* return mode locked */
    return rlm;
}

#if defined(__WIN32__)

// expects rl and rlm locked
static Boolean __CFRunLoopModeIsEmpty(CFRunLoopRef rl, CFRunLoopModeRef rlm) {
    if (NULL == rlm) return true;
    if (0 != rlm->_msgQMask) return false;
    if (NULL != rlm->_sources && 0 < CFSetGetCount(rlm->_sources)) return false;
    if (NULL != rlm->_timers && 0 < CFSetGetCount(rlm->_timers)) return false;
    if (NULL != rlm->_submodes) {
	CFIndex idx, cnt;
	for (idx = 0, cnt = CFArrayGetCount(rlm->_submodes); idx < cnt; idx++) {
	    CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
	    CFRunLoopModeRef subrlm;
	    Boolean subIsEmpty;
	    subrlm = __CFRunLoopFindMode(rl, modeName, false);
	    subIsEmpty = (NULL != subrlm) ? __CFRunLoopModeIsEmpty(rl, subrlm) : true;
	    if (NULL != subrlm) __CFRunLoopModeUnlock(subrlm);
	    if (!subIsEmpty) return false;
	}
    }
    return true;
}

DWORD __CFRunLoopGetWindowsMessageQueueMask(CFRunLoopRef rl, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    DWORD result = 0;
    __CFRunLoopLock(rl);
    rlm = __CFRunLoopFindMode(rl, modeName, false);
    if (rlm) {
	result = rlm->_msgQMask;
	__CFRunLoopModeUnlock(rlm);
    }
    __CFRunLoopUnlock(rl);
    return result;
}

void __CFRunLoopSetWindowsMessageQueueMask(CFRunLoopRef rl, DWORD mask, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    rlm = __CFRunLoopFindMode(rl, modeName, true);
    rlm->_msgQMask = mask;
    __CFRunLoopModeUnlock(rlm);
    __CFRunLoopUnlock(rl);
}

#else

// expects rl and rlm locked
static Boolean __CFRunLoopModeIsEmpty(CFRunLoopRef rl, CFRunLoopModeRef rlm) {
    if (NULL == rlm) return true;
    if (NULL != rlm->_sources && 0 < CFSetGetCount(rlm->_sources)) return false;
    if (NULL != rlm->_timers && 0 < CFSetGetCount(rlm->_timers)) return false;
    if (NULL != rlm->_submodes) {
	CFIndex idx, cnt;
	for (idx = 0, cnt = CFArrayGetCount(rlm->_submodes); idx < cnt; idx++) {
	    CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
	    CFRunLoopModeRef subrlm;
	    Boolean subIsEmpty;
	    subrlm = __CFRunLoopFindMode(rl, modeName, false);
	    subIsEmpty = (NULL != subrlm) ? __CFRunLoopModeIsEmpty(rl, subrlm) : true;
	    if (NULL != subrlm) __CFRunLoopModeUnlock(subrlm);
	    if (!subIsEmpty) return false;
	}
    }
    return true;
}

#endif

/* Bit 3 in the base reserved bits is used for invalid state in run loop objects */

CF_INLINE Boolean __CFIsValid(const void *cf) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)cf)->_info, 3, 3);
}

CF_INLINE void __CFSetValid(void *cf) {
    __CFBitfieldSetValue(((CFRuntimeBase *)cf)->_info, 3, 3, 1);
}

CF_INLINE void __CFUnsetValid(void *cf) {
    __CFBitfieldSetValue(((CFRuntimeBase *)cf)->_info, 3, 3, 0);
}

struct __CFRunLoopSource {
    CFRuntimeBase _base;
    uint32_t _bits;
    CFSpinLock_t _lock;
    CFIndex _order;			/* immutable */
    CFMutableBagRef _runLoops;
    union {
	CFRunLoopSourceContext version0;	/* immutable, except invalidation */
#if defined(__MACH__)
	CFRunLoopSourceContext1 version1;	/* immutable, except invalidation */
#endif
    } _context;
};

/* Bit 1 of the base reserved bits is used for signaled state */

CF_INLINE Boolean __CFRunLoopSourceIsSignaled(CFRunLoopSourceRef rls) {
    return (Boolean)__CFBitfieldGetValue(rls->_bits, 1, 1);
}

CF_INLINE void __CFRunLoopSourceSetSignaled(CFRunLoopSourceRef rls) {
    __CFBitfieldSetValue(rls->_bits, 1, 1, 1);
}

CF_INLINE void __CFRunLoopSourceUnsetSignaled(CFRunLoopSourceRef rls) {
    __CFBitfieldSetValue(rls->_bits, 1, 1, 0);
}

CF_INLINE void __CFRunLoopSourceLock(CFRunLoopSourceRef rls) {
    __CFSpinLock(&(rls->_lock));
}

CF_INLINE void __CFRunLoopSourceUnlock(CFRunLoopSourceRef rls) {
    __CFSpinUnlock(&(rls->_lock));
}

/* rlm is not locked */
static void __CFRunLoopSourceSchedule(CFRunLoopSourceRef rls, CFRunLoopRef rl, CFRunLoopModeRef rlm) {	/* DOES CALLOUT */
    __CFRunLoopSourceLock(rls);
    if (NULL == rls->_runLoops) {
	rls->_runLoops = CFBagCreateMutable(CFGetAllocator(rls), 0, NULL);
    }
    CFBagAddValue(rls->_runLoops, rl);
    __CFRunLoopSourceUnlock(rls);	// have to unlock before the callout -- cannot help clients with safety
    if (0 == rls->_context.version0.version) {
	if (NULL != rls->_context.version0.schedule) {
	    rls->_context.version0.schedule(rls->_context.version0.info, rl, rlm->_name);
	}
#if defined(__MACH__)
    } else if (1 == rls->_context.version0.version) {
	mach_port_t port;
	port = rls->_context.version1.getPort(rls->_context.version1.info);
	if (MACH_PORT_NULL != port) {
	    mach_port_insert_member(mach_task_self(), port, rlm->_portSet);
	}
#endif
    }
}

/* rlm is not locked */
static void __CFRunLoopSourceCancel(CFRunLoopSourceRef rls, CFRunLoopRef rl, CFRunLoopModeRef rlm) {	/* DOES CALLOUT */
    if (0 == rls->_context.version0.version) {
	if (NULL != rls->_context.version0.cancel) {
	    rls->_context.version0.cancel(rls->_context.version0.info, rl, rlm->_name);	/* CALLOUT */
	}
#if defined(__MACH__)
    } else if (1 == rls->_context.version0.version) {
        mach_port_t port;
        port = rls->_context.version1.getPort(rls->_context.version1.info);	/* CALLOUT */
        if (MACH_PORT_NULL != port) {
	    mach_port_extract_member(mach_task_self(), port, rlm->_portSet);
	}
#endif
    }
    __CFRunLoopSourceLock(rls);
    if (NULL != rls->_runLoops) {
        CFBagRemoveValue(rls->_runLoops, rl);
    }
    __CFRunLoopSourceUnlock(rls);
}

struct __CFRunLoopObserver {
    CFRuntimeBase _base;
    CFSpinLock_t _lock;
    CFRunLoopRef _runLoop;
    CFIndex _rlCount;
    CFOptionFlags _activities;		/* immutable */
    CFIndex _order;			/* immutable */
    CFRunLoopObserverCallBack _callout;	/* immutable */
    CFRunLoopObserverContext _context;	/* immutable, except invalidation */
};

/* Bit 0 of the base reserved bits is used for firing state */
/* Bit 1 of the base reserved bits is used for repeats state */

CF_INLINE Boolean __CFRunLoopObserverIsFiring(CFRunLoopObserverRef rlo) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rlo)->_info, 0, 0);
}

CF_INLINE void __CFRunLoopObserverSetFiring(CFRunLoopObserverRef rlo) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlo)->_info, 0, 0, 1);
}

CF_INLINE void __CFRunLoopObserverUnsetFiring(CFRunLoopObserverRef rlo) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlo)->_info, 0, 0, 0);
}

CF_INLINE Boolean __CFRunLoopObserverRepeats(CFRunLoopObserverRef rlo) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rlo)->_info, 1, 1);
}

CF_INLINE void __CFRunLoopObserverSetRepeats(CFRunLoopObserverRef rlo) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlo)->_info, 1, 1, 1);
}

CF_INLINE void __CFRunLoopObserverUnsetRepeats(CFRunLoopObserverRef rlo) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlo)->_info, 1, 1, 0);
}

CF_INLINE void __CFRunLoopObserverLock(CFRunLoopObserverRef rlo) {
    __CFSpinLock(&(rlo->_lock));
}

CF_INLINE void __CFRunLoopObserverUnlock(CFRunLoopObserverRef rlo) {
    __CFSpinUnlock(&(rlo->_lock));
}

static void __CFRunLoopObserverSchedule(CFRunLoopObserverRef rlo, CFRunLoopRef rl, CFRunLoopModeRef rlm) {
    __CFRunLoopObserverLock(rlo);
    if (0 == rlo->_rlCount) {
	rlo->_runLoop = rl;
    }
    rlo->_rlCount++;
    __CFRunLoopObserverUnlock(rlo);
}

static void __CFRunLoopObserverCancel(CFRunLoopObserverRef rlo, CFRunLoopRef rl, CFRunLoopModeRef rlm) {
    __CFRunLoopObserverLock(rlo);
    rlo->_rlCount--;
    if (0 == rlo->_rlCount) {
	rlo->_runLoop = NULL;
    }
    __CFRunLoopObserverUnlock(rlo);
}

struct __CFRunLoopTimer {
    CFRuntimeBase _base;
    CFSpinLock_t _lock;
    CFRunLoopRef _runLoop;
    CFIndex _rlCount;
#if defined(__MACH__)
    mach_port_name_t _port;
#endif
    CFIndex _order;			/* immutable */
    int64_t _fireTSR;			/* TSR units */
    int64_t _intervalTSR;		/* immutable; 0 means non-repeating; TSR units */
    CFRunLoopTimerCallBack _callout;	/* immutable */
    CFRunLoopTimerContext _context;	/* immutable, except invalidation */
};

/* Bit 0 of the base reserved bits is used for firing state */
/* Bit 1 of the base reserved bits is used for has-reset state */

CF_INLINE Boolean __CFRunLoopTimerIsFiring(CFRunLoopTimerRef rlt) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rlt)->_info, 0, 0);
}

CF_INLINE void __CFRunLoopTimerSetFiring(CFRunLoopTimerRef rlt) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlt)->_info, 0, 0, 1);
}

CF_INLINE void __CFRunLoopTimerUnsetFiring(CFRunLoopTimerRef rlt) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlt)->_info, 0, 0, 0);
}

CF_INLINE void __CFRunLoopTimerLock(CFRunLoopTimerRef rlt) {
    __CFSpinLock(&(rlt->_lock));
}

CF_INLINE void __CFRunLoopTimerUnlock(CFRunLoopTimerRef rlt) {
    __CFSpinUnlock(&(rlt->_lock));
}

static CFSpinLock_t __CFRLTFireTSRLock = 0;

CF_INLINE void __CFRunLoopTimerFireTSRLock(void) {
    __CFSpinLock(&__CFRLTFireTSRLock);
}

CF_INLINE void __CFRunLoopTimerFireTSRUnlock(void) {
    __CFSpinUnlock(&__CFRLTFireTSRLock);
}

static CFMutableDictionaryRef __CFRLTPortMap = NULL;
static CFSpinLock_t __CFRLTPortMapLock = 0;

CF_INLINE void __CFRunLoopTimerPortMapLock(void) {
    __CFSpinLock(&__CFRLTPortMapLock);
}

CF_INLINE void __CFRunLoopTimerPortMapUnlock(void) {
    __CFSpinUnlock(&__CFRLTPortMapLock);
}

static void __CFRunLoopTimerSchedule(CFRunLoopTimerRef rlt, CFRunLoopRef rl, CFRunLoopModeRef rlm) {
#if defined(__MACH__)
    __CFRunLoopTimerLock(rlt);
    if (0 == rlt->_rlCount) {
	rlt->_runLoop = rl;
	if (MACH_PORT_NULL == rlt->_port) {
	    rlt->_port = mk_timer_create();
	}
	__CFRunLoopTimerPortMapLock();
	if (NULL == __CFRLTPortMap) {
	    __CFRLTPortMap = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, NULL);
	}
	CFDictionarySetValue(__CFRLTPortMap, (void *)rlt->_port, rlt);
	__CFRunLoopTimerPortMapUnlock();
    }
    rlt->_rlCount++;
    mach_port_insert_member(mach_task_self(), rlt->_port, rlm->_portSet);
    mk_timer_arm(rlt->_port, __CFUInt64ToAbsoluteTime(rlt->_fireTSR));
    __CFRunLoopTimerUnlock(rlt);
#endif
}

static void __CFRunLoopTimerCancel(CFRunLoopTimerRef rlt, CFRunLoopRef rl, CFRunLoopModeRef rlm) {
#if defined(__MACH__)
    __CFRunLoopTimerLock(rlt);
    mach_port_extract_member(mach_task_self(), rlt->_port, rlm->_portSet);
    rlt->_rlCount--;
    if (0 == rlt->_rlCount) {
	__CFRunLoopTimerPortMapLock();
	if (NULL != __CFRLTPortMap) {
	    CFDictionaryRemoveValue(__CFRLTPortMap, (void *)rlt->_port);
	}
	__CFRunLoopTimerPortMapUnlock();
	rlt->_runLoop = NULL;
	mk_timer_cancel(rlt->_port, NULL);
    }
    __CFRunLoopTimerUnlock(rlt);
#endif
}

static void __CFRunLoopTimerRescheduleWithAllModes(CFRunLoopTimerRef rlt, CFRunLoopRef rl) {
#if defined(__MACH__)
    mk_timer_arm(rlt->_port, __CFUInt64ToAbsoluteTime(rlt->_fireTSR));
#endif
}


/* CFRunLoop */

CONST_STRING_DECL(kCFRunLoopDefaultMode, "kCFRunLoopDefaultMode")
CONST_STRING_DECL(kCFRunLoopCommonModes, "kCFRunLoopCommonModes")

#if defined(__MACH__)

struct _findsource {
    mach_port_t port;
    CFRunLoopSourceRef result;
};

static void __CFRunLoopFindSource(const void *value, void *ctx) {
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)value;
    struct _findsource *context = (struct _findsource *)ctx;
    mach_port_t port;
    if (NULL != context->result) return;
    if (1 != rls->_context.version0.version) return;
    __CFRunLoopSourceLock(rls);
    port = rls->_context.version1.getPort(rls->_context.version1.info);
    if (port == context->port) {
	context->result = rls;
    }
    __CFRunLoopSourceUnlock(rls);
}

// call with rl and rlm locked
static CFRunLoopSourceRef __CFRunLoopModeFindSourceForMachPort(CFRunLoopRef rl, CFRunLoopModeRef rlm, mach_port_t port) {	/* DOES CALLOUT */
    struct _findsource context = {port, NULL};
    if (NULL != rlm->_sources) {
	CFSetApplyFunction(rlm->_sources, (__CFRunLoopFindSource), &context);
    }
    if (NULL == context.result && NULL != rlm->_submodes) {
	CFIndex idx, cnt;
	for (idx = 0, cnt = CFArrayGetCount(rlm->_submodes); idx < cnt; idx++) {
	    CFRunLoopSourceRef source = NULL;
	    CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
	    CFRunLoopModeRef subrlm;
            subrlm = __CFRunLoopFindMode(rl, modeName, false);
	    if (NULL != subrlm) {
		source = __CFRunLoopModeFindSourceForMachPort(rl, subrlm, port);
		__CFRunLoopModeUnlock(subrlm);
	    }
	    if (NULL != source) {
		context.result = source;
		break;
	    }
	}
    }
    return context.result;
}

// call with rl and rlm locked
static CFRunLoopTimerRef __CFRunLoopModeFindTimerForMachPort(CFRunLoopModeRef rlm, mach_port_name_t port) {
    CFRunLoopTimerRef result = NULL;
    __CFRunLoopTimerPortMapLock();
    if (NULL != __CFRLTPortMap) {
	result = (CFRunLoopTimerRef)CFDictionaryGetValue(__CFRLTPortMap, (void *)port);
    }
    __CFRunLoopTimerPortMapUnlock();
    return result;
}
#endif

static void __CFRunLoopDeallocateSources(const void *value, void *context) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)context;
    CFIndex idx, cnt;
    const void **list, *buffer[256];
    if (NULL == rlm->_sources) return;
    cnt = CFSetGetCount(rlm->_sources);
    list = (cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0);
    CFSetGetValues(rlm->_sources, list);
    for (idx = 0; idx < cnt; idx++) {
	CFRetain(list[idx]);
    }
    CFSetRemoveAllValues(rlm->_sources);
    for (idx = 0; idx < cnt; idx++) {
	__CFRunLoopSourceCancel((CFRunLoopSourceRef)list[idx], rl, rlm);
	CFRelease(list[idx]);
    }
    if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
}

static void __CFRunLoopDeallocateObservers(const void *value, void *context) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)context;
    CFIndex idx, cnt;
    const void **list, *buffer[256];
    if (NULL == rlm->_observers) return;
    cnt = CFSetGetCount(rlm->_observers);
    list = (cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0);
    CFSetGetValues(rlm->_observers, list);
    for (idx = 0; idx < cnt; idx++) {
	CFRetain(list[idx]);
    }
    CFSetRemoveAllValues(rlm->_observers);
    for (idx = 0; idx < cnt; idx++) {
	__CFRunLoopObserverCancel((CFRunLoopObserverRef)list[idx], rl, rlm);
	CFRelease(list[idx]);
    }
    if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
}

static void __CFRunLoopDeallocateTimers(const void *value, void *context) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)context;
    CFIndex idx, cnt;
    const void **list, *buffer[256];
    if (NULL == rlm->_timers) return;
    cnt = CFSetGetCount(rlm->_timers);
    list = (cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0);
    CFSetGetValues(rlm->_timers, list);
    for (idx = 0; idx < cnt; idx++) {
	CFRetain(list[idx]);
    }
    CFSetRemoveAllValues(rlm->_timers);
    for (idx = 0; idx < cnt; idx++) {
	__CFRunLoopTimerCancel((CFRunLoopTimerRef)list[idx], rl, rlm);
	CFRelease(list[idx]);
    }
    if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
}

static void __CFRunLoopDeallocate(CFTypeRef cf) {
    CFRunLoopRef rl = (CFRunLoopRef)cf;
    /* We try to keep the run loop in a valid state as long as possible,
       since sources may have non-retained references to the run loop.
       Another reason is that we don't want to lock the run loop for
       callback reasons, if we can get away without that.  We start by
       eliminating the sources, since they are the most likely to call
       back into the run loop during their "cancellation". Common mode
       items will be removed from the mode indirectly by the following
       three lines. */
    __CFRunLoopSetDeallocating(rl);
    if (NULL != rl->_modes) {
	CFSetApplyFunction(rl->_modes, (__CFRunLoopDeallocateSources), rl);
	CFSetApplyFunction(rl->_modes, (__CFRunLoopDeallocateObservers), rl);
	CFSetApplyFunction(rl->_modes, (__CFRunLoopDeallocateTimers), rl);
    }
    __CFRunLoopLock(rl);
    if (NULL != rl->_commonModeItems) {
	CFRelease(rl->_commonModeItems);
    }
    if (NULL != rl->_commonModes) {
	CFRelease(rl->_commonModes);
    }
    if (NULL != rl->_modes) {
	CFRelease(rl->_modes);
    }
#if defined(__MACH__)
    mach_port_destroy(mach_task_self(), rl->_waitPort);
    rl->_waitPort = 0;
#endif
#if defined(__WIN32__)
    CloseHandle(rl->_waitPort);
    rl->_waitPort = 0;
#endif
    __CFRunLoopUnlock(rl);
}

static const CFRuntimeClass __CFRunLoopModeClass = {
    0,
    "CFRunLoopMode",
    NULL,      // init
    NULL,      // copy
    __CFRunLoopModeDeallocate,
    __CFRunLoopModeEqual,
    __CFRunLoopModeHash,
    NULL,      // 
    __CFRunLoopModeCopyDescription
};

static const CFRuntimeClass __CFRunLoopClass = {
    0,
    "CFRunLoop",
    NULL,      // init
    NULL,      // copy
    __CFRunLoopDeallocate,
    NULL,
    NULL,
    NULL,      // 
    __CFRunLoopCopyDescription
};

__private_extern__ void __CFRunLoopInitialize(void) {
    __kCFRunLoopTypeID = _CFRuntimeRegisterClass(&__CFRunLoopClass);
    __kCFRunLoopModeTypeID = _CFRuntimeRegisterClass(&__CFRunLoopModeClass);
}
 
CFTypeID CFRunLoopGetTypeID(void) {
    return __kCFRunLoopTypeID;
}

static CFRunLoopRef __CFRunLoopCreate(void) {
    CFRunLoopRef loop = NULL;
    CFRunLoopModeRef rlm;
    uint32_t size = sizeof(struct __CFRunLoop) - sizeof(CFRuntimeBase);
    loop = (CFRunLoopRef)_CFRuntimeCreateInstance(kCFAllocatorSystemDefault, __kCFRunLoopTypeID, size, NULL);
    if (NULL == loop) {
	return NULL;
    }
    loop->_stopped = NULL;
    loop->_lock = 0;
#if defined(__MACH__)
    {
	kern_return_t ret;
	ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &(loop->_waitPort));
	if (KERN_SUCCESS == ret) {
	    ret = mach_port_insert_right(mach_task_self(), loop->_waitPort, loop->_waitPort, MACH_MSG_TYPE_MAKE_SEND);
	}
	if (KERN_SUCCESS == ret) {
	    mach_port_limits_t limits;
	    limits.mpl_qlimit = 1;
	    ret = mach_port_set_attributes(mach_task_self(), loop->_waitPort, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT);
	}
	if (KERN_SUCCESS != ret) HALT;
    }
#elif defined(__WIN32__)
    loop->_waitPort = CreateEvent(NULL, true, false, NULL);
#endif
    loop->_commonModes = CFSetCreateMutable(CFGetAllocator(loop), 0, &kCFTypeSetCallBacks);
    CFSetAddValue(loop->_commonModes, kCFRunLoopDefaultMode);
    loop->_commonModeItems = NULL;
    loop->_currentMode = NULL;
    loop->_modes = CFSetCreateMutable(CFGetAllocator(loop), 0, &kCFTypeSetCallBacks);
    _CFSetSetCapacity(loop->_modes, 10);
    rlm = __CFRunLoopFindMode(loop, kCFRunLoopDefaultMode, true);
    if (NULL != rlm) __CFRunLoopModeUnlock(rlm);
    return loop;
}

static CFRunLoopRef mainLoop = NULL;
static int mainLoopPid = 0;
static CFSpinLock_t mainLoopLock = 0;

CFRunLoopRef CFRunLoopGetMain(void) {
    __CFSpinLock(&mainLoopLock);
    if (mainLoopPid != getpid()) {
	// intentionally leak mainLoop so we don't kill any ports in the child
	mainLoop = NULL;
    }
    if (!mainLoop) {
	mainLoop = __CFRunLoopCreate();
	mainLoopPid = getpid();
    }
    __CFSpinUnlock(&mainLoopLock);
    return mainLoop;
}

static void _CFRunLoopSetMain(CFRunLoopRef rl) {
    if (rl != mainLoop) {
        if (rl) CFRetain(rl);
//	intentionally leak the old main run loop
//	if (mainLoop) CFRelease(mainLoop);
	mainLoop = rl;
    }
}

CFRunLoopRef CFRunLoopGetCurrent(void) {
    if (pthread_main_np()) {
	return CFRunLoopGetMain();
    }
    CFRunLoopRef currentLoop = __CFGetThreadSpecificData_inline()->_runLoop;
    int currentLoopPid = __CFGetThreadSpecificData_inline()->_runLoop_pid;
    if (currentLoopPid != getpid()) {
	// intentionally leak currentLoop so we don't kill any ports in the child
	currentLoop = NULL;
    }
    if (!currentLoop) {
	currentLoop = __CFRunLoopCreate();
	__CFGetThreadSpecificData_inline()->_runLoop = currentLoop;
	__CFGetThreadSpecificData_inline()->_runLoop_pid = getpid();
    }
    return currentLoop;
}

void _CFRunLoopSetCurrent(CFRunLoopRef rl) {
    if (pthread_main_np()) {
	return _CFRunLoopSetMain(rl);
    }
    CFRunLoopRef currentLoop = __CFGetThreadSpecificData_inline()->_runLoop;
    if (rl != currentLoop) {
        if (rl) CFRetain(rl);
//	intentionally leak old run loop
//	if (currentLoop) CFRelease(currentLoop);
        __CFGetThreadSpecificData_inline()->_runLoop = rl;
	__CFGetThreadSpecificData_inline()->_runLoop_pid = getpid();
    }
}

CFStringRef CFRunLoopCopyCurrentMode(CFRunLoopRef rl) {
    CFStringRef result = NULL;
    __CFRunLoopLock(rl);
    if (NULL != rl->_currentMode) {
	result = CFRetain(rl->_currentMode->_name);
    }
    __CFRunLoopUnlock(rl);
    return result;
}

static void __CFRunLoopGetModeName(const void *value, void *context) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFMutableArrayRef array = (CFMutableArrayRef)context;
    CFArrayAppendValue(array, rlm->_name);
}

CFArrayRef CFRunLoopCopyAllModes(CFRunLoopRef rl) {
    CFMutableArrayRef array;
    __CFRunLoopLock(rl);
    array = CFArrayCreateMutable(kCFAllocatorDefault, CFSetGetCount(rl->_modes), &kCFTypeArrayCallBacks);
    CFSetApplyFunction(rl->_modes, (__CFRunLoopGetModeName), array);
    __CFRunLoopUnlock(rl);
    return array;
}

static void __CFRunLoopAddItemsToCommonMode(const void *value, void *ctx) {
    CFTypeRef item = (CFTypeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)(((CFTypeRef *)ctx)[0]);
    CFStringRef modeName = (CFStringRef)(((CFTypeRef *)ctx)[1]);
    if (CFGetTypeID(item) == __kCFRunLoopSourceTypeID) {
	CFRunLoopAddSource(rl, (CFRunLoopSourceRef)item, modeName);
    } else if (CFGetTypeID(item) == __kCFRunLoopObserverTypeID) {
	CFRunLoopAddObserver(rl, (CFRunLoopObserverRef)item, modeName);
    } else if (CFGetTypeID(item) == __kCFRunLoopTimerTypeID) {
	CFRunLoopAddTimer(rl, (CFRunLoopTimerRef)item, modeName);
    }
}

static void __CFRunLoopAddItemToCommonModes(const void *value, void *ctx) {
    CFStringRef modeName = (CFStringRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)(((CFTypeRef *)ctx)[0]);
    CFTypeRef item = (CFTypeRef)(((CFTypeRef *)ctx)[1]);
    if (CFGetTypeID(item) == __kCFRunLoopSourceTypeID) {
	CFRunLoopAddSource(rl, (CFRunLoopSourceRef)item, modeName);
    } else if (CFGetTypeID(item) == __kCFRunLoopObserverTypeID) {
	CFRunLoopAddObserver(rl, (CFRunLoopObserverRef)item, modeName);
    } else if (CFGetTypeID(item) == __kCFRunLoopTimerTypeID) {
	CFRunLoopAddTimer(rl, (CFRunLoopTimerRef)item, modeName);
    }
}

static void __CFRunLoopRemoveItemFromCommonModes(const void *value, void *ctx) {
    CFStringRef modeName = (CFStringRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)(((CFTypeRef *)ctx)[0]);
    CFTypeRef item = (CFTypeRef)(((CFTypeRef *)ctx)[1]);
    if (CFGetTypeID(item) == __kCFRunLoopSourceTypeID) {
	CFRunLoopRemoveSource(rl, (CFRunLoopSourceRef)item, modeName);
    } else if (CFGetTypeID(item) == __kCFRunLoopObserverTypeID) {
	CFRunLoopRemoveObserver(rl, (CFRunLoopObserverRef)item, modeName);
    } else if (CFGetTypeID(item) == __kCFRunLoopTimerTypeID) {
	CFRunLoopRemoveTimer(rl, (CFRunLoopTimerRef)item, modeName);
    }
}

void CFRunLoopAddCommonMode(CFRunLoopRef rl, CFStringRef modeName) {
    if (__CFRunLoopIsDeallocating(rl)) return;
    __CFRunLoopLock(rl);
    if (!CFSetContainsValue(rl->_commonModes, modeName)) {
	CFSetRef set = rl->_commonModeItems ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModeItems) : NULL;
	CFSetAddValue(rl->_commonModes, modeName);
	__CFRunLoopUnlock(rl);
	if (NULL != set) {
	    CFTypeRef context[2] = {rl, modeName};
	    /* add all common-modes items to new mode */
	    CFSetApplyFunction(set, (__CFRunLoopAddItemsToCommonMode), (void *)context);
	    CFRelease(set);
	}
    } else {
	__CFRunLoopUnlock(rl);
    }
}

static CFComparisonResult __CFRunLoopObserverComparator(const void *val1, const void *val2, void *context) {
    CFRunLoopObserverRef o1 = (CFRunLoopObserverRef)val1;
    CFRunLoopObserverRef o2 = (CFRunLoopObserverRef)val2;
    if (o1->_order < o2->_order) return kCFCompareLessThan;
    if (o2->_order < o1->_order) return kCFCompareGreaterThan;
    return kCFCompareEqualTo;
}

struct _collectobs {
    CFRunLoopActivity activity;
    CFMutableArrayRef array;
};

static void __CFRunLoopCollectObservers(const void *value, void *context) {
    CFRunLoopObserverRef rlo = (CFRunLoopObserverRef)value;
    struct _collectobs *info = (struct _collectobs *)context;
    if (0 != (rlo->_activities & info->activity) && __CFIsValid(rlo) && !__CFRunLoopObserverIsFiring(rlo)) {
	CFArrayAppendValue(info->array, rlo);
    }
}

/* rl is unlocked, rlm is locked on entrance and exit */
/* ALERT: this should collect all the candidate observers from the top level
 * and all submodes, recursively, THEN start calling them, in order to obey
 * the ordering parameter. */
static void __CFRunLoopDoObservers(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopActivity activity) {	/* DOES CALLOUT */
    CFIndex idx, cnt;
    CFMutableArrayRef array;
    CFArrayRef submodes;
    struct _collectobs info;

    /* Fire the observers */
    submodes = (NULL != rlm->_submodes && 0 < CFArrayGetCount(rlm->_submodes)) ? CFArrayCreateCopy(kCFAllocatorSystemDefault, rlm->_submodes) : NULL;
    if (NULL != rlm->_observers) {
	array = CFArrayCreateMutable(kCFAllocatorSystemDefault, CFSetGetCount(rlm->_observers), &kCFTypeArrayCallBacks);
	info.array = array;
	info.activity = activity;
	CFSetApplyFunction(rlm->_observers, (__CFRunLoopCollectObservers), &info);
	cnt = CFArrayGetCount(array);
	if (0 < cnt) {
	    __CFRunLoopModeUnlock(rlm);
	    CFArraySortValues(array, CFRangeMake(0, cnt), (__CFRunLoopObserverComparator), NULL);
	    for (idx = 0; idx < cnt; idx++) {
		CFRunLoopObserverRef rlo = (CFRunLoopObserverRef)CFArrayGetValueAtIndex(array, idx);
		__CFRunLoopObserverLock(rlo);
		if (__CFIsValid(rlo)) {
		    __CFRunLoopObserverUnlock(rlo);
		    __CFRunLoopObserverSetFiring(rlo);
		    rlo->_callout(rlo, activity, rlo->_context.info);	/* CALLOUT */
		    __CFRunLoopObserverUnsetFiring(rlo);
		    if (!__CFRunLoopObserverRepeats(rlo)) {
			CFRunLoopObserverInvalidate(rlo);
		    }
		} else {
		    __CFRunLoopObserverUnlock(rlo);
		}
	    }
	    __CFRunLoopModeLock(rlm);
	}
	CFRelease(array);
    }
    if (NULL != submodes) {
	__CFRunLoopModeUnlock(rlm);
	for (idx = 0, cnt = CFArrayGetCount(submodes); idx < cnt; idx++) {
	    CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(submodes, idx);
	    CFRunLoopModeRef subrlm;
            __CFRunLoopLock(rl);
            subrlm = __CFRunLoopFindMode(rl, modeName, false);
	    __CFRunLoopUnlock(rl);
	    if (NULL != subrlm) {
		__CFRunLoopDoObservers(rl, subrlm, activity);
		__CFRunLoopModeUnlock(subrlm);
	    }
	}
	CFRelease(submodes);
        __CFRunLoopModeLock(rlm);
    }
}

static CFComparisonResult __CFRunLoopSourceComparator(const void *val1, const void *val2, void *context) {
    CFRunLoopSourceRef o1 = (CFRunLoopSourceRef)val1;
    CFRunLoopSourceRef o2 = (CFRunLoopSourceRef)val2;
    if (o1->_order < o2->_order) return kCFCompareLessThan;
    if (o2->_order < o1->_order) return kCFCompareGreaterThan;
    return kCFCompareEqualTo;
}

static void __CFRunLoopCollectSources0(const void *value, void *context) {
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)value;
    CFTypeRef *sources = (CFTypeRef *)context;
    if (0 == rls->_context.version0.version && __CFIsValid(rls) && __CFRunLoopSourceIsSignaled(rls)) {
	if (NULL == *sources) {
	    *sources = CFRetain(rls);
	} else if (CFGetTypeID(*sources) == __kCFRunLoopSourceTypeID) {
	    CFTypeRef oldrls = *sources;
	    *sources = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
	    CFArrayAppendValue((CFMutableArrayRef)*sources, oldrls);
	    CFArrayAppendValue((CFMutableArrayRef)*sources, rls);
	    CFRelease(oldrls);
	} else {
	    CFArrayAppendValue((CFMutableArrayRef)*sources, rls);
	}
    }
}

/* rl is unlocked, rlm is locked on entrance and exit */
static Boolean __CFRunLoopDoSources0(CFRunLoopRef rl, CFRunLoopModeRef rlm, Boolean stopAfterHandle) {	/* DOES CALLOUT */
    CFTypeRef sources = NULL;
    Boolean sourceHandled = false;
    CFIndex idx, cnt;

    __CFRunLoopModeUnlock(rlm); // locks have to be taken in order
    __CFRunLoopLock(rl);
    __CFRunLoopModeLock(rlm);
    /* Fire the version 0 sources */
    if (NULL != rlm->_sources && 0 < CFSetGetCount(rlm->_sources)) {
	CFSetApplyFunction(rlm->_sources, (__CFRunLoopCollectSources0), &sources);
    }
    for (idx = 0, cnt = (NULL != rlm->_submodes) ? CFArrayGetCount(rlm->_submodes) : 0; idx < cnt; idx++) {
	CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
	CFRunLoopModeRef subrlm;
	subrlm = __CFRunLoopFindMode(rl, modeName, false);
	if (NULL != subrlm) {
	    if (NULL != subrlm->_sources && 0 < CFSetGetCount(subrlm->_sources)) {
		CFSetApplyFunction(subrlm->_sources, (__CFRunLoopCollectSources0), &sources);
	    }
	    __CFRunLoopModeUnlock(subrlm);
	}
    }
    __CFRunLoopUnlock(rl);
    if (NULL != sources) {
	// sources is either a single (retained) CFRunLoopSourceRef or an array of (retained) CFRunLoopSourceRef
	__CFRunLoopModeUnlock(rlm);
	if (CFGetTypeID(sources) == __kCFRunLoopSourceTypeID) {
	    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)sources;
	    __CFRunLoopSourceLock(rls);
	    __CFRunLoopSourceUnsetSignaled(rls);
	    if (__CFIsValid(rls)) {
		__CFRunLoopSourceUnlock(rls);
		if (NULL != rls->_context.version0.perform) {
		    rls->_context.version0.perform(rls->_context.version0.info); /* CALLOUT */
		}
		sourceHandled = true;
	    } else {
		__CFRunLoopSourceUnlock(rls);
	    }
	} else {
	    cnt = CFArrayGetCount(sources);
	    CFArraySortValues((CFMutableArrayRef)sources, CFRangeMake(0, cnt), (__CFRunLoopSourceComparator), NULL);
	    for (idx = 0; idx < cnt; idx++) {
		CFRunLoopSourceRef rls = (CFRunLoopSourceRef)CFArrayGetValueAtIndex(sources, idx);
		__CFRunLoopSourceLock(rls);
		__CFRunLoopSourceUnsetSignaled(rls);
		if (__CFIsValid(rls)) {
		    __CFRunLoopSourceUnlock(rls);
		    if (NULL != rls->_context.version0.perform) {
			rls->_context.version0.perform(rls->_context.version0.info); /* CALLOUT */
		    }
		    sourceHandled = true;
		} else {
		    __CFRunLoopSourceUnlock(rls);
		}
		if (stopAfterHandle && sourceHandled) {
		    break;
		}
	    }
	}
	CFRelease(sources);
	__CFRunLoopModeLock(rlm);
    }
    return sourceHandled;
}

#if defined(__MACH__)
static Boolean __CFRunLoopDoSource1(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopSourceRef rls, mach_msg_header_t *msg, CFIndex size, mach_msg_header_t **reply) {	/* DOES CALLOUT */
    Boolean sourceHandled = false;

    /* Fire a version 1 source */
    CFRetain(rls);
    __CFRunLoopModeUnlock(rlm);
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls)) {
	__CFRunLoopSourceUnsetSignaled(rls);
	__CFRunLoopSourceUnlock(rls);
	if (NULL != rls->_context.version1.perform) {
	    *reply = rls->_context.version1.perform(msg, size, kCFAllocatorSystemDefault, rls->_context.version1.info); /* CALLOUT */
	}
	sourceHandled = true;
    } else {
	__CFRunLoopSourceUnlock(rls);
    }
    CFRelease(rls);
    __CFRunLoopModeLock(rlm);
    return sourceHandled;
}
#endif 

static Boolean __CFRunLoopDoTimer(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopTimerRef rlt) {	/* DOES CALLOUT */
    Boolean timerHandled = false;
    int64_t oldFireTSR = 0;

    /* Fire a timer */
    CFRetain(rlt);
    __CFRunLoopModeUnlock(rlm);
    __CFRunLoopTimerLock(rlt);
    if (__CFIsValid(rlt) && !__CFRunLoopTimerIsFiring(rlt)) {
	__CFRunLoopTimerSetFiring(rlt);
	__CFRunLoopTimerUnlock(rlt);
	__CFRunLoopTimerFireTSRLock();
	oldFireTSR = rlt->_fireTSR;
	__CFRunLoopTimerFireTSRUnlock();
	rlt->_callout(rlt, rlt->_context.info);	/* CALLOUT */
	__CFRunLoopTimerUnsetFiring(rlt);
	timerHandled = true;
    } else {
	__CFRunLoopTimerUnlock(rlt);
    }
    if (__CFIsValid(rlt) && timerHandled) {
	if (0 == rlt->_intervalTSR) {
	    CFRunLoopTimerInvalidate(rlt);      /* DOES CALLOUT */
	} else {
	    /* This is just a little bit tricky: we want to support calling
	     * CFRunLoopTimerSetNextFireDate() from within the callout and
	     * honor that new time here if it is a later date, otherwise
	     * it is completely ignored. */
	    int64_t currentFireTSR;
	    __CFRunLoopTimerFireTSRLock();
	    currentFireTSR = rlt->_fireTSR;
	    if (oldFireTSR < currentFireTSR) {
		/* Next fire TSR was set, and set to a date after the previous
		 * fire date, so we honor it. */
	    } else {
		if ((uint64_t)LLONG_MAX <= (uint64_t)oldFireTSR + (uint64_t)rlt->_intervalTSR) {
		    currentFireTSR = LLONG_MAX;
		} else {
		    int64_t currentTSR = (int64_t)__CFReadTSR();
		    currentFireTSR = oldFireTSR;
		    while (currentFireTSR <= currentTSR) {
			currentFireTSR += rlt->_intervalTSR;
		    }
		}
	    }
	    rlt->_fireTSR = currentFireTSR;
	    __CFRunLoopTimerFireTSRUnlock();
	    __CFRunLoopTimerRescheduleWithAllModes(rlt, rl);
	}
    }
    CFRelease(rlt);
    __CFRunLoopModeLock(rlm);
    return timerHandled;
}

CF_EXPORT Boolean _CFRunLoopFinished(CFRunLoopRef rl, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    Boolean result = false;
    __CFRunLoopLock(rl);
    rlm = __CFRunLoopFindMode(rl, modeName, false);
    if (NULL == rlm || __CFRunLoopModeIsEmpty(rl, rlm)) {
	result = true;
    }
    __CFRunLoopUnlock(rl);
    if (rlm) __CFRunLoopModeUnlock(rlm);
    return result;
}

// rl is locked, rlm is locked on entry and exit
#if defined(__MACH__)
static void __CFRunLoopModeAddPortsToPortSet(CFRunLoopRef rl, CFRunLoopModeRef rlm, mach_port_t portSet) {
    CFIndex idx, cnt;
    const void **list, *buffer[256];

    // Timers and version 1 sources go into the portSet currently
    if (NULL != rlm->_sources) {
	cnt = CFSetGetCount(rlm->_sources);
	list = (cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0);
	CFSetGetValues(rlm->_sources, list);
	for (idx = 0; idx < cnt; idx++) {
	    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)list[idx];
	    mach_port_t port;
	    if (1 != rls->_context.version0.version) continue;
	    port = rls->_context.version1.getPort(rls->_context.version1.info);
	    if (MACH_PORT_NULL != port) {
		mach_port_insert_member(mach_task_self(), port, portSet);
	    }
	}
	if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
    }
    if (NULL != rlm->_timers) {
	cnt = CFSetGetCount(rlm->_timers);
	list = (cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0);
	CFSetGetValues(rlm->_timers, list);
	for (idx = 0; idx < cnt; idx++) {
	    CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)list[idx];
	    if (MACH_PORT_NULL != rlt->_port) {
		mach_port_insert_member(mach_task_self(), rlt->_port, portSet);
	    }
	}
	if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
    }
    // iterate over submodes
    for (idx = 0, cnt = NULL != rlm->_submodes ? CFArrayGetCount(rlm->_submodes) : 0; idx < cnt; idx++) {
	CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
	CFRunLoopModeRef subrlm;
	subrlm = __CFRunLoopFindMode(rl, modeName, false);
	if (NULL != subrlm) {
	    __CFRunLoopModeAddPortsToPortSet(rl, subrlm, portSet);
	    __CFRunLoopModeUnlock(subrlm);
	}
    }
}
#endif

/* rl is unlocked, rlm locked on entrance and exit */
static int32_t __CFRunLoopRun(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFTimeInterval seconds, Boolean stopAfterHandle, Boolean waitIfEmpty) {  /* DOES CALLOUT */
    int64_t termTSR;
#if defined(__MACH__)
    mach_port_name_t timeoutPort = MACH_PORT_NULL;
    Boolean timeoutPortAdded = false;
#elif defined(__WIN32__)
    HANDLE timeoutPort = NULL;
#endif
    Boolean poll = false;

    if (__CFRunLoopIsStopped(rl)) {
	return kCFRunLoopRunStopped;
    } else if (rlm->_stopped) {
	rlm->_stopped = false;
	return kCFRunLoopRunStopped;
    }
#if !defined(__WIN32__)
    if (seconds <= 0.0) {
	termTSR = 0;
    } else if (__CFTSRToTimeInterval(LLONG_MAX) < seconds) {
	termTSR = LLONG_MAX;
    } else if ((uint64_t)LLONG_MAX <= __CFReadTSR() + (uint64_t)__CFTimeIntervalToTSR(seconds)) {
	termTSR = LLONG_MAX;
    } else {
	termTSR = (int64_t)__CFReadTSR() + __CFTimeIntervalToTSR(seconds);
	timeoutPort = mk_timer_create();
	mk_timer_arm(timeoutPort, __CFUInt64ToAbsoluteTime(termTSR));
    }
#elif defined(__WIN32__)
	{
		//int64_t time = (int64_t)(seconds * -10000000.0);
		//timeoutPort = CreateWaitableTimer(NULL,FALSE,NULL);
		//SetWaitableTimer(rl->_waitPort, &time, 0, NULL, NULL);
	}
#endif
    if (seconds <= 0.0) {
	poll = true;
    }
    for (;;) {
#if defined(__MACH__)
	mach_msg_header_t *msg;
	kern_return_t ret;
	mach_port_t waitSet = MACH_PORT_NULL;
	Boolean destroyWaitSet = false;
#endif
	CFRunLoopSourceRef rls;
	int32_t returnValue = 0;
	Boolean sourceHandledThisLoop = false;
	uint8_t buffer[1024 + 80];	// large enough for 1k of inline payload

	__CFRunLoopDoObservers(rl, rlm, kCFRunLoopBeforeTimers);
    	__CFRunLoopDoObservers(rl, rlm, kCFRunLoopBeforeSources);

    	sourceHandledThisLoop = __CFRunLoopDoSources0(rl, rlm, stopAfterHandle);

	if (sourceHandledThisLoop) {
	    poll = true;
	}

	if (!poll) {
	    __CFRunLoopDoObservers(rl, rlm, kCFRunLoopBeforeWaiting);
	    __CFRunLoopSetSleeping(rl);
	}
#if defined(__MACH__)
	if (NULL != rlm->_submodes) {
	    // !!! what do we do if this doesn't succeed?
	    ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &waitSet);
	    if (KERN_SUCCESS != ret) HALT;
	    __CFRunLoopModeUnlock(rlm);
	    __CFRunLoopLock(rl);
	    __CFRunLoopModeLock(rlm);
	    __CFRunLoopModeAddPortsToPortSet(rl, rlm, waitSet);
	    __CFRunLoopUnlock(rl);
	    if (MACH_PORT_NULL != timeoutPort) {
		mach_port_insert_member(mach_task_self(), timeoutPort, waitSet);
	    }
	    destroyWaitSet = true;
	} else {
	    waitSet = rlm->_portSet;
	    if (!timeoutPortAdded && MACH_PORT_NULL != timeoutPort) {
		mach_port_insert_member(mach_task_self(), timeoutPort, waitSet);
		timeoutPortAdded = true;
	    }
	}
	__CFRunLoopModeUnlock(rlm);

	msg = (mach_msg_header_t *)buffer;
	msg->msgh_size = sizeof(buffer);

	/* In that sleep of death what nightmares may come ... */
	try_receive:
	msg->msgh_bits = 0;
	msg->msgh_local_port = waitSet;
	msg->msgh_remote_port = MACH_PORT_NULL;
	msg->msgh_id = 0;
#if defined(MACH_RCV_TRAILER_AUDIT)
	ret = mach_msg(msg, MACH_RCV_MSG|MACH_RCV_LARGE|(poll ? MACH_RCV_TIMEOUT : 0)|MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)|MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT), 0, msg->msgh_size, waitSet, 0, MACH_PORT_NULL);
#else
	ret = mach_msg(msg, MACH_RCV_MSG|MACH_RCV_LARGE|(poll ? MACH_RCV_TIMEOUT : 0)|MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)|MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_SENDER), 0, msg->msgh_size, waitSet, 0, MACH_PORT_NULL);
#endif
	if (MACH_RCV_TOO_LARGE == ret) {
#if defined(MACH_RCV_TRAILER_AUDIT)
	    uint32_t newSize = round_msg(msg->msgh_size) + sizeof(mach_msg_audit_trailer_t);
#else
	    uint32_t newSize = round_msg(msg->msgh_size) + sizeof(mach_msg_security_trailer_t);
#endif
	    if (msg == (mach_msg_header_t *)buffer) msg = NULL;
	    msg = CFAllocatorReallocate(kCFAllocatorSystemDefault, msg, newSize, 0);
	    msg->msgh_size = newSize;
	    goto try_receive;
	} else if (MACH_RCV_TIMED_OUT == ret) {
	    // timeout, for poll
	    if (msg != (mach_msg_header_t *)buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, msg);
	    msg = NULL;
	} else if (MACH_MSG_SUCCESS != ret) {
	    HALT;
	}
#elif defined(__WIN32__)  
// should msgQMask be an OR'ing of this and all submodes' masks?
	if (0 == GetQueueStatus(rlm->_msgQMask)) {
	    HANDLE objects[2];
	    objects[0] = rl->_waitPort;
	    //objects[1] = timeoutPort;
	    MsgWaitForMultipleObjects(1 /*1*/, objects /*&(rl->_waitPort)*/, false, seconds, rlm->_msgQMask);
	}
	ResetEvent(rl->_waitPort);
#endif

#if defined(__MACH__)
	if (destroyWaitSet) {
	    __CFClearPortSet(mach_task_self(), waitSet);
	    mach_port_destroy(mach_task_self(), waitSet);
	}
#endif
	__CFRunLoopLock(rl);
	__CFRunLoopModeLock(rlm);
	__CFRunLoopUnlock(rl);
	if (!poll) {
	    __CFRunLoopUnsetSleeping(rl);
	    __CFRunLoopDoObservers(rl, rlm, kCFRunLoopAfterWaiting);
	}
	poll = false;
	__CFRunLoopModeUnlock(rlm);
	__CFRunLoopLock(rl);
	__CFRunLoopModeLock(rlm);

#if defined(__MACH__)
	if (NULL != msg) {
	    if (msg->msgh_local_port == timeoutPort) {
		returnValue = kCFRunLoopRunTimedOut;
		__CFRunLoopUnlock(rl);
	    } else if (msg->msgh_local_port == rl->_waitPort) {
		// wakeup
		__CFRunLoopUnlock(rl);
	    } else if (NULL != (rls = __CFRunLoopModeFindSourceForMachPort(rl, rlm, msg->msgh_local_port))) {
		mach_msg_header_t *reply = NULL;
		__CFRunLoopUnlock(rl);
		if (__CFRunLoopDoSource1(rl, rlm, rls, msg, msg->msgh_size, &reply)) {
		    sourceHandledThisLoop = true;
		}
		if (NULL != reply) {
		    ret = mach_msg(reply, MACH_SEND_MSG, reply->msgh_size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
//#warning CF: what should be done with the return value?
		    CFAllocatorDeallocate(kCFAllocatorSystemDefault, reply);
		}
	    } else {
		CFRunLoopTimerRef rlt;
		rlt = __CFRunLoopModeFindTimerForMachPort(rlm, msg->msgh_local_port);
		 __CFRunLoopUnlock(rl);
		if (NULL != rlt) {
		    __CFRunLoopDoTimer(rl, rlm, rlt);
		}
	    }
	    if (msg != (mach_msg_header_t *)buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, msg);
	} else {
	    __CFRunLoopUnlock(rl);
	}	
#endif

	__CFRunLoopModeUnlock(rlm);	// locks must be taken in order
	__CFRunLoopLock(rl);
	__CFRunLoopModeLock(rlm);
	if (sourceHandledThisLoop && stopAfterHandle) {
	    returnValue = kCFRunLoopRunHandledSource;
	} else if (0 != returnValue || (uint64_t)termTSR <= __CFReadTSR()) {
	    returnValue = kCFRunLoopRunTimedOut;
	} else if (__CFRunLoopIsStopped(rl)) {
	    returnValue = kCFRunLoopRunStopped;
	} else if (rlm->_stopped) {
	    rlm->_stopped = false;
	    returnValue = kCFRunLoopRunStopped;
	} else if (!waitIfEmpty && __CFRunLoopModeIsEmpty(rl, rlm)) {
	    returnValue = kCFRunLoopRunFinished;
	}
	__CFRunLoopUnlock(rl);
	if (0 != returnValue) {
#if defined(__MACH__)
	    if (MACH_PORT_NULL != timeoutPort) {
		if (!destroyWaitSet) mach_port_extract_member(mach_task_self(), timeoutPort, waitSet);
		mk_timer_destroy(timeoutPort);
	    }
#endif
	    return returnValue;
	}
    }
}

void CFRunLoopRun(void) {	/* DOES CALLOUT */
    int32_t result;
    do {
	result = CFRunLoopRunSpecific(CFRunLoopGetCurrent(), kCFRunLoopDefaultMode, 1.0e10, false);
    } while (kCFRunLoopRunStopped != result && kCFRunLoopRunFinished != result);
}

SInt32 CFRunLoopRunSpecific(CFRunLoopRef rl, CFStringRef modeName, CFTimeInterval seconds, Boolean returnAfterSourceHandled) {     /* DOES CALLOUT */
    CFRunLoopModeRef currentMode, previousMode;
    CFIndex *previousStopped; 
    int32_t result;

    if (__CFRunLoopIsDeallocating(rl)) return kCFRunLoopRunFinished;
    __CFRunLoopLock(rl);
    currentMode = __CFRunLoopFindMode(rl, modeName, false);
    if (NULL == currentMode || __CFRunLoopModeIsEmpty(rl, currentMode)) {
	if (currentMode) __CFRunLoopModeUnlock(currentMode);
	__CFRunLoopUnlock(rl);
	return kCFRunLoopRunFinished;
    }
    previousStopped = (CFIndex *)rl->_stopped;
    rl->_stopped = CFAllocatorAllocate(kCFAllocatorSystemDefault, 16, 0);
    rl->_stopped[0] = 0x4346524C;
    rl->_stopped[1] = 0x4346524C; // 'CFRL'
    rl->_stopped[2] = 0x00000000; // here the value is stored
    rl->_stopped[3] = 0x4346524C;
    previousMode = rl->_currentMode;
    rl->_currentMode = currentMode;
    __CFRunLoopUnlock(rl);
    __CFRunLoopDoObservers(rl, currentMode, kCFRunLoopEntry);
    result = __CFRunLoopRun(rl, currentMode, seconds, returnAfterSourceHandled, false);
    __CFRunLoopDoObservers(rl, currentMode, kCFRunLoopExit);
    __CFRunLoopModeUnlock(currentMode);
    __CFRunLoopLock(rl);
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, (void *)rl->_stopped);
    rl->_stopped = previousStopped;
    rl->_currentMode = previousMode;
    __CFRunLoopUnlock(rl);
    return result;
}

SInt32 CFRunLoopRunInMode(CFStringRef modeName, CFTimeInterval seconds, Boolean returnAfterSourceHandled) {     /* DOES CALLOUT */
    return CFRunLoopRunSpecific(CFRunLoopGetCurrent(), modeName, seconds, returnAfterSourceHandled);
}

static void __CFRunLoopFindMinTimer(const void *value, void *ctx) {
    CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)value;
    CFRunLoopTimerRef *result = ctx;
    if (NULL == *result || rlt->_fireTSR < (*result)->_fireTSR) {
	*result = rlt;
    }
}

CFAbsoluteTime CFRunLoopGetNextTimerFireDate(CFRunLoopRef rl, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    CFRunLoopTimerRef result = NULL;
    int64_t fireTime = 0;
    __CFRunLoopLock(rl);
    rlm = __CFRunLoopFindMode(rl, modeName, false);
    __CFRunLoopUnlock(rl);
    if (rlm) {
	if (NULL != rlm->_timers && 0 < CFSetGetCount(rlm->_timers)) {
	    __CFRunLoopTimerFireTSRLock();
	    CFSetApplyFunction(rlm->_timers, (__CFRunLoopFindMinTimer), &result);
	    fireTime = result->_fireTSR;
	    __CFRunLoopTimerFireTSRUnlock();
	}
	__CFRunLoopModeUnlock(rlm);
    }
    return (0 == fireTime) ? 0.0 : __CFTSRToAbsoluteTime(fireTime);
}

Boolean CFRunLoopIsWaiting(CFRunLoopRef rl) {
    return __CFRunLoopIsSleeping(rl);
}

void CFRunLoopWakeUp(CFRunLoopRef rl) {
#if defined(__MACH__)
    kern_return_t ret;
    /* We unconditionally try to send the message, since we don't want
     * to lose a wakeup, but the send may fail if there is already a
     * wakeup pending, since the queue length is 1. */
    ret = __CFSendTrivialMachMessage(rl->_waitPort, 0, MACH_SEND_TIMEOUT, 0);
    if (ret != MACH_MSG_SUCCESS && ret != MACH_SEND_TIMED_OUT) {
	HALT;
    }
#else
    SetEvent(rl->_waitPort);
#endif
}

void CFRunLoopStop(CFRunLoopRef rl) {
    __CFRunLoopLock(rl);
    __CFRunLoopSetStopped(rl);
    __CFRunLoopUnlock(rl);
    CFRunLoopWakeUp(rl);
}

CF_EXPORT void _CFRunLoopStopMode(CFRunLoopRef rl, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    rlm = __CFRunLoopFindMode(rl, modeName, true);
    __CFRunLoopUnlock(rl);
    if (NULL != rlm) {
	rlm->_stopped = true;
	__CFRunLoopModeUnlock(rlm);
    }
    CFRunLoopWakeUp(rl);
}

CF_EXPORT Boolean _CFRunLoopModeContainsMode(CFRunLoopRef rl, CFStringRef modeName, CFStringRef candidateContainedName) {
    CFRunLoopModeRef rlm;
    if (modeName == kCFRunLoopCommonModes || candidateContainedName == kCFRunLoopCommonModes) {
	return false;
    } else if (CFEqual(modeName, candidateContainedName)) {
	return true;
    }
    __CFRunLoopLock(rl);
    rlm = __CFRunLoopFindMode(rl, modeName, true);
    __CFRunLoopUnlock(rl);
    if (NULL != rlm) {
	CFArrayRef submodes;
	if (NULL == rlm->_submodes) {
	    __CFRunLoopModeUnlock(rlm);
	    return false;
	}
	if (CFArrayContainsValue(rlm->_submodes, CFRangeMake(0, CFArrayGetCount(rlm->_submodes)), candidateContainedName)) {
	    __CFRunLoopModeUnlock(rlm);
	    return true;
	}
	submodes = (NULL != rlm->_submodes && 0 < CFArrayGetCount(rlm->_submodes)) ? CFArrayCreateCopy(kCFAllocatorSystemDefault, rlm->_submodes) : NULL;
	__CFRunLoopModeUnlock(rlm);
	if (NULL != submodes) {
	    CFIndex idx, cnt;
	    for (idx = 0, cnt = CFArrayGetCount(submodes); idx < cnt; idx++) {
		CFStringRef subname = (CFStringRef)CFArrayGetValueAtIndex(submodes, idx);
		if (_CFRunLoopModeContainsMode(rl, subname, candidateContainedName)) {
		    CFRelease(submodes);
		    return true;
		}
	    }
	    CFRelease(submodes);
	}
    }
    return false;
}

CF_EXPORT void _CFRunLoopAddModeToMode(CFRunLoopRef rl, CFStringRef modeName, CFStringRef toModeName) {
    CFRunLoopModeRef rlm;
    if (__CFRunLoopIsDeallocating(rl)) return;
    // should really do a recursive check here, to make sure that a cycle isn't
    // introduced; of course, if that happens, you aren't going to get very far.
    if (modeName == kCFRunLoopCommonModes || toModeName == kCFRunLoopCommonModes || CFEqual(modeName, toModeName)) {
	return;
    } else {
	__CFRunLoopLock(rl);
	rlm = __CFRunLoopFindMode(rl, toModeName, true);
	__CFRunLoopUnlock(rl);
	if (NULL != rlm) {
	    if (NULL == rlm->_submodes) {
		rlm->_submodes = CFArrayCreateMutable(CFGetAllocator(rlm), 0, &kCFTypeArrayCallBacks);
	    }
	    if (!CFArrayContainsValue(rlm->_submodes, CFRangeMake(0, CFArrayGetCount(rlm->_submodes)), modeName)) {
		CFArrayAppendValue(rlm->_submodes, modeName);
	    }
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}

CF_EXPORT void _CFRunLoopRemoveModeFromMode(CFRunLoopRef rl, CFStringRef modeName, CFStringRef fromModeName) {
    CFRunLoopModeRef rlm;
    // should really do a recursive check here, to make sure that a cycle isn't
    // introduced; of course, if that happens, you aren't going to get very far.
    if (modeName == kCFRunLoopCommonModes || fromModeName == kCFRunLoopCommonModes || CFEqual(modeName, fromModeName)) {
	return;
    } else {
	__CFRunLoopLock(rl);
	rlm = __CFRunLoopFindMode(rl, fromModeName, true);
	__CFRunLoopUnlock(rl);
	if (NULL != rlm) {
	    if (NULL != rlm->_submodes) {
		CFIndex idx, cnt = CFArrayGetCount(rlm->_submodes);
		idx = CFArrayGetFirstIndexOfValue(rlm->_submodes, CFRangeMake(0, cnt), modeName);
		if (0 <= idx) CFArrayRemoveValueAtIndex(rlm->_submodes, idx);
	    }
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}

Boolean CFRunLoopContainsSource(CFRunLoopRef rl, CFRunLoopSourceRef rls, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    Boolean hasValue = false;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	if (NULL != rl->_commonModeItems) {
	    hasValue = CFSetContainsValue(rl->_commonModeItems, rls);
	}
	__CFRunLoopUnlock(rl);
    } else {
	rlm = __CFRunLoopFindMode(rl, modeName, false);
	__CFRunLoopUnlock(rl);
	if (NULL != rlm && NULL != rlm->_sources) {
	    hasValue = CFSetContainsValue(rlm->_sources, rls);
	    __CFRunLoopModeUnlock(rlm);
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
    return hasValue;
}

void CFRunLoopAddSource(CFRunLoopRef rl, CFRunLoopSourceRef rls, CFStringRef modeName) {	/* DOES CALLOUT */
    CFRunLoopModeRef rlm;
    if (__CFRunLoopIsDeallocating(rl)) return;
    if (!__CFIsValid(rls)) return;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
	if (NULL == rl->_commonModeItems) {
	    rl->_commonModeItems = CFSetCreateMutable(CFGetAllocator(rl), 0, &kCFTypeSetCallBacks);
	    _CFSetSetCapacity(rl->_commonModeItems, 20);
	}
	CFSetAddValue(rl->_commonModeItems, rls);
	__CFRunLoopUnlock(rl);
	if (NULL != set) {
	    CFTypeRef context[2] = {rl, rls};
	    /* add new item to all common-modes */
	    CFSetApplyFunction(set, (__CFRunLoopAddItemToCommonModes), (void *)context);
	    CFRelease(set);
	}
    } else {
	rlm = __CFRunLoopFindMode(rl, modeName, true);
	__CFRunLoopUnlock(rl);
	if (NULL != rlm && NULL == rlm->_sources) {
	    rlm->_sources = CFSetCreateMutable(CFGetAllocator(rlm), 0, &kCFTypeSetCallBacks);
	    _CFSetSetCapacity(rlm->_sources, 10);
	}
	if (NULL != rlm && !CFSetContainsValue(rlm->_sources, rls)) {
	    CFSetAddValue(rlm->_sources, rls);
	    __CFRunLoopModeUnlock(rlm);
	    __CFRunLoopSourceSchedule(rls, rl, rlm);	/* DOES CALLOUT */
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}

void CFRunLoopRemoveSource(CFRunLoopRef rl, CFRunLoopSourceRef rls, CFStringRef modeName) {	/* DOES CALLOUT */
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	if (NULL != rl->_commonModeItems && CFSetContainsValue(rl->_commonModeItems, rls)) {
	    CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
	    CFSetRemoveValue(rl->_commonModeItems, rls);
	    __CFRunLoopUnlock(rl);
	    if (NULL != set) {
		CFTypeRef context[2] = {rl, rls};
		/* remove new item from all common-modes */
		CFSetApplyFunction(set, (__CFRunLoopRemoveItemFromCommonModes), (void *)context);
		CFRelease(set);
	    }
	} else {
	    __CFRunLoopUnlock(rl);
	}
    } else {
	rlm = __CFRunLoopFindMode(rl, modeName, false);
	__CFRunLoopUnlock(rl);
	if (NULL != rlm && NULL != rlm->_sources && CFSetContainsValue(rlm->_sources, rls)) {
	    CFRetain(rls);
	    CFSetRemoveValue(rlm->_sources, rls);
	    __CFRunLoopModeUnlock(rlm);
	    __CFRunLoopSourceCancel(rls, rl, rlm);	/* DOES CALLOUT */
	    CFRelease(rls);
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}

Boolean CFRunLoopContainsObserver(CFRunLoopRef rl, CFRunLoopObserverRef rlo, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    Boolean hasValue = false;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	if (NULL != rl->_commonModeItems) {
	    hasValue = CFSetContainsValue(rl->_commonModeItems, rlo);
	}
	__CFRunLoopUnlock(rl);
    } else {
	rlm = __CFRunLoopFindMode(rl, modeName, false);
	__CFRunLoopUnlock(rl);
	if (NULL != rlm && NULL != rlm->_observers) {
	    hasValue = CFSetContainsValue(rlm->_observers, rlo);
	    __CFRunLoopModeUnlock(rlm);
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
    return hasValue;
}

void CFRunLoopAddObserver(CFRunLoopRef rl, CFRunLoopObserverRef rlo, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    if (__CFRunLoopIsDeallocating(rl)) return;
    if (!__CFIsValid(rlo) || (NULL != rlo->_runLoop && rlo->_runLoop != rl)) return;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
	if (NULL == rl->_commonModeItems) {
	    rl->_commonModeItems = CFSetCreateMutable(CFGetAllocator(rl), 0, &kCFTypeSetCallBacks);
	}
	CFSetAddValue(rl->_commonModeItems, rlo);
	__CFRunLoopUnlock(rl);
	if (NULL != set) {
	    CFTypeRef context[2] = {rl, rlo};
	    /* add new item to all common-modes */
	    CFSetApplyFunction(set, (__CFRunLoopAddItemToCommonModes), (void *)context);
	    CFRelease(set);
	}
    } else {
	rlm = __CFRunLoopFindMode(rl, modeName, true);
	__CFRunLoopUnlock(rl);
	if (NULL != rlm && NULL == rlm->_observers) {
	    rlm->_observers = CFSetCreateMutable(CFGetAllocator(rlm), 0, &kCFTypeSetCallBacks);
	}
	if (NULL != rlm && !CFSetContainsValue(rlm->_observers, rlo)) {
	    CFSetAddValue(rlm->_observers, rlo);
	    __CFRunLoopModeUnlock(rlm);
	    __CFRunLoopObserverSchedule(rlo, rl, rlm);
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}

void CFRunLoopRemoveObserver(CFRunLoopRef rl, CFRunLoopObserverRef rlo, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	if (NULL != rl->_commonModeItems && CFSetContainsValue(rl->_commonModeItems, rlo)) {
	    CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
	    CFSetRemoveValue(rl->_commonModeItems, rlo);
	    __CFRunLoopUnlock(rl);
	    if (NULL != set) {
		CFTypeRef context[2] = {rl, rlo};
		/* remove new item from all common-modes */
		CFSetApplyFunction(set, (__CFRunLoopRemoveItemFromCommonModes), (void *)context);
		CFRelease(set);
	    }
	} else {
	    __CFRunLoopUnlock(rl);
	}
    } else {
	rlm = __CFRunLoopFindMode(rl, modeName, false);
	__CFRunLoopUnlock(rl);
	if (NULL != rlm && NULL != rlm->_observers && CFSetContainsValue(rlm->_observers, rlo)) {
	    CFRetain(rlo);
	    CFSetRemoveValue(rlm->_observers, rlo);
	    __CFRunLoopModeUnlock(rlm);
	    __CFRunLoopObserverCancel(rlo, rl, rlm);
	    CFRelease(rlo);
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}

Boolean CFRunLoopContainsTimer(CFRunLoopRef rl, CFRunLoopTimerRef rlt, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    Boolean hasValue = false;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	if (NULL != rl->_commonModeItems) {
	    hasValue = CFSetContainsValue(rl->_commonModeItems, rlt);
	}
	__CFRunLoopUnlock(rl);
    } else {
	rlm = __CFRunLoopFindMode(rl, modeName, false);
	__CFRunLoopUnlock(rl);
	if (NULL != rlm && NULL != rlm->_timers) {
	    hasValue = CFSetContainsValue(rlm->_timers, rlt);
	    __CFRunLoopModeUnlock(rlm);
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
    return hasValue;
}

void CFRunLoopAddTimer(CFRunLoopRef rl, CFRunLoopTimerRef rlt, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    if (__CFRunLoopIsDeallocating(rl)) return;
    if (!__CFIsValid(rlt) || (NULL != rlt->_runLoop && rlt->_runLoop != rl)) return;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
	if (NULL == rl->_commonModeItems) {
	    rl->_commonModeItems = CFSetCreateMutable(CFGetAllocator(rl), 0, &kCFTypeSetCallBacks);
	}
	CFSetAddValue(rl->_commonModeItems, rlt);
	__CFRunLoopUnlock(rl);
	if (NULL != set) {
	    CFTypeRef context[2] = {rl, rlt};
	    /* add new item to all common-modes */
	    CFSetApplyFunction(set, (__CFRunLoopAddItemToCommonModes), (void *)context);
	    CFRelease(set);
	}
    } else {
	rlm = __CFRunLoopFindMode(rl, modeName, true);
	__CFRunLoopUnlock(rl);
	if (NULL != rlm && NULL == rlm->_timers) {
	    rlm->_timers = CFSetCreateMutable(CFGetAllocator(rlm), 0, &kCFTypeSetCallBacks);
	}
	if (NULL != rlm && !CFSetContainsValue(rlm->_timers, rlt)) {
	    CFSetAddValue(rlm->_timers, rlt);
	    __CFRunLoopModeUnlock(rlm);
	    __CFRunLoopTimerSchedule(rlt, rl, rlm);
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}

void CFRunLoopRemoveTimer(CFRunLoopRef rl, CFRunLoopTimerRef rlt, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	if (NULL != rl->_commonModeItems && CFSetContainsValue(rl->_commonModeItems, rlt)) {
	    CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
	    CFSetRemoveValue(rl->_commonModeItems, rlt);
	    __CFRunLoopUnlock(rl);
	    if (NULL != set) {
		CFTypeRef context[2] = {rl, rlt};
		/* remove new item from all common-modes */
		CFSetApplyFunction(set, (__CFRunLoopRemoveItemFromCommonModes), (void *)context);
		CFRelease(set);
	    }
	} else {
	    __CFRunLoopUnlock(rl);
	}
    } else {
	rlm = __CFRunLoopFindMode(rl, modeName, false);
	__CFRunLoopUnlock(rl);
	if (NULL != rlm && NULL != rlm->_timers && CFSetContainsValue(rlm->_timers, rlt)) {
	    CFRetain(rlt);
	    CFSetRemoveValue(rlm->_timers, rlt);
	    __CFRunLoopModeUnlock(rlm);
	    __CFRunLoopTimerCancel(rlt, rl, rlm);
	    CFRelease(rlt);
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}


/* CFRunLoopSource */

static Boolean __CFRunLoopSourceEqual(CFTypeRef cf1, CFTypeRef cf2) {	/* DOES CALLOUT */
    CFRunLoopSourceRef rls1 = (CFRunLoopSourceRef)cf1;
    CFRunLoopSourceRef rls2 = (CFRunLoopSourceRef)cf2;
    if (rls1 == rls2) return true;
    if (rls1->_order != rls2->_order) return false;
    if (rls1->_context.version0.version != rls2->_context.version0.version) return false;
    if (rls1->_context.version0.hash != rls2->_context.version0.hash) return false;
    if (rls1->_context.version0.equal != rls2->_context.version0.equal) return false;
    if (0 == rls1->_context.version0.version && rls1->_context.version0.perform != rls2->_context.version0.perform) return false;
    if (1 == rls1->_context.version0.version && rls1->_context.version1.perform != rls2->_context.version1.perform) return false;
    if (rls1->_context.version0.equal)
	return rls1->_context.version0.equal(rls1->_context.version0.info, rls2->_context.version0.info);
    return (rls1->_context.version0.info == rls2->_context.version0.info);
}

static CFHashCode __CFRunLoopSourceHash(CFTypeRef cf) {	/* DOES CALLOUT */
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)cf;
    if (rls->_context.version0.hash)
	return rls->_context.version0.hash(rls->_context.version0.info);
    return (CFHashCode)rls->_context.version0.info;
}

static CFStringRef __CFRunLoopSourceCopyDescription(CFTypeRef cf) {	/* DOES CALLOUT */
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)cf;
    CFStringRef result;
    CFStringRef contextDesc = NULL;
    if (NULL != rls->_context.version0.copyDescription) {
	contextDesc = rls->_context.version0.copyDescription(rls->_context.version0.info);
    }
    if (NULL == contextDesc) {
	contextDesc = CFStringCreateWithFormat(CFGetAllocator(rls), NULL, CFSTR("<CFRunLoopSource context %p>"), rls->_context.version0.info);
    }
    result = CFStringCreateWithFormat(CFGetAllocator(rls), NULL, CFSTR("<CFRunLoopSource %p [%p]>{locked = %s, valid = %s, order = %d, context = %@}"), cf, CFGetAllocator(rls), rls->_lock ? "Yes" : "No", __CFIsValid(rls) ? "Yes" : "No", rls->_order, contextDesc);
    CFRelease(contextDesc);
    return result;
}

static void __CFRunLoopSourceDeallocate(CFTypeRef cf) {	/* DOES CALLOUT */
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)cf;
    CFRunLoopSourceInvalidate(rls);
    if (rls->_context.version0.release) {
	rls->_context.version0.release(rls->_context.version0.info);
    }
}

static const CFRuntimeClass __CFRunLoopSourceClass = {
    0,
    "CFRunLoopSource",
    NULL,      // init
    NULL,      // copy
    __CFRunLoopSourceDeallocate,
    __CFRunLoopSourceEqual,
    __CFRunLoopSourceHash,
    NULL,      // 
    __CFRunLoopSourceCopyDescription
};

__private_extern__ void __CFRunLoopSourceInitialize(void) {
    __kCFRunLoopSourceTypeID = _CFRuntimeRegisterClass(&__CFRunLoopSourceClass);
}

CFTypeID CFRunLoopSourceGetTypeID(void) {
    return __kCFRunLoopSourceTypeID;
}

CFRunLoopSourceRef CFRunLoopSourceCreate(CFAllocatorRef allocator, CFIndex order, CFRunLoopSourceContext *context) {
    CFRunLoopSourceRef memory;
    uint32_t size;
    if (NULL == context) HALT;
    size = sizeof(struct __CFRunLoopSource) - sizeof(CFRuntimeBase);
    memory = (CFRunLoopSourceRef)_CFRuntimeCreateInstance(allocator, __kCFRunLoopSourceTypeID, size, NULL);
    if (NULL == memory) {
	return NULL;
    }
    __CFSetValid(memory);
    __CFRunLoopSourceUnsetSignaled(memory);
    memory->_lock = 0;
    memory->_bits = 0;
    memory->_order = order;
    memory->_runLoops = NULL;
#if defined(__MACH__)
    memmove(&memory->_context, context, (0 == context->version) ? sizeof(CFRunLoopSourceContext) : sizeof(CFRunLoopSourceContext1));
#else
    memmove(&memory->_context, context, sizeof(CFRunLoopSourceContext));
#endif
    if (context->retain) {
	memory->_context.version0.info = (void *)context->retain(context->info);
    }
    return memory;
}

CFIndex CFRunLoopSourceGetOrder(CFRunLoopSourceRef rls) {
    __CFGenericValidateType(rls, __kCFRunLoopSourceTypeID);
    return rls->_order;
}

static void __CFRunLoopSourceRemoveFromRunLoop(const void *value, void *context) {
    CFRunLoopRef rl = (CFRunLoopRef)value;
    CFTypeRef *params = context;
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)params[0];
    CFArrayRef array;
    CFIndex idx;
    if (rl == params[1]) return;
    array = CFRunLoopCopyAllModes(rl);
    for (idx = CFArrayGetCount(array); idx--;) {
	CFStringRef modeName = CFArrayGetValueAtIndex(array, idx);
	CFRunLoopRemoveSource(rl, rls, modeName);
    }
    CFRunLoopRemoveSource(rl, rls, kCFRunLoopCommonModes);
    CFRelease(array);
    params[1] = rl;
}

void CFRunLoopSourceInvalidate(CFRunLoopSourceRef rls) {
    __CFGenericValidateType(rls, __kCFRunLoopSourceTypeID);
    CFRetain(rls);
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls)) {
	__CFUnsetValid(rls);
	if (NULL != rls->_runLoops) {
	    CFTypeRef params[2] = {rls, NULL};
	    CFBagRef bag = CFBagCreateCopy(kCFAllocatorSystemDefault, rls->_runLoops);
	    CFRelease(rls->_runLoops);
	    rls->_runLoops = NULL;
	    __CFRunLoopSourceUnlock(rls);
	    CFBagApplyFunction(bag, (__CFRunLoopSourceRemoveFromRunLoop), params);
	    CFRelease(bag);
	} else {
	    __CFRunLoopSourceUnlock(rls);
	}
	/* for hashing- and equality-use purposes, can't actually release the context here */
    } else {
	__CFRunLoopSourceUnlock(rls);
    }
    CFRelease(rls);
}

Boolean CFRunLoopSourceIsValid(CFRunLoopSourceRef rls) {
    __CFGenericValidateType(rls, __kCFRunLoopSourceTypeID);
    return __CFIsValid(rls);
}

void CFRunLoopSourceGetContext(CFRunLoopSourceRef rls, CFRunLoopSourceContext *context) {
    __CFGenericValidateType(rls, __kCFRunLoopSourceTypeID);
#if defined(__MACH__)
    CFAssert1(0 == context->version || 1 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0 or 1", __PRETTY_FUNCTION__);
    memmove(context, &rls->_context, (0 == context->version) ? sizeof(CFRunLoopSourceContext) : sizeof(CFRunLoopSourceContext1));
#else
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    memmove(context, &rls->_context, sizeof(CFRunLoopSourceContext));
#endif
}

void CFRunLoopSourceSignal(CFRunLoopSourceRef rls) {
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls)) {
	__CFRunLoopSourceSetSignaled(rls);
    }
    __CFRunLoopSourceUnlock(rls);
}


/* CFRunLoopObserver */

static CFStringRef __CFRunLoopObserverCopyDescription(CFTypeRef cf) {	/* DOES CALLOUT */
    CFRunLoopObserverRef rlo = (CFRunLoopObserverRef)cf;
    CFStringRef result;
    CFStringRef contextDesc = NULL;
    __CFRunLoopObserverLock(rlo);
    if (NULL != rlo->_context.copyDescription) {
	contextDesc = rlo->_context.copyDescription(rlo->_context.info);
    }
    if (!contextDesc) {
	contextDesc = CFStringCreateWithFormat(CFGetAllocator(rlo), NULL, CFSTR("<CFRunLoopObserver context %p>"), rlo->_context.info);
    }
    result = CFStringCreateWithFormat(CFGetAllocator(rlo), NULL, CFSTR("<CFRunLoopObserver %p [%p]>{locked = %s, valid = %s, activities = 0x%x, repeats = %s, order = %d, callout = %p, context = %@}"), cf, CFGetAllocator(rlo), rlo->_lock ? "Yes" : "No", __CFIsValid(rlo) ? "Yes" : "No", rlo->_activities, __CFRunLoopObserverRepeats(rlo) ? "Yes" : "No", rlo->_order, rlo->_callout, contextDesc);
    __CFRunLoopObserverUnlock(rlo);
    CFRelease(contextDesc);
    return result;
}

static void __CFRunLoopObserverDeallocate(CFTypeRef cf) {	/* DOES CALLOUT */
    CFRunLoopObserverRef rlo = (CFRunLoopObserverRef)cf;
    CFRunLoopObserverInvalidate(rlo);
}

static const CFRuntimeClass __CFRunLoopObserverClass = {
    0,
    "CFRunLoopObserver",
    NULL,      // init
    NULL,      // copy
    __CFRunLoopObserverDeallocate,
    NULL,
    NULL,
    NULL,      // 
    __CFRunLoopObserverCopyDescription
};

__private_extern__ void __CFRunLoopObserverInitialize(void) {
    __kCFRunLoopObserverTypeID = _CFRuntimeRegisterClass(&__CFRunLoopObserverClass);
}

CFTypeID CFRunLoopObserverGetTypeID(void) {
    return __kCFRunLoopObserverTypeID;
}

CFRunLoopObserverRef CFRunLoopObserverCreate(CFAllocatorRef allocator, CFOptionFlags activities, Boolean repeats, CFIndex order, CFRunLoopObserverCallBack callout, CFRunLoopObserverContext *context) {
    CFRunLoopObserverRef memory;
    UInt32 size;
    size = sizeof(struct __CFRunLoopObserver) - sizeof(CFRuntimeBase);
    memory = (CFRunLoopObserverRef)_CFRuntimeCreateInstance(allocator, __kCFRunLoopObserverTypeID, size, NULL);
    if (NULL == memory) {
	return NULL;
    }
    __CFSetValid(memory);
    __CFRunLoopObserverUnsetFiring(memory);
    if (repeats) {
	__CFRunLoopObserverSetRepeats(memory);
    } else {
	__CFRunLoopObserverUnsetRepeats(memory);
    }
    memory->_lock = 0;
    memory->_runLoop = NULL;
    memory->_rlCount = 0;
    memory->_activities = activities;
    memory->_order = order;
    memory->_callout = callout;
    if (context) {
	if (context->retain) {
	    memory->_context.info = (void *)context->retain(context->info);
	} else {
	    memory->_context.info = context->info;
	}
	memory->_context.retain = context->retain;
	memory->_context.release = context->release;
	memory->_context.copyDescription = context->copyDescription;
    } else {
	memory->_context.info = 0;
	memory->_context.retain = 0;
	memory->_context.release = 0;
	memory->_context.copyDescription = 0;
    }
    return memory;
}

CFOptionFlags CFRunLoopObserverGetActivities(CFRunLoopObserverRef rlo) {
    __CFGenericValidateType(rlo, __kCFRunLoopObserverTypeID);
    return rlo->_activities;
}

CFIndex CFRunLoopObserverGetOrder(CFRunLoopObserverRef rlo) {
    __CFGenericValidateType(rlo, __kCFRunLoopObserverTypeID);
    return rlo->_order;
}

Boolean CFRunLoopObserverDoesRepeat(CFRunLoopObserverRef rlo) {
    __CFGenericValidateType(rlo, __kCFRunLoopObserverTypeID);
    return __CFRunLoopObserverRepeats(rlo);
}

void CFRunLoopObserverInvalidate(CFRunLoopObserverRef rlo) {	/* DOES CALLOUT */
    __CFGenericValidateType(rlo, __kCFRunLoopObserverTypeID);
    CFRetain(rlo);
    __CFRunLoopObserverLock(rlo);
    if (__CFIsValid(rlo)) {
	CFRunLoopRef rl = rlo->_runLoop;
	__CFUnsetValid(rlo);
	__CFRunLoopObserverUnlock(rlo);
	if (NULL != rl) {
	    CFArrayRef array;
	    CFIndex idx;
	    array = CFRunLoopCopyAllModes(rl);
	    for (idx = CFArrayGetCount(array); idx--;) {
		CFStringRef modeName = CFArrayGetValueAtIndex(array, idx);
		CFRunLoopRemoveObserver(rl, rlo, modeName);
	    }
	    CFRunLoopRemoveObserver(rl, rlo, kCFRunLoopCommonModes);
	    CFRelease(array);
	}
	if (rlo->_context.release)
	    rlo->_context.release(rlo->_context.info);	/* CALLOUT */
	rlo->_context.info = NULL;
    } else {
	__CFRunLoopObserverUnlock(rlo);
    }
    CFRelease(rlo);
}

Boolean CFRunLoopObserverIsValid(CFRunLoopObserverRef rlo) {
    return __CFIsValid(rlo);
}

void CFRunLoopObserverGetContext(CFRunLoopObserverRef rlo, CFRunLoopObserverContext *context) {
    __CFGenericValidateType(rlo, __kCFRunLoopObserverTypeID);
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    *context = rlo->_context;
}

/* CFRunLoopTimer */

static CFStringRef __CFRunLoopTimerCopyDescription(CFTypeRef cf) {	/* DOES CALLOUT */
    CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)cf;
    CFStringRef result;
    CFStringRef contextDesc = NULL;
    int64_t fireTime;
    __CFRunLoopTimerFireTSRLock();
    fireTime = rlt->_fireTSR;
    __CFRunLoopTimerFireTSRUnlock();
    __CFRunLoopTimerLock(rlt);
    if (NULL != rlt->_context.copyDescription) {
	contextDesc = rlt->_context.copyDescription(rlt->_context.info);
    }
    if (NULL == contextDesc) {
	contextDesc = CFStringCreateWithFormat(CFGetAllocator(rlt), NULL, CFSTR("<CFRunLoopTimer context %p>"), rlt->_context.info);
    }
    result = CFStringCreateWithFormat(CFGetAllocator(rlt), NULL, CFSTR("<CFRunLoopTimer %x [%x]>{locked = %s, valid = %s, interval = %0.09g, next fire date = %0.09g, order = %d, callout = %p, context = %@}"), cf, CFGetAllocator(rlt), rlt->_lock ? "Yes" : "No", __CFIsValid(rlt) ? "Yes" : "No", __CFTSRToTimeInterval(rlt->_intervalTSR), __CFTSRToAbsoluteTime(fireTime), rlt->_order, rlt->_callout, contextDesc);
    __CFRunLoopTimerUnlock(rlt);
    CFRelease(contextDesc);
    return result;
}

static void __CFRunLoopTimerDeallocate(CFTypeRef cf) {	/* DOES CALLOUT */
    CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)cf;
    CFRunLoopTimerInvalidate(rlt);	/* DOES CALLOUT */
}

static const CFRuntimeClass __CFRunLoopTimerClass = {
    0,
    "CFRunLoopTimer",
    NULL,      // init
    NULL,      // copy
    __CFRunLoopTimerDeallocate,
    NULL,	// equal
    NULL,
    NULL,      // 
    __CFRunLoopTimerCopyDescription
};

__private_extern__ void __CFRunLoopTimerInitialize(void) {
    __kCFRunLoopTimerTypeID = _CFRuntimeRegisterClass(&__CFRunLoopTimerClass);
}

CFTypeID CFRunLoopTimerGetTypeID(void) {
    return __kCFRunLoopTimerTypeID;
}

CFRunLoopTimerRef CFRunLoopTimerCreate(CFAllocatorRef allocator, CFAbsoluteTime fireDate, CFTimeInterval interval, CFOptionFlags flags, CFIndex order, CFRunLoopTimerCallBack callout, CFRunLoopTimerContext *context) {
    CFRunLoopTimerRef memory;
    UInt32 size;
    size = sizeof(struct __CFRunLoopTimer) - sizeof(CFRuntimeBase);
    memory = (CFRunLoopTimerRef)_CFRuntimeCreateInstance(allocator, __kCFRunLoopTimerTypeID, size, NULL);
    if (NULL == memory) {
	return NULL;
    }
    __CFSetValid(memory);
    __CFRunLoopTimerUnsetFiring(memory);
    memory->_lock = 0;
    memory->_runLoop = NULL;
    memory->_rlCount = 0;
#if defined(__MACH__)
    memory->_port = MACH_PORT_NULL;
#endif
    memory->_order = order;
    if (fireDate < __CFTSRToAbsoluteTime(0)) {
	memory->_fireTSR = 0;
    } else if (__CFTSRToAbsoluteTime(LLONG_MAX) < fireDate) {
	memory->_fireTSR = LLONG_MAX;
    } else {
	memory->_fireTSR = __CFAbsoluteTimeToTSR(fireDate);
    }
    if (interval <= 0.0) {
	memory->_intervalTSR = 0;
    } else if (__CFTSRToTimeInterval(LLONG_MAX) < interval) {
	memory->_intervalTSR = LLONG_MAX;
    } else {
	memory->_intervalTSR = __CFTimeIntervalToTSR(interval);
    }
    memory->_callout = callout;
    if (NULL != context) {
	if (context->retain) {
	    memory->_context.info = (void *)context->retain(context->info);
	} else {
	    memory->_context.info = context->info;
	}
	memory->_context.retain = context->retain;
	memory->_context.release = context->release;
	memory->_context.copyDescription = context->copyDescription;
    } else {
	memory->_context.info = 0;
	memory->_context.retain = 0;
	memory->_context.release = 0;
	memory->_context.copyDescription = 0;
    }
    return memory;
}

CFAbsoluteTime CFRunLoopTimerGetNextFireDate(CFRunLoopTimerRef rlt) {
    int64_t fireTime, result = 0;
    CF_OBJC_FUNCDISPATCH0(__kCFRunLoopTimerTypeID, CFAbsoluteTime, rlt, "_cffireTime");
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    __CFRunLoopTimerFireTSRLock();
    fireTime = rlt->_fireTSR;
    __CFRunLoopTimerFireTSRUnlock();
    __CFRunLoopTimerLock(rlt);
    if (__CFIsValid(rlt)) {
	result = fireTime;
    }
    __CFRunLoopTimerUnlock(rlt);
    return __CFTSRToAbsoluteTime(result);
}

void CFRunLoopTimerSetNextFireDate(CFRunLoopTimerRef rlt, CFAbsoluteTime fireDate) {
    __CFRunLoopTimerFireTSRLock();
    if (fireDate < __CFTSRToAbsoluteTime(0)) {
	rlt->_fireTSR = 0;
    } else if (__CFTSRToAbsoluteTime(LLONG_MAX) < fireDate) {
	rlt->_fireTSR = LLONG_MAX;
    } else {
	rlt->_fireTSR = __CFAbsoluteTimeToTSR(fireDate);
     }
    __CFRunLoopTimerFireTSRUnlock();
    if (rlt->_runLoop != NULL) {
	__CFRunLoopTimerRescheduleWithAllModes(rlt, rlt->_runLoop);
    }
}

CFTimeInterval CFRunLoopTimerGetInterval(CFRunLoopTimerRef rlt) {
    CF_OBJC_FUNCDISPATCH0(__kCFRunLoopTimerTypeID, CFTimeInterval, rlt, "timeInterval");
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    return __CFTSRToTimeInterval(rlt->_intervalTSR);
}

Boolean CFRunLoopTimerDoesRepeat(CFRunLoopTimerRef rlt) {
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    return (0 != rlt->_intervalTSR);
}

CFIndex CFRunLoopTimerGetOrder(CFRunLoopTimerRef rlt) {
    CF_OBJC_FUNCDISPATCH0(__kCFRunLoopTimerTypeID, CFIndex, rlt, "order");
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    return rlt->_order;
}

void CFRunLoopTimerInvalidate(CFRunLoopTimerRef rlt) {	/* DOES CALLOUT */
    CF_OBJC_FUNCDISPATCH0(__kCFRunLoopTimerTypeID, void, rlt, "invalidate");
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    CFRetain(rlt);
    __CFRunLoopTimerLock(rlt);
    if (__CFIsValid(rlt)) {
	CFRunLoopRef rl = rlt->_runLoop;
	void *info = rlt->_context.info;
	__CFUnsetValid(rlt);
#if defined(__MACH__)
	__CFRunLoopTimerPortMapLock();
	if (NULL != __CFRLTPortMap) {
	    CFDictionaryRemoveValue(__CFRLTPortMap, (void *)rlt->_port);
	}
	__CFRunLoopTimerPortMapUnlock();
	mk_timer_destroy(rlt->_port);
	rlt->_port = MACH_PORT_NULL;
#endif
	rlt->_context.info = NULL;
	__CFRunLoopTimerUnlock(rlt);
	if (NULL != rl) {
	    CFArrayRef array;
	    CFIndex idx;
	    array = CFRunLoopCopyAllModes(rl);
	    for (idx = CFArrayGetCount(array); idx--;) {
		CFStringRef modeName = CFArrayGetValueAtIndex(array, idx);
		CFRunLoopRemoveTimer(rl, rlt, modeName);
	    }
	    CFRunLoopRemoveTimer(rl, rlt, kCFRunLoopCommonModes);
	    CFRelease(array);
	}
	if (NULL != rlt->_context.release) {
	    rlt->_context.release(info);	/* CALLOUT */
	}
    } else {
	__CFRunLoopTimerUnlock(rlt);
    }
    CFRelease(rlt);
}

Boolean CFRunLoopTimerIsValid(CFRunLoopTimerRef rlt) {
    CF_OBJC_FUNCDISPATCH0(__kCFRunLoopTimerTypeID, Boolean, rlt, "isValid");
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    return __CFIsValid(rlt);
}

void CFRunLoopTimerGetContext(CFRunLoopTimerRef rlt, CFRunLoopTimerContext *context) {
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    *context = rlt->_context;
}

struct rlpair {
    CFRunLoopRef rl;	// not retained
    CFStringRef mode;	// not retained
};

static Boolean __CFRLPKeyEqual(const void *value1, const void *value2) {
    const struct rlpair *s1 = value1;
    const struct rlpair *s2 = value2;
    return (s1->rl == s2->rl) && CFEqual(s1->mode, s2->mode);
}

static CFHashCode __CFRLPKeyHash(const void *value) {
    const struct rlpair *s = value;
    return (CFHashCode)s->rl + CFHash(s->mode);
}

static CFSpinLock_t __CFRunLoopPerformLock = 0;
static CFMutableDictionaryRef __CFRunLoopPerformSources = NULL;

struct performentry {
    CFRunLoopPerformCallBack callout;
    void *info;
};

struct performinfo {
    CFSpinLock_t lock;
    CFRunLoopSourceRef source;
    CFRunLoopRef rl;
    CFStringRef mode;
    CFIndex count;
    CFIndex size;
    struct performentry *entries;
};

static void __CFRunLoopPerformCancel(void *info, CFRunLoopRef rl, CFStringRef mode) {
    // we don't ever remove the source, so we know the run loop is dying
    struct rlpair key, *pair;
    struct performinfo *pinfo = info;
    __CFSpinLock(&__CFRunLoopPerformLock);
    key.rl = rl;
    key.mode = mode;
    if (CFDictionaryGetKeyIfPresent(__CFRunLoopPerformSources, &key, (const void **)&pair)) {
	CFDictionaryRemoveValue(__CFRunLoopPerformSources, pair);
	CFAllocatorDeallocate(kCFAllocatorSystemDefault, pair);
    }
    CFRunLoopSourceInvalidate(pinfo->source);
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, pinfo->entries);
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, pinfo);
    // We can free pinfo here, even though the source isn't freed and still has
    // a weak pointer to it, because the hash and equal callbacks of the source
    // don't indirect into the info for their operations.
    __CFSpinUnlock(&__CFRunLoopPerformLock);
}

static void __CFRunLoopPerformPerform(void *info) {
    struct performinfo *pinfo = info;
    struct performentry *entries;
    CFIndex idx, cnt;
    __CFSpinLock(&(pinfo->lock));
    entries = pinfo->entries;
    cnt = pinfo->count;
    pinfo->entries = NULL;
    pinfo->count = 0;
    pinfo->size = 0;
    __CFSpinUnlock(&(pinfo->lock));
    for (idx = 0; idx < cnt; idx++) {
	entries[idx].callout(entries[idx].info);
    }
    // done with this list
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, entries);
    // don't need to check to see if there's still something in the queue,
    // and resignal here, since anything added during the callouts,
    // from this or another thread, would have caused resignalling
}

// retaining and freeing the info pointer and stuff inside is completely
// the caller's (and probably the callout's) responsibility
void _CFRunLoopPerformEnqueue(CFRunLoopRef rl, CFStringRef mode, CFRunLoopPerformCallBack callout, void *info) {
    CFRunLoopSourceContext context = {0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    CFRunLoopSourceRef source;
    struct rlpair key;
    struct performinfo *pinfo;
    __CFSpinLock(&__CFRunLoopPerformLock);
    if (!__CFRunLoopPerformSources) {
	CFDictionaryKeyCallBacks kcb = {0, NULL, NULL, NULL, __CFRLPKeyEqual, __CFRLPKeyHash};
	__CFRunLoopPerformSources = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kcb, &kCFTypeDictionaryValueCallBacks);
    }
    key.rl = rl;
    key.mode = mode;
    if (!CFDictionaryGetValueIfPresent(__CFRunLoopPerformSources, &key, (const void **)&source)) {
	struct rlpair *pair;
	context.info = CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(struct performinfo), 0);
	pinfo = context.info;
	pinfo->lock = 0;
	pinfo->rl = rl;
	pinfo->mode = mode;
	pinfo->count = 0;
	pinfo->size = 0;
	pinfo->entries = NULL;
	context.cancel = __CFRunLoopPerformCancel;
	context.perform = __CFRunLoopPerformPerform;
	source = CFRunLoopSourceCreate(kCFAllocatorSystemDefault, 0, &context);
	pair = CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(*pair), 0);
	*pair = key;
	CFDictionarySetValue(__CFRunLoopPerformSources, pair, source);
	pinfo->source = source;
	CFRunLoopAddSource(rl, source, mode);
    } else {
	CFRetain(source);
	CFRunLoopSourceGetContext(source, &context);
	pinfo = context.info;
    }
    __CFSpinLock(&(pinfo->lock));
    __CFSpinUnlock(&__CFRunLoopPerformLock);
    if (pinfo->count == pinfo->size) {
	pinfo->size = (0 == pinfo->size ? 3 : 2 * pinfo->size);
	pinfo->entries = CFAllocatorReallocate(kCFAllocatorSystemDefault, pinfo->entries, pinfo->size * sizeof(struct performentry), 0);
    }
    pinfo->entries[pinfo->count].callout = callout;
    pinfo->entries[pinfo->count].info = info;
    pinfo->count++;
    __CFSpinUnlock(&(pinfo->lock));
    CFRunLoopSourceSignal(source);
    CFRunLoopWakeUp(rl);
    CFRelease(source);
}

