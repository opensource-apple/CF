/*
 * Copyright (c) 2012 Apple Inc. All rights reserved.
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

/*	CFMachPort.c
	Copyright (c) 1998-2012, Apple Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFMachPort.h>
#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFArray.h>
#include <dispatch/dispatch.h>
#include <dispatch/private.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include "CFInternal.h"


#define AVOID_WEAK_COLLECTIONS 1

#if !AVOID_WEAK_COLLECTIONS
#import "CFPointerArray.h"
#endif

static dispatch_queue_t _CFMachPortQueue() {
    static volatile dispatch_queue_t __CFMachPortQueue = NULL;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{ __CFMachPortQueue = dispatch_queue_create("CFMachPort Queue", NULL); });
    return __CFMachPortQueue;
}


enum {
    kCFMachPortStateReady = 0,
    kCFMachPortStateInvalidating = 1,
    kCFMachPortStateInvalid = 2,
    kCFMachPortStateDeallocating = 3
};

struct __CFMachPort {
    CFRuntimeBase _base;
    int32_t _state;
    mach_port_t _port;                          /* immutable */
    dispatch_source_t _dsrc;                    /* protected by _lock */
    dispatch_source_t _dsrc2;                   /* protected by _lock */
    dispatch_semaphore_t _dsrc_sem;             /* protected by _lock */
    dispatch_semaphore_t _dsrc2_sem;            /* protected by _lock */
    CFMachPortInvalidationCallBack _icallout;   /* protected by _lock */
    CFRunLoopSourceRef _source;                 /* immutable, once created */
    CFMachPortCallBack _callout;                /* immutable */
    CFMachPortContext _context;                 /* immutable */
    CFSpinLock_t _lock;
};

/* Bit 1 in the base reserved bits is used for has-receive-ref state */
/* Bit 2 in the base reserved bits is used for has-send-ref state */
/* Bit 3 in the base reserved bits is used for has-send-ref2 state */

CF_INLINE Boolean __CFMachPortHasReceive(CFMachPortRef mp) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)mp)->_cfinfo[CF_INFO_BITS], 1, 1);
}

CF_INLINE void __CFMachPortSetHasReceive(CFMachPortRef mp) {
    __CFBitfieldSetValue(((CFRuntimeBase *)mp)->_cfinfo[CF_INFO_BITS], 1, 1, 1);
}

CF_INLINE Boolean __CFMachPortHasSend(CFMachPortRef mp) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)mp)->_cfinfo[CF_INFO_BITS], 2, 2);
}

CF_INLINE void __CFMachPortSetHasSend(CFMachPortRef mp) {
    __CFBitfieldSetValue(((CFRuntimeBase *)mp)->_cfinfo[CF_INFO_BITS], 2, 2, 1);
}

CF_INLINE Boolean __CFMachPortHasSend2(CFMachPortRef mp) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)mp)->_cfinfo[CF_INFO_BITS], 3, 3);
}

CF_INLINE void __CFMachPortSetHasSend2(CFMachPortRef mp) {
    __CFBitfieldSetValue(((CFRuntimeBase *)mp)->_cfinfo[CF_INFO_BITS], 3, 3, 1);
}

CF_INLINE Boolean __CFMachPortIsValid(CFMachPortRef mp) {
    return kCFMachPortStateReady == mp->_state;
}


void _CFMachPortInstallNotifyPort(CFRunLoopRef rl, CFStringRef mode) {
}

static Boolean __CFMachPortEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFMachPortRef mp1 = (CFMachPortRef)cf1;
    CFMachPortRef mp2 = (CFMachPortRef)cf2;
    return (mp1->_port == mp2->_port);
}

static CFHashCode __CFMachPortHash(CFTypeRef cf) {
    CFMachPortRef mp = (CFMachPortRef)cf;
    return (CFHashCode)mp->_port;
}

static CFStringRef __CFMachPortCopyDescription(CFTypeRef cf) {
    CFMachPortRef mp = (CFMachPortRef)cf;
    CFStringRef contextDesc = NULL;
    if (NULL != mp->_context.info && NULL != mp->_context.copyDescription) {
        contextDesc = mp->_context.copyDescription(mp->_context.info);
    }
    if (NULL == contextDesc) {
        contextDesc = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFMachPort context %p>"), mp->_context.info);
    }
    Dl_info info;
    void *addr = mp->_callout;
    const char *name = (dladdr(addr, &info) && info.dli_saddr == addr && info.dli_sname) ? info.dli_sname : "???";
    CFStringRef result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFMachPort %p [%p]>{valid = %s, port = %p, source = %p, callout = %s (%p), context = %@}"), cf, CFGetAllocator(mp), (__CFMachPortIsValid(mp) ? "Yes" : "No"), mp->_port, mp->_source, name, addr, contextDesc);
    if (NULL != contextDesc) {
        CFRelease(contextDesc);
    }
    return result;
}

// Only call with mp->_lock locked
CF_INLINE void __CFMachPortInvalidateLocked(CFRunLoopSourceRef source, CFMachPortRef mp) {
    CFMachPortInvalidationCallBack cb = mp->_icallout;
    if (cb) {
        __CFSpinUnlock(&mp->_lock);
        cb(mp, mp->_context.info);
        __CFSpinLock(&mp->_lock);
    }
    if (NULL != source) {
        __CFSpinUnlock(&mp->_lock);
        CFRunLoopSourceInvalidate(source);
        CFRelease(source);
        __CFSpinLock(&mp->_lock);
    }
    void *info = mp->_context.info;
    mp->_context.info = NULL;
    if (mp->_context.release) {
        __CFSpinUnlock(&mp->_lock);
        mp->_context.release(info);
        __CFSpinLock(&mp->_lock);
    }
    mp->_state = kCFMachPortStateInvalid;
    OSMemoryBarrier();
}

static void __CFMachPortDeallocate(CFTypeRef cf) {
    CHECK_FOR_FORK_RET();
    CFMachPortRef mp = (CFMachPortRef)cf;

    // CFMachPortRef is invalid before we get here, except under GC
    __CFSpinLock(&mp->_lock);
    CFRunLoopSourceRef source = NULL;
    Boolean wasReady = (mp->_state == kCFMachPortStateReady);
    if (wasReady) {
        mp->_state = kCFMachPortStateInvalidating;
        OSMemoryBarrier();
        if (mp->_dsrc) {
            dispatch_source_cancel(mp->_dsrc);
            mp->_dsrc = NULL;
        }
        if (mp->_dsrc2) {
            dispatch_source_cancel(mp->_dsrc2);
            mp->_dsrc2 = NULL;
        }
        source = mp->_source;
        mp->_source = NULL;
    }    
    if (wasReady) {
        __CFMachPortInvalidateLocked(source, mp);
    }
    mp->_state = kCFMachPortStateDeallocating;

    // hand ownership of the port and semaphores to the block below
    mach_port_t port = mp->_port;
    dispatch_semaphore_t sem1 = mp->_dsrc_sem;
    dispatch_semaphore_t sem2 = mp->_dsrc2_sem;
    Boolean doSend2 = __CFMachPortHasSend2(mp), doSend = __CFMachPortHasSend(mp), doReceive = __CFMachPortHasReceive(mp);

    __CFSpinUnlock(&mp->_lock);

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0), ^{
            if (sem1) {
                dispatch_semaphore_wait(sem1, DISPATCH_TIME_FOREVER);
                // immediate release is only safe if dispatch_semaphore_signal() does not touch the semaphore after doing the signal bit
                dispatch_release(sem1);
            }
            if (sem2) {
                dispatch_semaphore_wait(sem2, DISPATCH_TIME_FOREVER);
                // immediate release is only safe if dispatch_semaphore_signal() does not touch the semaphore after doing the signal bit
                dispatch_release(sem2);
            }

            // MUST deallocate the send right FIRST if necessary,
            // then the receive right if necessary.  Don't ask me why;
            // if it's done in the other order the port will leak.
            if (doSend2) {
                mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, -1);
            }
            if (doSend) {
                mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, -1);
            }
            if (doReceive) {
                mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
            }
        });
}

// This lock protects __CFAllMachPorts. Take before any instance-specific lock.
static CFSpinLock_t __CFAllMachPortsLock = CFSpinLockInit;

#if AVOID_WEAK_COLLECTIONS
static CFMutableArrayRef __CFAllMachPorts = NULL;
#else
static __CFPointerArray *__CFAllMachPorts = nil;
#endif

static Boolean __CFMachPortCheck(mach_port_t port) {
    mach_port_type_t type = 0;
    kern_return_t ret = mach_port_type(mach_task_self(), port, &type);
    return (KERN_SUCCESS != ret || (0 == (type & MACH_PORT_TYPE_PORT_RIGHTS))) ? false : true;
}

static void __CFMachPortChecker(Boolean fromTimer) {
    __CFSpinLock(&__CFAllMachPortsLock); // take this lock first before any instance-specific lock
#if AVOID_WEAK_COLLECTIONS
    for (CFIndex idx = 0, cnt = __CFAllMachPorts ? CFArrayGetCount(__CFAllMachPorts) : 0; idx < cnt; idx++) {
        CFMachPortRef mp = (CFMachPortRef)CFArrayGetValueAtIndex(__CFAllMachPorts, idx);
#else
    for (CFIndex idx = 0, cnt = __CFAllMachPorts ? [__CFAllMachPorts count] : 0; idx < cnt; idx++) {
        CFMachPortRef mp = (CFMachPortRef)[__CFAllMachPorts pointerAtIndex:idx];
#endif
        if (!mp) continue;
        // second clause cleans no-longer-wanted CFMachPorts out of our strong table
        if (!__CFMachPortCheck(mp->_port) || (!kCFUseCollectableAllocator && 1 == CFGetRetainCount(mp))) {
            CFRunLoopSourceRef source = NULL;
            Boolean wasReady = (mp->_state == kCFMachPortStateReady);
            if (wasReady) {
                __CFSpinLock(&mp->_lock); // take this lock second
                mp->_state = kCFMachPortStateInvalidating;
                OSMemoryBarrier();
                if (mp->_dsrc) {
                    dispatch_source_cancel(mp->_dsrc);
                    mp->_dsrc = NULL;
                }
                if (mp->_dsrc2) {
                    dispatch_source_cancel(mp->_dsrc2);
                    mp->_dsrc2 = NULL;
                }
                source = mp->_source;
                mp->_source = NULL;
                CFRetain(mp);
                __CFSpinUnlock(&mp->_lock);
                dispatch_async(dispatch_get_main_queue(), ^{
                    // We can grab the mach port-specific spin lock here since we're no longer on the same thread as the one taking the all mach ports spin lock.
                    // But be sure to release it during callouts
                    __CFSpinLock(&mp->_lock);
                    __CFMachPortInvalidateLocked(source, mp);
                    __CFSpinUnlock(&mp->_lock);
                    CFRelease(mp);
                });
            }
#if AVOID_WEAK_COLLECTIONS
            CFArrayRemoveValueAtIndex(__CFAllMachPorts, idx);
#else
            [__CFAllMachPorts removePointerAtIndex:idx];
#endif
            idx--;
            cnt--;
        }
    }
#if !AVOID_WEAK_COLLECTIONS
    [__CFAllMachPorts compact];
#endif
    __CFSpinUnlock(&__CFAllMachPortsLock);
};


static CFTypeID __kCFMachPortTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFMachPortClass = {
    0,
    "CFMachPort",
    NULL,      // init
    NULL,      // copy
    __CFMachPortDeallocate,
    __CFMachPortEqual,
    __CFMachPortHash,
    NULL,      // 
    __CFMachPortCopyDescription
};

__private_extern__ void __CFMachPortInitialize(void) {
    __kCFMachPortTypeID = _CFRuntimeRegisterClass(&__CFMachPortClass);
}

CFTypeID CFMachPortGetTypeID(void) {
    return __kCFMachPortTypeID;
}

/* Note: any receive or send rights that the port contains coming in will
 * not be cleaned up by CFMachPort; it will increment and decrement
 * references on the port if the kernel ever allows that in the future,
 * but will not cleanup any references you got when you got the port. */
CFMachPortRef _CFMachPortCreateWithPort2(CFAllocatorRef allocator, mach_port_t port, CFMachPortCallBack callout, CFMachPortContext *context, Boolean *shouldFreeInfo, Boolean deathWatch) {
    if (shouldFreeInfo) *shouldFreeInfo = true;
    CHECK_FOR_FORK_RET(NULL);

    mach_port_type_t type = 0;
    kern_return_t ret = mach_port_type(mach_task_self(), port, &type);
    if (KERN_SUCCESS != ret || (0 == (type & MACH_PORT_TYPE_PORT_RIGHTS))) {
        if (type & ~MACH_PORT_TYPE_DEAD_NAME) {
            CFLog(kCFLogLevelError, CFSTR("*** CFMachPortCreateWithPort(): bad Mach port parameter (0x%lx) or unsupported mysterious kind of Mach port (%d, %ld)"), (unsigned long)port, ret, (unsigned long)type);
        }
        return NULL;
    }

    static dispatch_source_t timerSource = NULL;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        uint64_t nanos = 63 * 1000 * 1000 * 1000ULL;
        uint64_t leeway = 9 * 1000ULL;
        timerSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _CFMachPortQueue());
        dispatch_source_set_timer(timerSource, dispatch_time(DISPATCH_TIME_NOW, nanos), nanos, leeway);
        dispatch_source_set_event_handler(timerSource, ^{
            __CFMachPortChecker(true);
        });
        dispatch_resume(timerSource);
    });

    CFMachPortRef mp = NULL;
    __CFSpinLock(&__CFAllMachPortsLock);
#if AVOID_WEAK_COLLECTIONS
    for (CFIndex idx = 0, cnt = __CFAllMachPorts ? CFArrayGetCount(__CFAllMachPorts) : 0; idx < cnt; idx++) {
        CFMachPortRef p = (CFMachPortRef)CFArrayGetValueAtIndex(__CFAllMachPorts, idx);
        if (p && p->_port == port) {
            CFRetain(p);
            mp = p;
            break;
        }
    }
#else                
    for (CFIndex idx = 0, cnt = __CFAllMachPorts ? [__CFAllMachPorts count] : 0; idx < cnt; idx++) {
        CFMachPortRef p = (CFMachPortRef)[__CFAllMachPorts pointerAtIndex:idx];
        if (p && p->_port == port) {
            CFRetain(p);
            mp = p;
            break;
        }
    }
#endif
    __CFSpinUnlock(&__CFAllMachPortsLock);
    
    if (!mp) {
        CFIndex size = sizeof(struct __CFMachPort) - sizeof(CFRuntimeBase);
        CFMachPortRef memory = (CFMachPortRef)_CFRuntimeCreateInstance(allocator, CFMachPortGetTypeID(), size, NULL);
        if (NULL == memory) {
            return NULL;
        }
        memory->_port = port;
        memory->_dsrc = NULL;
        memory->_dsrc2 = NULL;
        memory->_dsrc_sem = NULL;
        memory->_dsrc2_sem = NULL;
        memory->_icallout = NULL;
        memory->_source = NULL;
        memory->_context.info = NULL;
        memory->_context.retain = NULL;
        memory->_context.release = NULL;
        memory->_context.copyDescription = NULL;
        memory->_callout = callout;
        memory->_lock = CFSpinLockInit;
        if (NULL != context) {
            objc_memmove_collectable(&memory->_context, context, sizeof(CFMachPortContext));
            memory->_context.info = context->retain ? (void *)context->retain(context->info) : context->info;
        }
        memory->_state = kCFMachPortStateReady;
        __CFSpinLock(&__CFAllMachPortsLock);
    #if AVOID_WEAK_COLLECTIONS
        if (!__CFAllMachPorts) __CFAllMachPorts = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
        CFArrayAppendValue(__CFAllMachPorts, memory);
    #else
        if (!__CFAllMachPorts) __CFAllMachPorts = [[__CFPointerArray alloc] initWithOptions:(kCFUseCollectableAllocator ? CFPointerFunctionsZeroingWeakMemory : CFPointerFunctionsStrongMemory)];
        [__CFAllMachPorts addPointer:memory];
    #endif
        __CFSpinUnlock(&__CFAllMachPortsLock);
        mp = memory;
        if (shouldFreeInfo) *shouldFreeInfo = false;

        if (type & MACH_PORT_TYPE_SEND_RIGHTS) {
            dispatch_source_t theSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_SEND, port, DISPATCH_MACH_SEND_DEAD, _CFMachPortQueue());
            dispatch_source_set_cancel_handler(theSource, ^{
                dispatch_release(theSource);
            });
            dispatch_source_set_event_handler(theSource, ^{
                __CFMachPortChecker(false);
            });
            memory->_dsrc = theSource;
            dispatch_resume(theSource);
        }
        if (memory->_dsrc) {
            dispatch_source_t source = memory->_dsrc; // put these in locals so they are fully copied into the block
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            memory->_dsrc_sem = sem;
            dispatch_source_set_cancel_handler(memory->_dsrc, ^{ dispatch_semaphore_signal(sem); dispatch_release(source); });
        }
        if (memory->_dsrc2) {
            dispatch_source_t source = memory->_dsrc2;
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            memory->_dsrc2_sem = sem;
            dispatch_source_set_cancel_handler(memory->_dsrc2, ^{ dispatch_semaphore_signal(sem); dispatch_release(source); });
        }
    }
    
    if (mp && !CFMachPortIsValid(mp)) { // must do this outside lock to avoid deadlock
        CFRelease(mp);
        mp = NULL;
    }
    return mp;
}

CFMachPortRef CFMachPortCreateWithPort(CFAllocatorRef allocator, mach_port_t port, CFMachPortCallBack callout, CFMachPortContext *context, Boolean *shouldFreeInfo) {
    return _CFMachPortCreateWithPort2(allocator, port, callout, context, shouldFreeInfo, true);
}

CFMachPortRef CFMachPortCreate(CFAllocatorRef allocator, CFMachPortCallBack callout, CFMachPortContext *context, Boolean *shouldFreeInfo) {
    if (shouldFreeInfo) *shouldFreeInfo = true;
    CHECK_FOR_FORK_RET(NULL);
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (KERN_SUCCESS == ret) {
        ret = mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    }
    if (KERN_SUCCESS != ret) {
        if (MACH_PORT_NULL != port) mach_port_destroy(mach_task_self(), port);
        return NULL;
    }
    CFMachPortRef result = _CFMachPortCreateWithPort2(allocator, port, callout, context, shouldFreeInfo, true);
    if (NULL == result) {
        if (MACH_PORT_NULL != port) mach_port_destroy(mach_task_self(), port);
        return NULL;
    }
    __CFMachPortSetHasReceive(result);
    __CFMachPortSetHasSend(result);
    return result;
}

void CFMachPortInvalidate(CFMachPortRef mp) {
    CHECK_FOR_FORK_RET();
    CF_OBJC_FUNCDISPATCHV(CFMachPortGetTypeID(), void, (NSMachPort *)mp, invalidate);
    __CFGenericValidateType(mp, CFMachPortGetTypeID());
    CFRetain(mp);
    CFRunLoopSourceRef source = NULL;
    Boolean wasReady = false;
    __CFSpinLock(&__CFAllMachPortsLock); // take this lock first
    __CFSpinLock(&mp->_lock);
    wasReady = (mp->_state == kCFMachPortStateReady);
    if (wasReady) {
        mp->_state = kCFMachPortStateInvalidating;
        OSMemoryBarrier();
#if AVOID_WEAK_COLLECTIONS
        for (CFIndex idx = 0, cnt = __CFAllMachPorts ? CFArrayGetCount(__CFAllMachPorts) : 0; idx < cnt; idx++) {
            CFMachPortRef p = (CFMachPortRef)CFArrayGetValueAtIndex(__CFAllMachPorts, idx);
            if (p == mp) {
                CFArrayRemoveValueAtIndex(__CFAllMachPorts, idx);
                break;
            }
        }
#else
        for (CFIndex idx = 0, cnt = __CFAllMachPorts ? [__CFAllMachPorts count] : 0; idx < cnt; idx++) {
            CFMachPortRef p = (CFMachPortRef)[__CFAllMachPorts pointerAtIndex:idx];
            if (p == mp) {
                [__CFAllMachPorts removePointerAtIndex:idx];
                break;
            }
        }
#endif        
        if (mp->_dsrc) {
            dispatch_source_cancel(mp->_dsrc);
            mp->_dsrc = NULL;
        }
        if (mp->_dsrc2) {
            dispatch_source_cancel(mp->_dsrc2);
            mp->_dsrc2 = NULL;
        }
        source = mp->_source;
        mp->_source = NULL;
    }
    __CFSpinUnlock(&mp->_lock);
    __CFSpinUnlock(&__CFAllMachPortsLock); // release this lock last
    if (wasReady) {
        __CFSpinLock(&mp->_lock);
        __CFMachPortInvalidateLocked(source, mp);
        __CFSpinUnlock(&mp->_lock);
    }
    CFRelease(mp);
}

mach_port_t CFMachPortGetPort(CFMachPortRef mp) {
    CHECK_FOR_FORK_RET(0);
    CF_OBJC_FUNCDISPATCHV(CFMachPortGetTypeID(), mach_port_t, (NSMachPort *)mp, machPort);
    __CFGenericValidateType(mp, CFMachPortGetTypeID());
    return mp->_port;
}

void CFMachPortGetContext(CFMachPortRef mp, CFMachPortContext *context) {
    __CFGenericValidateType(mp, CFMachPortGetTypeID());
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    objc_memmove_collectable(context, &mp->_context, sizeof(CFMachPortContext));
}

Boolean CFMachPortIsValid(CFMachPortRef mp) {
    CF_OBJC_FUNCDISPATCHV(CFMachPortGetTypeID(), Boolean, (NSMachPort *)mp, isValid);
    __CFGenericValidateType(mp, CFMachPortGetTypeID());
    if (!__CFMachPortIsValid(mp)) return false;
    mach_port_type_t type = 0;
    kern_return_t ret = mach_port_type(mach_task_self(), mp->_port, &type);
    if (KERN_SUCCESS != ret || (type & ~(MACH_PORT_TYPE_SEND|MACH_PORT_TYPE_SEND_ONCE|MACH_PORT_TYPE_RECEIVE|MACH_PORT_TYPE_DNREQUEST))) {
	return false;
    }
    return true;
}

CFMachPortInvalidationCallBack CFMachPortGetInvalidationCallBack(CFMachPortRef mp) {
    __CFGenericValidateType(mp, CFMachPortGetTypeID());
    __CFSpinLock(&mp->_lock);
    CFMachPortInvalidationCallBack cb = mp->_icallout;
    __CFSpinUnlock(&mp->_lock);
    return cb;
}

/* After the CFMachPort has started going invalid, or done invalid, you can't change this, and
   we'll only do the callout directly on a transition from NULL to non-NULL. */
void CFMachPortSetInvalidationCallBack(CFMachPortRef mp, CFMachPortInvalidationCallBack callout) {
    CHECK_FOR_FORK_RET();
    __CFGenericValidateType(mp, CFMachPortGetTypeID());
    __CFSpinLock(&mp->_lock);
    if (__CFMachPortIsValid(mp) || !callout) {
        mp->_icallout = callout;
    } else if (!mp->_icallout && callout) {
        __CFSpinUnlock(&mp->_lock);
        callout(mp, mp->_context.info);
        __CFSpinLock(&mp->_lock);
    } else {
        CFLog(kCFLogLevelWarning, CFSTR("CFMachPortSetInvalidationCallBack(): attempt to set invalidation callback (%p) on invalid CFMachPort (%p) thwarted"), callout, mp);
    }
    __CFSpinUnlock(&mp->_lock);
}

/* Returns the number of messages queued for a receive port. */
CFIndex CFMachPortGetQueuedMessageCount(CFMachPortRef mp) {  
    CHECK_FOR_FORK_RET(0);
    __CFGenericValidateType(mp, CFMachPortGetTypeID());
    mach_port_status_t status;
    mach_msg_type_number_t num = MACH_PORT_RECEIVE_STATUS_COUNT;
    kern_return_t ret = mach_port_get_attributes(mach_task_self(), mp->_port, MACH_PORT_RECEIVE_STATUS, (mach_port_info_t)&status, &num);
    return (KERN_SUCCESS != ret) ? 0 : status.mps_msgcount;
}

static mach_port_t __CFMachPortGetPort(void *info) {
    CFMachPortRef mp = (CFMachPortRef)info;
    return mp->_port;
}

static void *__CFMachPortPerform(void *msg, CFIndex size, CFAllocatorRef allocator, void *info) {
    CHECK_FOR_FORK_RET(NULL);
    CFMachPortRef mp = (CFMachPortRef)info;
    __CFSpinLock(&mp->_lock);
    Boolean isValid = __CFMachPortIsValid(mp);
    void *context_info = NULL;
    void (*context_release)(const void *) = NULL;
    if (isValid) {
        if (mp->_context.retain) {
            context_info = (void *)mp->_context.retain(mp->_context.info);
            context_release = mp->_context.release;
        } else {
            context_info = mp->_context.info;
        }
    }
    __CFSpinUnlock(&mp->_lock);
    if (!isValid) return NULL;

    mp->_callout(mp, msg, size, context_info);

    if (context_release) {
        context_release(context_info);
    }
    CHECK_FOR_FORK_RET(NULL);
    return NULL;
}

CFRunLoopSourceRef CFMachPortCreateRunLoopSource(CFAllocatorRef allocator, CFMachPortRef mp, CFIndex order) {
    CHECK_FOR_FORK_RET(NULL);
    __CFGenericValidateType(mp, CFMachPortGetTypeID());
    if (!CFMachPortIsValid(mp)) return NULL;
    CFRunLoopSourceRef result = NULL;
    __CFSpinLock(&mp->_lock);
    if (__CFMachPortIsValid(mp)) {
        if (NULL != mp->_source && !CFRunLoopSourceIsValid(mp->_source)) {
            CFRelease(mp->_source);
            mp->_source = NULL;
        }
        if (NULL == mp->_source) {
            CFRunLoopSourceContext1 context;
            context.version = 1;
            context.info = (void *)mp;
            context.retain = (const void *(*)(const void *))CFRetain;
            context.release = (void (*)(const void *))CFRelease;
            context.copyDescription = (CFStringRef (*)(const void *))__CFMachPortCopyDescription;
            context.equal = (Boolean (*)(const void *, const void *))__CFMachPortEqual;
            context.hash = (CFHashCode (*)(const void *))__CFMachPortHash;
            context.getPort = __CFMachPortGetPort;
            context.perform = __CFMachPortPerform;
            mp->_source = CFRunLoopSourceCreate(allocator, order, (CFRunLoopSourceContext *)&context);
        }
        result = mp->_source ? (CFRunLoopSourceRef)CFRetain(mp->_source) : NULL;
    }
    __CFSpinUnlock(&mp->_lock);
    return result;
}

