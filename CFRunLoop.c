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

/*	CFRunLoop.c
	Copyright (c) 1998-2009, Apple Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFSet.h>
#include <CoreFoundation/CFBag.h>
#include "CFInternal.h"
#include <math.h>
#include <stdio.h>
#include <limits.h>
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/clock_types.h>
#include <mach/clock.h>
#include <unistd.h>
#include <dlfcn.h>
extern mach_port_t _dispatch_get_main_queue_port_4CF(void);
extern void _dispatch_main_queue_callback_4CF(mach_msg_header_t *msg);
#else
#include <process.h>
#define __CFReadTSR() mach_absolute_time()
#endif
#include <Block.h>

static int _LogCFRunLoop = 0;

// for conservative arithmetic safety, such that (TIMER_DATE_LIMIT + TIMER_INTERVAL_LIMIT + kCFAbsoluteTimeIntervalSince1970) * 10^9 < 2^63
#define TIMER_DATE_LIMIT	4039289856.0
#define TIMER_INTERVAL_LIMIT	504911232.0

#define HANDLE_DISPATCH_ON_BASE_INVOCATION_ONLY 0

#if DEPLOYMENT_TARGET_WINDOWS
static pthread_t kNilPthreadT = { nil, nil };
#define pthreadPointer(a) a.p

static DWORD __CFTSDKeyRunLoopKey = ~0;

// See function below.
bool gMARRY_MESSAGE_QUEUE = FALSE;
//
// vod __CFSetNonMachRunLoopMarryMsgQueueMode(bool bEnabled)
//
//  This function will exist for a while until code is written
//  by the WebKit folks to handle the new RunLoop message-queue
//  model.
CF_EXPORT void __CFSetNonMachRunLoopMarryMsgQueueMode(bool bEnabled) {
    gMARRY_MESSAGE_QUEUE = bEnabled;
}
bool __CFIsNonMachRunLoopMarryMsgQueueEnabled(void) {
    return gMARRY_MESSAGE_QUEUE;
}

#else

static pthread_t kNilPthreadT = (pthread_t)0;
#define pthreadPointer(a) a
#define lockCount(a) a
#endif

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
#include <sys/types.h>
#include <sys/event.h>

typedef struct {
    CFIndex	version;
    void *	info;
    const void *(*retain)(const void *info);
    void	(*release)(const void *info);
    CFStringRef	(*copyDescription)(const void *info);
    Boolean	(*equal)(const void *info1, const void *info2);
    CFHashCode	(*hash)(const void *info);
    void	(*perform)(const struct kevent *kev, void *info);
    struct kevent event;
} CFRunLoopSourceContext2;

// The bits in the flags field in the kevent structure are cleared except for EV_ONESHOT and EV_CLEAR.
// Do not use the udata field of the kevent structure -- that field is smashed by CFRunLoop.
// There is no way to EV_ENABLE or EV_DISABLE a kevent.
// The "autoinvalidation" of EV_ONESHOT is not handled properly by CFRunLoop yet.
// The "autoinvalidation" of EV_DELETE on the last close of a file descriptor is not handled properly by CFRunLoop yet.
// There is no way to reset the state in a kevent (such as clearing the EV_EOF state for fifos).
#endif

CF_EXPORT bool CFDictionaryGetKeyIfPresent(CFDictionaryRef dict, const void *key, const void **actualkey);

// In order to reuse most of the code across Mach and Windows v1 RunLoopSources, we define a
// simple abstraction layer spanning Mach ports and Windows HANDLES
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED

typedef mach_port_t __CFPort;
#define CFPORT_NULL MACH_PORT_NULL
typedef mach_port_t __CFPortSet;

static void __THE_SYSTEM_HAS_NO_PORTS_AVAILABLE__(kern_return_t ret) __attribute__((noinline));
static void __THE_SYSTEM_HAS_NO_PORTS_AVAILABLE__(kern_return_t ret) { HALT; };

static __CFPort __CFPortAllocate(void) {
    __CFPort result = CFPORT_NULL;
    kern_return_t ret;
    ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &result);
    if (KERN_SUCCESS == ret) {
        ret = mach_port_insert_right(mach_task_self(), result, result, MACH_MSG_TYPE_MAKE_SEND);
    } else {
        __THE_SYSTEM_HAS_NO_PORTS_AVAILABLE__(ret);
    }
    if (KERN_SUCCESS == ret) {
        mach_port_limits_t limits;
        limits.mpl_qlimit = 1;
        ret = mach_port_set_attributes(mach_task_self(), result, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT);
    }
    if (KERN_SUCCESS != ret) mach_port_destroy(mach_task_self(), result);
    return (KERN_SUCCESS == ret) ? result : CFPORT_NULL;
}

CF_INLINE void __CFPortFree(__CFPort port) {
    mach_port_destroy(mach_task_self(), port);
}

static void __THE_SYSTEM_HAS_NO_PORT_SETS_AVAILABLE__(kern_return_t ret) __attribute__((noinline));
static void __THE_SYSTEM_HAS_NO_PORT_SETS_AVAILABLE__(kern_return_t ret) { HALT; };

CF_INLINE __CFPortSet __CFPortSetAllocate(void) {
    __CFPortSet result;
    kern_return_t ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &result);
    if (KERN_SUCCESS != ret) { __THE_SYSTEM_HAS_NO_PORT_SETS_AVAILABLE__(ret); }
    return (KERN_SUCCESS == ret) ? result : CFPORT_NULL;
}

CF_INLINE Boolean __CFPortSetInsert(__CFPort port, __CFPortSet portSet) {
    kern_return_t ret = mach_port_insert_member(mach_task_self(), port, portSet);
    return (KERN_SUCCESS == ret);
}

CF_INLINE Boolean __CFPortSetRemove(__CFPort port, __CFPortSet portSet) {
    kern_return_t ret = mach_port_extract_member(mach_task_self(), port, portSet);
    return (KERN_SUCCESS == ret);
}

CF_INLINE void __CFPortSetFree(__CFPortSet portSet) {
    kern_return_t ret;
    mach_port_name_array_t array;
    mach_msg_type_number_t idx, number;

    ret = mach_port_get_set_status(mach_task_self(), portSet, &array, &number);
    if (KERN_SUCCESS == ret) {
        for (idx = 0; idx < number; idx++) {
            mach_port_extract_member(mach_task_self(), array[idx], portSet);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)array, number * sizeof(mach_port_name_t));
    }
    mach_port_destroy(mach_task_self(), portSet);
}

#elif DEPLOYMENT_TARGET_WINDOWS

typedef HANDLE __CFPort;
#define CFPORT_NULL NULL

// A simple dynamic array of HANDLEs, which grows to a high-water mark
typedef struct ___CFPortSet {
    uint16_t	used;
    uint16_t	size;
    HANDLE	*handles;
    CFSpinLock_t lock;		// insert and remove must be thread safe, like the Mach calls
} *__CFPortSet;

CF_INLINE __CFPort __CFPortAllocate(void) {
    return CreateEventA(NULL, true, false, NULL);
}

CF_INLINE void __CFPortFree(__CFPort port) {
    CloseHandle(port);
}

static __CFPortSet __CFPortSetAllocate(void) {
    __CFPortSet result = (__CFPortSet)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(struct ___CFPortSet), 0);
    result->used = 0;
    result->size = 4;
    result->handles = (HANDLE *)CFAllocatorAllocate(kCFAllocatorSystemDefault, result->size * sizeof(HANDLE), 0);
    CF_SPINLOCK_INIT_FOR_STRUCTS(result->lock);
    return result;
}

static void __CFPortSetFree(__CFPortSet portSet) {
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, portSet->handles);
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, portSet);
}

// Returns portBuf if ports fit in that space, else returns another ptr that must be freed
static __CFPort *__CFPortSetGetPorts(__CFPortSet portSet, __CFPort *portBuf, uint32_t bufSize, uint32_t *portsUsed) {
    __CFSpinLock(&(portSet->lock));
    __CFPort *result = portBuf;
    if (bufSize < portSet->used)
	result = (__CFPort *)CFAllocatorAllocate(kCFAllocatorSystemDefault, portSet->used * sizeof(HANDLE), 0);
    if (portSet->used > 1) {
	// rotate the ports to vaguely simulate round-robin behaviour
	uint16_t lastPort = portSet->used - 1;
	HANDLE swapHandle = portSet->handles[0];
	memmove(portSet->handles, &portSet->handles[1], lastPort * sizeof(HANDLE));
	portSet->handles[lastPort] = swapHandle;
    }
    memmove(result, portSet->handles, portSet->used * sizeof(HANDLE));
    *portsUsed = portSet->used;
    __CFSpinUnlock(&(portSet->lock));
    return result;
}

static Boolean __CFPortSetInsert(__CFPort port, __CFPortSet portSet) {
    __CFSpinLock(&(portSet->lock));
    if (portSet->used >= portSet->size) {
        portSet->size += 4;
        portSet->handles = (HANDLE *)CFAllocatorReallocate(kCFAllocatorSystemDefault, portSet->handles, portSet->size * sizeof(HANDLE), 0);
    }
    if (portSet->used >= MAXIMUM_WAIT_OBJECTS)
        CFLog(kCFLogLevelWarning, CFSTR("*** More than MAXIMUM_WAIT_OBJECTS (%d) ports add to a port set.  The last ones will be ignored."), MAXIMUM_WAIT_OBJECTS);
    portSet->handles[portSet->used++] = port;
    __CFSpinUnlock(&(portSet->lock));
    return true;
}

static Boolean __CFPortSetRemove(__CFPort port, __CFPortSet portSet) {
    int i, j;
    __CFSpinLock(&(portSet->lock));
    for (i = 0; i < portSet->used; i++) {
        if (portSet->handles[i] == port) {
            for (j = i+1; j < portSet->used; j++) {
                portSet->handles[j-1] = portSet->handles[j];
            }
            portSet->used--;
            __CFSpinUnlock(&(portSet->lock));
            return true;
        }
    }
    __CFSpinUnlock(&(portSet->lock));
    return false;
}

#endif

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
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

static uint32_t __CFSendTrivialMachMessage(mach_port_t port, uint32_t msg_id, CFOptionFlags options, uint32_t timeout) {
    kern_return_t result;
    mach_msg_header_t header;
    header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    header.msgh_size = sizeof(mach_msg_header_t);
    header.msgh_remote_port = port;
    header.msgh_local_port = MACH_PORT_NULL;
    header.msgh_id = msg_id;
    result = mach_msg(&header, MACH_SEND_MSG|options, header.msgh_size, 0, MACH_PORT_NULL, timeout, MACH_PORT_NULL);
    if (result == MACH_SEND_TIMED_OUT) mach_msg_destroy(&header);
    return result;
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

#if DEPLOYMENT_TARGET_WINDOWS
// The MSG identifier for the  Windows message injected in the message queue to wake the run loop
static unsigned int __kCFRunLoopWakeUpMessage = 0;
static unsigned int __kCFRunLoopV1SourceReadyMessage = 0;
#endif

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
    CFMutableDictionaryRef _portToV1SourceMap;
    CFMutableArrayRef _submodes; // names of the submodes
    __CFPortSet _portSet;
    CFIndex _observerMask;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    int _kq;
#endif
#if DEPLOYMENT_TARGET_WINDOWS
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
#if DEPLOYMENT_TARGET_WINDOWS
    CFStringAppendFormat(result, NULL, CFSTR("<CFRunLoopMode %p [%p]>{name = %@, locked = %s, "), rlm, CFGetAllocator(rlm), rlm->_name, "unknown");
#else
    CFStringAppendFormat(result, NULL, CFSTR("<CFRunLoopMode %p [%p]>{name = %@, locked = %s, "), rlm, CFGetAllocator(rlm), rlm->_name, lockCount(rlm->_lock) ? "true" : "false");
#endif
    CFStringAppendFormat(result, NULL, CFSTR("port set = %p,"), rlm->_portSet);
#if DEPLOYMENT_TARGET_WINDOWS
    CFStringAppendFormat(result, NULL, CFSTR("MSGQ mask = %p,"), rlm->_msgQMask);
#endif
    CFStringAppendFormat(result, NULL, CFSTR("\n\tsources = %@,\n\tobservers = %@,\n\ttimers = %@\n},\n"), rlm->_sources, rlm->_observers, rlm->_timers);
    return result;
}

static void __CFRunLoopModeDeallocate(CFTypeRef cf) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)cf;
    if (NULL != rlm->_sources) CFRelease(rlm->_sources);
    if (NULL != rlm->_observers) CFRelease(rlm->_observers);
    if (NULL != rlm->_timers) CFRelease(rlm->_timers);
    if (NULL != rlm->_submodes) CFRelease(rlm->_submodes);
    if (NULL != rlm->_portToV1SourceMap) CFRelease(rlm->_portToV1SourceMap);
    CFRelease(rlm->_name);
    __CFPortSetFree(rlm->_portSet);
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    if (-1 != rlm->_kq) close(rlm->_kq);
#endif
    memset((char *)cf + sizeof(CFRuntimeBase), 0x7C, sizeof(struct __CFRunLoopMode) - sizeof(CFRuntimeBase));
}

struct _block_item {
    struct _block_item *_next;
    CFTypeRef _mode;	// CFString or CFSet or CFRunLoopTimer
    void (^_block)(void);
};

struct __CFRunLoop {
    CFRuntimeBase _base;
    CFSpinLock_t _lock;			/* locked for accessing mode list */
    __CFPort _wakeUpPort;			// used for CFRunLoopWakeUp 
    volatile uint32_t *_stopped;
    pthread_t _pthread;
    CFMutableSetRef _commonModes;
    CFMutableSetRef _commonModeItems;
    CFRunLoopModeRef _currentMode;
    CFMutableSetRef _modes;
    struct _block_item *_blocks_head;
    struct _block_item *_blocks_tail;
#if DEPLOYMENT_TARGET_WINDOWS
    DWORD _threadID;
    __CFPort _msgUpdatePort;
#endif
    CFTypeRef _counterpart;
};

#if DEPLOYMENT_TARGET_WINDOWS
void __CFRunLoopUpdateMsgWait(CFRunLoopRef rl);
void __CFRunLoopUpdateMsgWaitMarryMsgQueue(CFRunLoopRef rl);
static Boolean __CFRunLoopDoSource1(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopSourceRef rls);
CF_EXPORT LRESULT CALLBACK pumpRunLoopFromMessageQueue(int nCode, WPARAM wParam, LPARAM lParam);
#endif

/* Bit 0 of the base reserved bits is used for stopped state */
/* Bit 1 of the base reserved bits is used for sleeping state */
/* Bit 2 of the base reserved bits is used for deallocating state */

CF_INLINE Boolean __CFRunLoopIsStopped(CFRunLoopRef rl) {
    return (rl->_stopped && rl->_stopped[2]) ? true : false;
}

CF_INLINE void __CFRunLoopSetStopped(CFRunLoopRef rl) {
    if (!rl->_stopped) {
        rl->_stopped = (uint32_t volatile *)CFAllocatorAllocate(kCFAllocatorSystemDefault, 4 * sizeof(uint32_t), 0);
        rl->_stopped[0] = 0x4346524C;
        rl->_stopped[1] = 0x4346524C; // 'CFRL'
        rl->_stopped[2] = 0x00000000; // here the value is stored
        rl->_stopped[3] = 0x4346524C;
    }
    if (rl->_stopped) rl->_stopped[2] = 0x53544F50;	// 'STOP'
}

CF_INLINE void __CFRunLoopUnsetStopped(CFRunLoopRef rl) {
    if (rl->_stopped) rl->_stopped[2] = 0x0;
}

CF_INLINE Boolean __CFRunLoopIsSleeping(CFRunLoopRef rl) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rl)->_cfinfo[CF_INFO_BITS], 1, 1);
}

CF_INLINE void __CFRunLoopSetSleeping(CFRunLoopRef rl) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rl)->_cfinfo[CF_INFO_BITS], 1, 1, 1);
}

CF_INLINE void __CFRunLoopUnsetSleeping(CFRunLoopRef rl) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rl)->_cfinfo[CF_INFO_BITS], 1, 1, 0);
}

CF_INLINE Boolean __CFRunLoopIsDeallocating(CFRunLoopRef rl) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rl)->_cfinfo[CF_INFO_BITS], 2, 2);
}

CF_INLINE void __CFRunLoopSetDeallocating(CFRunLoopRef rl) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rl)->_cfinfo[CF_INFO_BITS], 2, 2, 1);
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
#if DEPLOYMENT_TARGET_WINDOWS
    CFStringAppendFormat(result, NULL, CFSTR("<CFRunLoop %p [%p]>{locked = %s, wakeup port = 0x%x, stopped = %s,\ncurrent mode = %@,\n"), cf, CFGetAllocator(cf), "unknown", rl->_wakeUpPort, (rl->_stopped && *(rl->_stopped)) ? "true" : "false", rl->_currentMode ? rl->_currentMode->_name : CFSTR("(none)"));
#else
    CFStringAppendFormat(result, NULL, CFSTR("<CFRunLoop %p [%p]>{locked = %s, wakeup port = 0x%x, stopped = %s,\ncurrent mode = %@,\n"), cf, CFGetAllocator(cf), lockCount(rl->_lock) ? "true" : "false", rl->_wakeUpPort, (rl->_stopped && (rl->_stopped[2] == 0x53544F50)) ? "true" : "false", rl->_currentMode ? rl->_currentMode->_name : CFSTR("(none)"));
#endif
    CFStringAppendFormat(result, NULL, CFSTR("common modes = %@,\ncommon mode items = %@,\nmodes = %@}\n"), rl->_commonModes, rl->_commonModeItems, rl->_modes);
    return result;
}

__private_extern__ void __CFRunLoopDump() { // __private_extern__ to keep the compiler from discarding it
    CFShow(CFCopyDescription(CFRunLoopGetCurrent()));
}

/* call with rl locked */
static CFRunLoopModeRef __CFRunLoopFindMode(CFRunLoopRef rl, CFStringRef modeName, Boolean create) {
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    struct __CFRunLoopMode srlm;
    memset(&srlm, 0, sizeof(srlm));
    srlm._base._cfisa = __CFISAForTypeID(__kCFRunLoopModeTypeID);
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
    rlm = (CFRunLoopModeRef)_CFRuntimeCreateInstance(kCFAllocatorSystemDefault, __kCFRunLoopModeTypeID, sizeof(struct __CFRunLoopMode) - sizeof(CFRuntimeBase), NULL);
    if (NULL == rlm) {
	return NULL;
    }
    CF_SPINLOCK_INIT_FOR_STRUCTS(rlm->_lock);
    rlm->_name = CFStringCreateCopy(kCFAllocatorSystemDefault, modeName);
    rlm->_stopped = false;
    rlm->_portToV1SourceMap = NULL;
    rlm->_sources = NULL;
    rlm->_observers = NULL;
    rlm->_timers = NULL;
    rlm->_submodes = NULL;
    rlm->_observerMask = 0;
    rlm->_portSet = __CFPortSetAllocate();
    if (!__CFPortSetInsert(rl->_wakeUpPort, rlm->_portSet)) HALT;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    rlm->_kq = -1;
#endif
#if DEPLOYMENT_TARGET_WINDOWS
    rlm->_msgQMask = 0;
#endif
    CFSetAddValue(rl->_modes, rlm);
    CFRelease(rlm);
    __CFRunLoopModeLock(rlm);	/* return mode locked */
    return rlm;
}


// expects rl and rlm locked
static Boolean __CFRunLoopModeIsEmpty(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopModeRef previousMode) {
    CHECK_FOR_FORK();
    if (NULL == rlm) return true;
#if DEPLOYMENT_TARGET_WINDOWS
    if (0 != rlm->_msgQMask) return false;
#endif
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    Boolean libdispatchQSafe = pthread_main_np() && ((HANDLE_DISPATCH_ON_BASE_INVOCATION_ONLY && NULL == previousMode) || (!HANDLE_DISPATCH_ON_BASE_INVOCATION_ONLY && 0 == pthread_getspecific(__CFTSDKeyIsInGCDMainQ)));
    if (libdispatchQSafe && (CFRunLoopGetMain() == rl) && CFSetContainsValue(rl->_commonModes, rlm->_name)) return false; // represents the libdispatch main queue
#endif
    if (NULL != rlm->_sources && 0 < CFSetGetCount(rlm->_sources)) return false;
    if (NULL != rlm->_timers && 0 < CFSetGetCount(rlm->_timers)) return false;
#if 1
    struct _block_item *item = rl->_blocks_head;
    while (item) {
        struct _block_item *curr = item;
        item = item->_next;
        Boolean doit = false;
        if (CFStringGetTypeID() == CFGetTypeID(curr->_mode)) {
            doit = CFEqual(curr->_mode, rlm->_name) || (CFEqual(curr->_mode, kCFRunLoopCommonModes) && CFSetContainsValue(rl->_commonModes, rlm->_name));
        } else {
            doit = CFSetContainsValue((CFSetRef)curr->_mode, rlm->_name) || (CFSetContainsValue((CFSetRef)curr->_mode, kCFRunLoopCommonModes) && CFSetContainsValue(rl->_commonModes, rlm->_name));
        }
        if (doit) return false;
    }
#endif
    if (NULL != rlm->_submodes) {
	CFIndex idx, cnt;
	for (idx = 0, cnt = CFArrayGetCount(rlm->_submodes); idx < cnt; idx++) {
	    CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
	    CFRunLoopModeRef subrlm;
	    Boolean subIsEmpty;
	    subrlm = __CFRunLoopFindMode(rl, modeName, false);
	    subIsEmpty = (NULL != subrlm) ? __CFRunLoopModeIsEmpty(rl, subrlm, previousMode) : true;
	    if (NULL != subrlm) __CFRunLoopModeUnlock(subrlm);
	    if (!subIsEmpty) return false;
	}
    }
    return true;
}

#if DEPLOYMENT_TARGET_WINDOWS
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
#endif

/* Bit 3 in the base reserved bits is used for invalid state in run loop objects */

CF_INLINE Boolean __CFIsValid(const void *cf) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)cf)->_cfinfo[CF_INFO_BITS], 3, 3);
}

CF_INLINE void __CFSetValid(void *cf) {
    __CFBitfieldSetValue(((CFRuntimeBase *)cf)->_cfinfo[CF_INFO_BITS], 3, 3, 1);
}

CF_INLINE void __CFUnsetValid(void *cf) {
    __CFBitfieldSetValue(((CFRuntimeBase *)cf)->_cfinfo[CF_INFO_BITS], 3, 3, 0);
}

struct __CFRunLoopSource {
    CFRuntimeBase _base;
    uint32_t _bits;
    CFSpinLock_t _lock;
    CFIndex _order;			/* immutable */
    CFMutableBagRef _runLoops;
    union {
	CFRunLoopSourceContext version0;	/* immutable, except invalidation */
        CFRunLoopSourceContext1 version1;	/* immutable, except invalidation */
    } _context;
};

/* Bit 1 of the base reserved bits is used for signalled state */

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
//printf("Scheduling source %p with mode %s in loop %p\n", rls, CFStringGetCStringPtr(rlm->_name, 0), rl);
    __CFRunLoopSourceLock(rls);
    if (NULL == rls->_runLoops) {
	rls->_runLoops = CFBagCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeBagCallBacks); // sources retain run loops!
    }
    CFBagAddValue(rls->_runLoops, rl);
    __CFRunLoopSourceUnlock(rls);	// have to unlock before the callout -- cannot help clients with safety
    if (0 == rls->_context.version0.version) {
	if (NULL != rls->_context.version0.schedule) {
	    rls->_context.version0.schedule(rls->_context.version0.info, rl, rlm->_name);
	}
    } else if (1 == rls->_context.version0.version) {
        __CFPort port = rls->_context.version1.getPort(rls->_context.version1.info);	/* CALLOUT */
	if (CFPORT_NULL != port) {
            __CFPortSetInsert(port, rlm->_portSet);
	}
#if DEPLOYMENT_TARGET_WINDOWS
        __CFRunLoopLock(rl);
        //#warning Bug here - if rl->_currentMode is NULL and rlm == the default mode, we should also update
        if (rl->_currentMode == rlm) {
            if (__CFIsNonMachRunLoopMarryMsgQueueEnabled())
                __CFRunLoopUpdateMsgWaitMarryMsgQueue(rl);
            else
                __CFRunLoopUpdateMsgWait(rl);
        }
        __CFRunLoopUnlock(rl);
#endif
    }
}

/* rlm is not locked */
static void __CFRunLoopSourceCancel(CFRunLoopSourceRef rls, CFRunLoopRef rl, CFRunLoopModeRef rlm) {	/* DOES CALLOUT */
//printf("Cancelling source %p from mode %s in loop %p\n", rls, CFStringGetCStringPtr(rlm->_name, 0), rl);
    __CFRunLoopSourceLock(rls);
    if (NULL != rls->_runLoops) {
        CFBagRemoveValue(rls->_runLoops, rl);
    }
    __CFRunLoopSourceUnlock(rls);
    if (0 == rls->_context.version0.version) {
	if (NULL != rls->_context.version0.cancel) {
	    rls->_context.version0.cancel(rls->_context.version0.info, rl, rlm->_name);	/* CALLOUT */
	}
    } else if (1 == rls->_context.version0.version) {
        __CFPort port = rls->_context.version1.getPort(rls->_context.version1.info);	/* CALLOUT */
        if (CFPORT_NULL != port) {
            __CFPortSetRemove(port, rlm->_portSet);
	}
#if DEPLOYMENT_TARGET_WINDOWS
        __CFRunLoopLock(rl);
        //#warning Bug here - must also update if rl->_currentMode == NULL and rlm == default mode
        if (rl->_currentMode == rlm && rl->_msgUpdatePort != CFPORT_NULL) {
            if (__CFIsNonMachRunLoopMarryMsgQueueEnabled())
                __CFRunLoopUpdateMsgWaitMarryMsgQueue(rl);
            else
                __CFRunLoopUpdateMsgWait(rl);
        }
        __CFRunLoopUnlock(rl);
#endif
    }
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
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rlo)->_cfinfo[CF_INFO_BITS], 0, 0);
}

CF_INLINE void __CFRunLoopObserverSetFiring(CFRunLoopObserverRef rlo) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlo)->_cfinfo[CF_INFO_BITS], 0, 0, 1);
}

CF_INLINE void __CFRunLoopObserverUnsetFiring(CFRunLoopObserverRef rlo) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlo)->_cfinfo[CF_INFO_BITS], 0, 0, 0);
}

CF_INLINE Boolean __CFRunLoopObserverRepeats(CFRunLoopObserverRef rlo) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rlo)->_cfinfo[CF_INFO_BITS], 1, 1);
}

CF_INLINE void __CFRunLoopObserverSetRepeats(CFRunLoopObserverRef rlo) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlo)->_cfinfo[CF_INFO_BITS], 1, 1, 1);
}

CF_INLINE void __CFRunLoopObserverUnsetRepeats(CFRunLoopObserverRef rlo) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlo)->_cfinfo[CF_INFO_BITS], 1, 1, 0);
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
    CFMutableSetRef _rlModes;
    CFAbsoluteTime _nextFireDate;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    mach_port_name_t _port;
    int64_t _fireTSR;			/* TSR units */
    int64_t _intervalTSR;		/* immutable; 0 means non-repeating; TSR units */
#elif DEPLOYMENT_TARGET_WINDOWS
    CFIndex _rlCount;
    int64_t _fireTSR;			/* TSR units */
    int64_t _intervalTSR;		/* immutable; 0 means non-repeating; TSR units */
#endif
    CFTimeInterval _interval;		/* immutable */
    CFIndex _order;			/* immutable */
    CFRunLoopTimerCallBack _callout;	/* immutable */
    CFRunLoopTimerContext _context;	/* immutable, except invalidation */
};

/* Bit 0 of the base reserved bits is used for firing state */
/* Bit 1 of the base reserved bits is used for fired-during-callout state */

CF_INLINE Boolean __CFRunLoopTimerIsFiring(CFRunLoopTimerRef rlt) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rlt)->_cfinfo[CF_INFO_BITS], 0, 0);
}

CF_INLINE void __CFRunLoopTimerSetFiring(CFRunLoopTimerRef rlt) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlt)->_cfinfo[CF_INFO_BITS], 0, 0, 1);
}

CF_INLINE void __CFRunLoopTimerUnsetFiring(CFRunLoopTimerRef rlt) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlt)->_cfinfo[CF_INFO_BITS], 0, 0, 0);
}

CF_INLINE Boolean __CFRunLoopTimerDidFire(CFRunLoopTimerRef rlt) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rlt)->_cfinfo[CF_INFO_BITS], 1, 1);
}

CF_INLINE void __CFRunLoopTimerSetDidFire(CFRunLoopTimerRef rlt) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlt)->_cfinfo[CF_INFO_BITS], 1, 1, 1);
}

CF_INLINE void __CFRunLoopTimerUnsetDidFire(CFRunLoopTimerRef rlt) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlt)->_cfinfo[CF_INFO_BITS], 1, 1, 0);
}

CF_INLINE Boolean __CFRunLoopTimerIsDeallocating(CFRunLoopTimerRef rlt) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rlt)->_cfinfo[CF_INFO_BITS], 2, 2);
}

CF_INLINE void __CFRunLoopTimerSetDeallocating(CFRunLoopTimerRef rlt) {
    __CFBitfieldSetValue(((CFRuntimeBase *)rlt)->_cfinfo[CF_INFO_BITS], 2, 2, 1);
}

CF_INLINE void __CFRunLoopTimerLock(CFRunLoopTimerRef rlt) {
    __CFSpinLock(&(rlt->_lock));
}

CF_INLINE void __CFRunLoopTimerUnlock(CFRunLoopTimerRef rlt) {
    __CFSpinUnlock(&(rlt->_lock));
}

static CFSpinLock_t __CFRLTFireTSRLock = CFSpinLockInit;

CF_INLINE void __CFRunLoopTimerFireTSRLock(void) {
    __CFSpinLock(&__CFRLTFireTSRLock);
}

CF_INLINE void __CFRunLoopTimerFireTSRUnlock(void) {
    __CFSpinUnlock(&__CFRLTFireTSRLock);
}

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
static CFMutableDictionaryRef __CFRLTPortMap = NULL;
static CFSpinLock_t __CFRLTPortMapLock = CFSpinLockInit;

CF_INLINE void __CFRunLoopTimerPortMapLock(void) {
    __CFSpinLock(&__CFRLTPortMapLock);
}

CF_INLINE void __CFRunLoopTimerPortMapUnlock(void) {
    __CFSpinUnlock(&__CFRLTPortMapLock);
}
#endif

static Boolean __CFRunLoopDoTimer(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopTimerRef rlt);

// called with timer locked in libdispatch mode
static void __CFRunLoopTimerRescheduleWithAllModes(CFRunLoopTimerRef rlt) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    mk_timer_arm(rlt->_port, __CFUInt64ToAbsoluteTime(rlt->_fireTSR));
#endif
}

static void __CFRunLoopTimerSchedule(CFRunLoopTimerRef rlt, CFRunLoopRef rl, CFRunLoopModeRef rlm) {
//CFLog(4, CFSTR("__CFRunLoopTimerSchedule(%p, loop %p, mode %@)  [%p, %p]"), rlt, rl, rlm->_name, rlt->_runLoop, rlt->_port);
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    __CFRunLoopTimerLock(rlt);
    if (NULL == rlt->_runLoop) {
	rlt->_runLoop = rl;
	if (MACH_PORT_NULL == rlt->_port) {
	    rlt->_port = mk_timer_create();
	}
	__CFRunLoopTimerPortMapLock();
	if (NULL == __CFRLTPortMap) {
	    __CFRLTPortMap = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, NULL);
	}
	CFDictionarySetValue(__CFRLTPortMap, (void *)(uintptr_t)rlt->_port, rlt);
	__CFRunLoopTimerPortMapUnlock();
	__CFRunLoopTimerRescheduleWithAllModes(rlt);
    }
    mach_port_insert_member(mach_task_self(), rlt->_port, rlm->_portSet);
    CFSetAddValue(rlt->_rlModes, rlm->_name);
    __CFRunLoopTimerUnlock(rlt);
#elif DEPLOYMENT_TARGET_WINDOWS
    if (0 == rlt->_rlCount) {
        rlt->_runLoop = rl;        
    }
    rlt->_rlCount++;
#endif
}

static void __CFRunLoopTimerCancel(CFRunLoopTimerRef rlt, CFRunLoopRef rl, CFRunLoopModeRef rlm) {
//CFLog(6, CFSTR("__CFRunLoopTimerCancel(rlt %p, rl %p, rlm %@)"), rlt, rl, rlm->_name);
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    __CFRunLoopTimerLock(rlt);
    CFSetRemoveValue(rlt->_rlModes, rlm->_name);
    __CFPortSetRemove(rlt->_port, rlm->_portSet);
    if (0 == CFSetGetCount(rlt->_rlModes)) {
	__CFRunLoopTimerPortMapLock();
	if (NULL != __CFRLTPortMap) {
	    CFDictionaryRemoveValue(__CFRLTPortMap, (void *)(uintptr_t)rlt->_port);
	}
	__CFRunLoopTimerPortMapUnlock();
	mk_timer_cancel(rlt->_port, NULL);
	rlt->_runLoop = NULL;
    }
    __CFRunLoopTimerUnlock(rlt);
#elif DEPLOYMENT_TARGET_WINDOWS
    CFRunLoopWakeUp(rl);
#endif
}

#if DEPLOYMENT_TARGET_WINDOWS

struct _collectTimersContext {
    CFMutableArrayRef results;
    int64_t cutoffTSR;
};

static void __CFRunLoopCollectTimers(const void *value, void *ctx) {
    CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)value;
    struct _collectTimersContext *context = (struct _collectTimersContext *)ctx;
    if (rlt->_fireTSR <= context->cutoffTSR) {
        if (NULL == context->results)
            context->results = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
        CFArrayAppendValue(context->results, rlt);
    }
}

// RunLoop and RunLoopMode must be locked
static void __CFRunLoopTimersToFireRecursive(CFRunLoopRef rl, CFRunLoopModeRef rlm, struct _collectTimersContext *ctxt) {
    if (NULL != rlm->_timers && 0 < CFSetGetCount(rlm->_timers)) {
        __CFRunLoopTimerFireTSRLock();
        CFSetApplyFunction(rlm->_timers, __CFRunLoopCollectTimers, ctxt);
        __CFRunLoopTimerFireTSRUnlock();
    }
    if (NULL != rlm->_submodes) {
        CFIndex idx, cnt;
        for (idx = 0, cnt = CFArrayGetCount(rlm->_submodes); idx < cnt; idx++) {
            CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
            CFRunLoopModeRef subrlm;
            subrlm = __CFRunLoopFindMode(rl, modeName, false);
            if (NULL != subrlm) {
                __CFRunLoopTimersToFireRecursive(rl, subrlm, ctxt);
                __CFRunLoopModeUnlock(subrlm);
            }
        }
    }
}

// RunLoop and RunLoopMode must be locked
static CFArrayRef __CFRunLoopTimersToFire(CFRunLoopRef rl, CFRunLoopModeRef rlm) {
    struct _collectTimersContext ctxt = {NULL, __CFReadTSR()};
    __CFRunLoopTimersToFireRecursive(rl, rlm, &ctxt);
    return ctxt.results;
}
#endif

/* CFRunLoop */

CONST_STRING_DECL(kCFRunLoopDefaultMode, "kCFRunLoopDefaultMode")
CONST_STRING_DECL(kCFRunLoopCommonModes, "kCFRunLoopCommonModes")

// call with rl and rlm locked
static CFRunLoopSourceRef __CFRunLoopModeFindSourceForMachPort(CFRunLoopRef rl, CFRunLoopModeRef rlm, __CFPort port) {	/* DOES CALLOUT */
    CHECK_FOR_FORK();
    CFRunLoopSourceRef found = rlm->_portToV1SourceMap ? (CFRunLoopSourceRef)CFDictionaryGetValue(rlm->_portToV1SourceMap, (const void *)(uintptr_t)port) : NULL;
    if (NULL == found && NULL != rlm->_submodes) {
	CFIndex idx, cnt;
	for (idx = 0, cnt = CFArrayGetCount(rlm->_submodes); idx < cnt; idx++) {
	    CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
	    CFRunLoopModeRef subrlm;
            subrlm = __CFRunLoopFindMode(rl, modeName, false);
	    if (NULL != subrlm) {
		found = __CFRunLoopModeFindSourceForMachPort(rl, subrlm, port);
		__CFRunLoopModeUnlock(subrlm);
	    }
	    if (NULL != found) {
		break;
	    }
	}
    }
    return found;
}

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
// call with rl and rlm locked
static CFRunLoopTimerRef __CFRunLoopModeFindTimerForMachPort(CFRunLoopModeRef rlm, __CFPort port) {
    CHECK_FOR_FORK();
    CFRunLoopTimerRef result = NULL;
    __CFRunLoopTimerPortMapLock();
    if (NULL != __CFRLTPortMap) {
	result = (CFRunLoopTimerRef)CFDictionaryGetValue(__CFRLTPortMap, (void *)(uintptr_t)port);
    }
    __CFRunLoopTimerPortMapUnlock();
    return result;
}
#endif

// Remove backreferences the mode's sources have to the rl (context);
// the primary purpose of rls->_runLoops is so that Invalidation can remove
// the source from the run loops it is in, but during deallocation of a
// run loop, we already know that the sources are going to be punted
// from it, so invalidation of sources does not need to remove from a
// deallocating run loop.
static void __CFRunLoopCleanseSources(const void *value, void *context) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)context;
    CFIndex idx, cnt;
    const void **list, *buffer[256];
    if (NULL == rlm->_sources) return;
    cnt = CFSetGetCount(rlm->_sources);
    list = (const void **)((cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0));
    CFSetGetValues(rlm->_sources, list);
    for (idx = 0; idx < cnt; idx++) {
	CFRunLoopSourceRef rls = (CFRunLoopSourceRef)list[idx];
	__CFRunLoopSourceLock(rls);
	if (NULL != rls->_runLoops) {
	    CFBagRemoveValue(rls->_runLoops, rl);
	}
	__CFRunLoopSourceUnlock(rls);
    }
    if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
}

static void __CFRunLoopDeallocateSources(const void *value, void *context) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)context;
    CFIndex idx, cnt;
    const void **list, *buffer[256];
    if (NULL == rlm->_sources) return;
    cnt = CFSetGetCount(rlm->_sources);
    list = (const void **)((cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0));
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
    list = (const void **)((cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0));
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
    list = (const void **)((cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0));
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

CF_EXPORT pthread_t _CFMainPThread;
CF_EXPORT CFRunLoopRef _CFRunLoopGet0b(pthread_t t);

static void __CFRunLoopDeallocate(CFTypeRef cf) {
    CFRunLoopRef rl = (CFRunLoopRef)cf;

    if (_CFRunLoopGet0b(_CFMainPThread) == cf) HALT;

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
	CFSetApplyFunction(rl->_modes, (__CFRunLoopCleanseSources), rl); // remove references to rl
	CFSetApplyFunction(rl->_modes, (__CFRunLoopDeallocateSources), rl);
	CFSetApplyFunction(rl->_modes, (__CFRunLoopDeallocateObservers), rl);
	CFSetApplyFunction(rl->_modes, (__CFRunLoopDeallocateTimers), rl);
    }
    __CFRunLoopLock(rl);
    struct _block_item *item = rl->_blocks_head;
    while (item) {
	struct _block_item *curr = item;
	item = item->_next;
	CFRelease(curr->_mode);
	Block_release(curr->_block);
	free(curr);
    }
    if (NULL != rl->_commonModeItems) {
	CFRelease(rl->_commonModeItems);
    }
    if (NULL != rl->_commonModes) {
	CFRelease(rl->_commonModes);
    }
    if (NULL != rl->_modes) {
	CFRelease(rl->_modes);
    }
    __CFPortFree(rl->_wakeUpPort);
    rl->_wakeUpPort = CFPORT_NULL;
#if DEPLOYMENT_TARGET_WINDOWS
    __CFPortFree(rl->_msgUpdatePort);
    rl->_msgUpdatePort = CFPORT_NULL;
#endif
    __CFRunLoopUnlock(rl);
    memset((char *)cf + sizeof(CFRuntimeBase), 0x8C, sizeof(struct __CFRunLoop) - sizeof(CFRuntimeBase));
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

static void __CFFinalizeRunLoop(uintptr_t data);

__private_extern__ void __CFRunLoopInitialize(void) {
    __kCFRunLoopTypeID = _CFRuntimeRegisterClass(&__CFRunLoopClass);
    __kCFRunLoopModeTypeID = _CFRuntimeRegisterClass(&__CFRunLoopModeClass);
#if DEPLOYMENT_TARGET_WINDOWS
    __kCFRunLoopWakeUpMessage = RegisterWindowMessageW((LPCWSTR)L"CFRunLoopWakeup");
    __kCFRunLoopV1SourceReadyMessage = RegisterWindowMessageW((LPCWSTR)L"CFRunLoopV1Ready");
#endif
#if DEPLOYMENT_TARGET_WINDOWS
    if (~0 == __CFTSDKeyRunLoopKey) __CFTSDKeyRunLoopKey = TlsAlloc();
#else
    pthread_key_init_np(__CFTSDKeyRunLoop, NULL);
    pthread_key_init_np(__CFTSDKeyRunLoopCntr, (void *)__CFFinalizeRunLoop);
#endif
}
 
CFTypeID CFRunLoopGetTypeID(void) {
    return __kCFRunLoopTypeID;
}

static CFRunLoopRef __CFRunLoopCreate(pthread_t t) {
    CFRunLoopRef loop = NULL;
    CFRunLoopModeRef rlm;
    uint32_t size = sizeof(struct __CFRunLoop) - sizeof(CFRuntimeBase);
    loop = (CFRunLoopRef)_CFRuntimeCreateInstance(kCFAllocatorSystemDefault, __kCFRunLoopTypeID, size, NULL);
    if (NULL == loop) {
	return NULL;
    }
    loop->_stopped = NULL;
    CF_SPINLOCK_INIT_FOR_STRUCTS(loop->_lock);
    loop->_wakeUpPort = __CFPortAllocate();
    if (CFPORT_NULL == loop->_wakeUpPort) HALT;
#if DEPLOYMENT_TARGET_WINDOWS
    loop->_msgUpdatePort = CFPORT_NULL;
#endif
    loop->_commonModes = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
    CFSetAddValue(loop->_commonModes, kCFRunLoopDefaultMode);
    loop->_commonModeItems = NULL;
    loop->_currentMode = NULL;
    loop->_modes = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
    _CFSetSetCapacity(loop->_modes, 10);
    loop->_blocks_head = NULL;
    loop->_blocks_tail = NULL;
    loop->_counterpart = NULL;
    loop->_pthread = t;
    rlm = __CFRunLoopFindMode(loop, kCFRunLoopDefaultMode, true);
    if (NULL != rlm) __CFRunLoopModeUnlock(rlm);
    return loop;
}

static CFMutableDictionaryRef __CFRunLoops = NULL;
static CFSpinLock_t loopsLock = CFSpinLockInit;

// should only be called by Foundation
// t==0 is a synonym for "main thread" that always works
CF_EXPORT CFRunLoopRef _CFRunLoopGet0(pthread_t t) {
    if (pthread_equal(t, kNilPthreadT)) {
	t = _CFMainPThread;
    }
    __CFSpinLock(&loopsLock);
    if (!__CFRunLoops) {
        __CFSpinUnlock(&loopsLock);
	CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, &kCFTypeDictionaryValueCallBacks);
	CFRunLoopRef mainLoop = __CFRunLoopCreate(_CFMainPThread);
	CFDictionarySetValue(dict, pthreadPointer(_CFMainPThread), mainLoop);
	if (!OSAtomicCompareAndSwapPtrBarrier(NULL, dict, (void * volatile *)&__CFRunLoops)) {
	    CFRelease(dict);
	}
	CFRelease(mainLoop);
        __CFSpinLock(&loopsLock);
    }
    CFRunLoopRef loop = (CFRunLoopRef)CFDictionaryGetValue(__CFRunLoops, pthreadPointer(t));
    if (!loop) {
        __CFSpinUnlock(&loopsLock);
	CFRunLoopRef newLoop = __CFRunLoopCreate(t);
        __CFSpinLock(&loopsLock);
	loop = (CFRunLoopRef)CFDictionaryGetValue(__CFRunLoops, pthreadPointer(t));
	if (!loop) {
	    CFDictionarySetValue(__CFRunLoops, pthreadPointer(t), newLoop);
	    loop = newLoop;
	}
	CFRelease(newLoop);
    }
    if (pthread_equal(t, pthread_self())) {
#if DEPLOYMENT_TARGET_WINDOWS
        TlsSetValue(__CFTSDKeyRunLoopKey, (LPVOID)(PTHREAD_DESTRUCTOR_ITERATIONS-1));
#else
        pthread_setspecific(__CFTSDKeyRunLoop, (void *)loop);
        if (0 == pthread_getspecific(__CFTSDKeyRunLoopCntr)) {
            pthread_setspecific(__CFTSDKeyRunLoopCntr, (void *)(PTHREAD_DESTRUCTOR_ITERATIONS-1));
        }
#endif
#if DEPLOYMENT_TARGET_WINDOWS
        // Install our message hook so we can spin the runloop
        loop->_threadID = GetCurrentThreadId();
        //#if MARRY_MESSAGE_QUEUE
        if (__CFIsNonMachRunLoopMarryMsgQueueEnabled()) {
#ifdef _DEBUG
            __CFGetThreadSpecificData_inline()->_messageHook = SetWindowsHookExW(WH_GETMESSAGE, (HOOKPROC)pumpRunLoopFromMessageQueue, GetModuleHandleW((LPCWSTR)L"CoreFoundation_debug.dll"), loop->_threadID); 
#else
            __CFGetThreadSpecificData_inline()->_messageHook = SetWindowsHookExW(WH_GETMESSAGE, (HOOKPROC)pumpRunLoopFromMessageQueue, GetModuleHandleW((LPCWSTR)L"CoreFoundation.dll"), loop->_threadID); 
#endif
        }
        //#endif
#endif
    }
    __CFSpinUnlock(&loopsLock);
    return loop;
}

// should only be called by Foundation
CFRunLoopRef _CFRunLoopGet0b(pthread_t t) {
    if (pthread_equal(t, kNilPthreadT)) {
	t = _CFMainPThread;
    }
    __CFSpinLock(&loopsLock);
    CFRunLoopRef loop = NULL;
    if (__CFRunLoops) {
        loop = (CFRunLoopRef)CFDictionaryGetValue(__CFRunLoops, pthreadPointer(t));
    }
    __CFSpinUnlock(&loopsLock);
    return loop;
}

static void __CFRunLoopRemoveAllSources(CFRunLoopRef rl, CFStringRef modeName);

// Called for each thread as it exits
static void __CFFinalizeRunLoop(uintptr_t data) {
    CFRunLoopRef rl = NULL;
    if (data <= 1) {
	__CFSpinLock(&loopsLock);
	if (__CFRunLoops) {
	    rl = (CFRunLoopRef)CFDictionaryGetValue(__CFRunLoops, pthreadPointer(pthread_self()));
	    if (rl) CFRetain(rl);
	    CFDictionaryRemoveValue(__CFRunLoops, pthreadPointer(pthread_self()));
	}
	__CFSpinUnlock(&loopsLock);
    } else {
#if DEPLOYMENT_TARGET_WINDOWS
        TlsSetValue(__CFTSDKeyRunLoopKey, (LPVOID)(data - 1));
#else
        pthread_setspecific(__CFTSDKeyRunLoopCntr, (void *)(data - 1));
#endif
    }
    if (rl && CFRunLoopGetMain() != rl) { // protect against cooperative threads
        if (NULL != rl->_counterpart) {
            CFRelease(rl->_counterpart);
	    rl->_counterpart = NULL;
        }
	// purge all sources before deallocation
        CFArrayRef array = CFRunLoopCopyAllModes(rl);
        for (CFIndex idx = CFArrayGetCount(array); idx--;) {
            CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(array, idx);
            __CFRunLoopRemoveAllSources(rl, modeName);
        }
        __CFRunLoopRemoveAllSources(rl, kCFRunLoopCommonModes);
        CFRelease(array);
    }
    if (rl) CFRelease(rl);
}

pthread_t _CFRunLoopGet1(CFRunLoopRef rl) {
    return rl->_pthread;
}

// should only be called by Foundation
CF_EXPORT CFTypeRef _CFRunLoopGet2(CFRunLoopRef rl) {
    CFTypeRef ret = NULL;
    __CFSpinLock(&loopsLock);
    if (NULL == rl->_counterpart) {
        Class cls = (Class)objc_lookUpClass("NSRunLoop");
	if (cls) {
            char hasMethod;
            CF_OBJC_CALL1(char, hasMethod, cls, "respondsToSelector:", sel_registerName("_new:"));
            if (hasMethod) {
                void *ns;
	        CF_OBJC_CALL1(void *, ns, cls, "_new:", rl);
                rl->_counterpart = ns ? CFRetain(ns) : NULL;
            }
        }
    }
    ret = rl->_counterpart;
    __CFSpinUnlock(&loopsLock);
    return ret;
}

// should only be called by Foundation
CF_EXPORT CFTypeRef _CFRunLoopGet2b(CFRunLoopRef rl) {
    return rl->_counterpart;
}

#if DEPLOYMENT_TARGET_MACOSX
void _CFRunLoopSetCurrent(CFRunLoopRef rl) {
    if (pthread_main_np()) return;
    CFRunLoopRef currentLoop = CFRunLoopGetCurrent();
    if (rl != currentLoop) {
        CFRetain(currentLoop); // avoid a deallocation of the currentLoop inside the lock
        __CFSpinLock(&loopsLock);
	if (rl) {
	    CFDictionarySetValue(__CFRunLoops, pthreadPointer(pthread_self()), rl);
	} else {
	    CFDictionaryRemoveValue(__CFRunLoops, pthreadPointer(pthread_self()));
	}
        __CFSpinUnlock(&loopsLock);
	CFRelease(currentLoop);
        pthread_setspecific(__CFTSDKeyRunLoop, NULL);
        pthread_setspecific(__CFTSDKeyRunLoopCntr, 0);
    }
}
#endif

CFRunLoopRef CFRunLoopGetMain(void) {
    CHECK_FOR_FORK();
    static CFRunLoopRef __main = NULL; // no retain needed
    if (!__main) __main = _CFRunLoopGet0(_CFMainPThread); // no CAS needed
    return __main;
}

CFRunLoopRef CFRunLoopGetCurrent(void) {
    CHECK_FOR_FORK();
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    CFRunLoopRef rl = (CFRunLoopRef)pthread_getspecific(__CFTSDKeyRunLoop);
    if (rl) return rl;
#endif
    return _CFRunLoopGet0(pthread_self());
}

CFStringRef CFRunLoopCopyCurrentMode(CFRunLoopRef rl) {
    CHECK_FOR_FORK();
    CFStringRef result = NULL;
    __CFRunLoopLock(rl);
    if (NULL != rl->_currentMode) {
	result = (CFStringRef)CFRetain(rl->_currentMode->_name);
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
    CHECK_FOR_FORK();
    CFMutableArrayRef array;
    __CFRunLoopLock(rl);
    array = CFArrayCreateMutable(kCFAllocatorSystemDefault, CFSetGetCount(rl->_modes), &kCFTypeArrayCallBacks);
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

CF_EXPORT Boolean _CFRunLoop01(CFRunLoopRef rl, CFStringRef modeName) {
    __CFRunLoopLock(rl);
    Boolean present = CFSetContainsValue(rl->_commonModes, modeName);
    __CFRunLoopUnlock(rl);
    return present; 
}

void CFRunLoopAddCommonMode(CFRunLoopRef rl, CFStringRef modeName) {
    CHECK_FOR_FORK();
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

static Boolean __CFRunLoopDoBlocks(CFRunLoopRef rl, CFStringRef curMode) { // Call with rl locked
    if (!rl->_blocks_head) return false;
    if (!curMode) return false;
    Boolean did = false;
    CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 0.0009765625;
    struct _block_item *head = rl->_blocks_head;
    struct _block_item *tail = rl->_blocks_tail;
    rl->_blocks_head = NULL;
    rl->_blocks_tail = NULL;
    CFSetRef commonModes = rl->_commonModes;
    __CFRunLoopUnlock(rl);
    struct _block_item *prev = NULL;
    struct _block_item *item = head;
    while (item && CFAbsoluteTimeGetCurrent() < deadline) {
        struct _block_item *curr = item;
        item = item->_next;
	Boolean doit = false;
	if (CFStringGetTypeID() == CFGetTypeID(curr->_mode)) {
	    doit = CFEqual(curr->_mode, curMode) || (CFEqual(curr->_mode, kCFRunLoopCommonModes) && CFSetContainsValue(commonModes, curMode));
        } else {
	    doit = CFSetContainsValue((CFSetRef)curr->_mode, curMode) || (CFSetContainsValue((CFSetRef)curr->_mode, kCFRunLoopCommonModes) && CFSetContainsValue(commonModes, curMode));
	}
	if (!doit) prev = curr;
	if (doit) {
	    if (prev) prev->_next = item;
	    if (curr == head) head = item;
	    if (curr == tail) tail = prev;
	    void (^block)(void) = curr->_block;
            CFRelease(curr->_mode);
            free(curr);
	    if (doit) {
	        block();	// rl is not locked, mode is not locked
	        did = true;
	    }
            Block_release(block); // do this before relocking to prevent deadlocks where some yahoo wants to run the run loop reentrantly from their dealloc
	}
    }
    __CFRunLoopLock(rl);
    if (head) {
	tail->_next = rl->_blocks_head;
	rl->_blocks_head = head;
        if (!rl->_blocks_tail) rl->_blocks_tail = tail;
    }
    return did;
}

static CFComparisonResult __CFRunLoopObserverQSortComparator(const void *val1, const void *val2, void *context) {
    CFRunLoopObserverRef o1 = *((CFRunLoopObserverRef *)val1);
    CFRunLoopObserverRef o2 = *((CFRunLoopObserverRef *)val2);
    if (!o1) {
	return (!o2) ? kCFCompareEqualTo : kCFCompareLessThan;
    }
    if (!o2) {
	return kCFCompareGreaterThan;
    }
    if (o1->_order < o2->_order) return kCFCompareLessThan;
    if (o2->_order < o1->_order) return kCFCompareGreaterThan;
    return kCFCompareEqualTo;
}


/* rl is unlocked, rlm is locked on entrance and exit */
/* ALERT: this should collect all the candidate observers from the top level
 * and all submodes, recursively, THEN start calling them, in order to obey
 * the ordering parameter. */
static void __CFRunLoopDoObservers(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopActivity activity) {	/* DOES CALLOUT */
    CHECK_FOR_FORK();
    CFIndex idx, cnt;
    CFArrayRef submodes;

    /* Fire the observers */
    submodes = (NULL != rlm->_submodes && 0 < CFArrayGetCount(rlm->_submodes)) ? CFArrayCreateCopy(kCFAllocatorSystemDefault, rlm->_submodes) : NULL;
    if (NULL != rlm->_observers) {
	cnt = CFSetGetCount(rlm->_observers);
	if (0 < cnt) {
	    STACK_BUFFER_DECL(CFRunLoopObserverRef, buffer, (cnt <= 1024) ? cnt : 1);
	    CFRunLoopObserverRef *collectedObservers = (CFRunLoopObserverRef *)((cnt <= 1024) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(CFRunLoopObserverRef), 0));
	    CFSetGetValues(rlm->_observers, (const void **)collectedObservers);
	    for (idx = 0; idx < cnt; idx++) {
		CFRunLoopObserverRef rlo = collectedObservers[idx];
		if (0 != (rlo->_activities & activity) && __CFIsValid(rlo) && !__CFRunLoopObserverIsFiring(rlo)) {
		    CFRetain(rlo);
		} else {
		    /* We're not interested in this one - set it to NULL so we don't process it later */
		    collectedObservers[idx] = NULL;
		}
	    }
	    __CFRunLoopModeUnlock(rlm);
	    CFQSortArray(collectedObservers, cnt, sizeof(CFRunLoopObserverRef), __CFRunLoopObserverQSortComparator, NULL);
	    for (idx = 0; idx < cnt; idx++) {
		CFRunLoopObserverRef rlo = collectedObservers[idx];
		if (rlo) {
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
		    CFRelease(rlo);
 		}
	    }
	    __CFRunLoopModeLock(rlm);
	    if (collectedObservers != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, collectedObservers);
	}
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
    CHECK_FOR_FORK();
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
#if DEPLOYMENT_TARGET_WINDOWS
            if (__CFRunLoopSourceIsSignaled(rls)) {
#endif
	    __CFRunLoopSourceUnsetSignaled(rls);
	    if (__CFIsValid(rls)) {
		__CFRunLoopSourceUnlock(rls);
		if (NULL != rls->_context.version0.perform) {
		    rls->_context.version0.perform(rls->_context.version0.info); /* CALLOUT */
		    CHECK_FOR_FORK();
		}
		sourceHandled = true;
	    } else {
		__CFRunLoopSourceUnlock(rls);
	    }
#if DEPLOYMENT_TARGET_WINDOWS
            } else {
                __CFRunLoopSourceUnlock(rls);
            }
#endif
	} else {
	    cnt = CFArrayGetCount((CFArrayRef)sources);
	    CFArraySortValues((CFMutableArrayRef)sources, CFRangeMake(0, cnt), (__CFRunLoopSourceComparator), NULL);
	    for (idx = 0; idx < cnt; idx++) {
		CFRunLoopSourceRef rls = (CFRunLoopSourceRef)CFArrayGetValueAtIndex((CFArrayRef)sources, idx);
		__CFRunLoopSourceLock(rls);
#if DEPLOYMENT_TARGET_WINDOWS
                if (__CFRunLoopSourceIsSignaled(rls)) {
#endif
		__CFRunLoopSourceUnsetSignaled(rls);
		if (__CFIsValid(rls)) {
		    __CFRunLoopSourceUnlock(rls);
		    if (NULL != rls->_context.version0.perform) {
			rls->_context.version0.perform(rls->_context.version0.info); /* CALLOUT */
		        CHECK_FOR_FORK();
		    }
		    sourceHandled = true;
		} else {
		    __CFRunLoopSourceUnlock(rls);
		}
#if DEPLOYMENT_TARGET_WINDOWS
                } else {
                    __CFRunLoopSourceUnlock(rls);
                }
#endif
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

// msg, size and reply are unused on Windows
static Boolean __CFRunLoopDoSource1(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopSourceRef rls
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
                                    , mach_msg_header_t *msg, CFIndex size, mach_msg_header_t **reply
#endif
                                    ) {	/* DOES CALLOUT */
    CHECK_FOR_FORK();
    Boolean sourceHandled = false;

    /* Fire a version 1 source */
    CFRetain(rls);
    __CFRunLoopModeUnlock(rlm);
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls)) {
	__CFRunLoopSourceUnsetSignaled(rls);
	__CFRunLoopSourceUnlock(rls);
	if (NULL != rls->_context.version1.perform) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	    *reply = rls->_context.version1.perform(msg, size, kCFAllocatorSystemDefault, rls->_context.version1.info); /* CALLOUT */
	    CHECK_FOR_FORK();
#else
            if (_LogCFRunLoop) { CFLog(kCFLogLevelDebug, CFSTR("%p (%s) __CFRunLoopDoSource1 performing rls %p"), CFRunLoopGetCurrent(), *_CFGetProgname(), rls); }
            rls->_context.version1.perform(rls->_context.version1.info); /* CALLOUT */
	    CHECK_FOR_FORK();
#endif
	} else {
        if (_LogCFRunLoop) { CFLog(kCFLogLevelDebug, CFSTR("%p (%s) __CFRunLoopDoSource1 perform is NULL"), CFRunLoopGetCurrent(), *_CFGetProgname()); }
    }
	sourceHandled = true;
    } else {
        if (_LogCFRunLoop) { CFLog(kCFLogLevelDebug, CFSTR("%p (%s) __CFRunLoopDoSource1 rls %p is invalid"), CFRunLoopGetCurrent(), *_CFGetProgname(), rls); }
	__CFRunLoopSourceUnlock(rls);
    }
    CFRelease(rls);
    __CFRunLoopModeLock(rlm);
    return sourceHandled;
}

// mode is locked on entry and exit
static Boolean __CFRunLoopDoTimer(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopTimerRef rlt) {	/* DOES CALLOUT */
    Boolean timerHandled = false, timerEarly = false;
    int64_t oldFireTSR = 0;

    /* Fire a timer */
    CFRetain(rlt);
    __CFRunLoopModeUnlock(rlm);
    if (__CFIsValid(rlt) && !__CFRunLoopTimerIsFiring(rlt)) {
        void *context_info = NULL;
        void (*context_release)(const void *) = NULL;
        if (rlt->_context.retain) {
            context_info = (void *)rlt->_context.retain(rlt->_context.info);
            context_release = rlt->_context.release;
        } else {
            context_info = rlt->_context.info;
        }
	__CFRunLoopTimerUnsetDidFire(rlt);
	__CFRunLoopTimerSetFiring(rlt);
//CFLog(4, CFSTR("Firing timer %p from loop %p in mode %@"), rlt, rl, rlm->_name);
	__CFRunLoopTimerFireTSRLock();
	oldFireTSR = rlt->_fireTSR;
	__CFRunLoopTimerFireTSRUnlock();
	if (0 && CFAbsoluteTimeGetCurrent() < rlt->_nextFireDate) { // not enabled at this time -- causes trouble when clock goes backwards
	    timerEarly = true;
	} else {
	    rlt->_callout(rlt, context_info);	/* CALLOUT */
	    CHECK_FOR_FORK();
	    timerHandled = true;
        }
//CFLog(4, CFSTR("Done firing timer %p from loop %p in mode %@"), rlt, rl, rlm->_name);
	__CFRunLoopTimerUnsetFiring(rlt);
        if (context_release) {
            context_release(context_info);
        }
    } else {
	// If the timer fires while it is firing in a higher activiation,
	// it is not allowed to fire, but we have to remember that fact.
	// Later, if the timer's fire date is being handled manually, we
	// need to re-arm the kernel timer, since it has possibly already
	// fired (this firing which is being skipped, say) and the timer
	// will permanently stop if we completely drop this firing.
	if (__CFRunLoopTimerIsFiring(rlt)) __CFRunLoopTimerSetDidFire(rlt);
    }
    if (__CFIsValid(rlt) && timerHandled) {
	if (0.0 == rlt->_interval) {
	    CFRunLoopTimerInvalidate(rlt);      /* DOES CALLOUT */
	} else {
	    /* This is just a little bit tricky: we want to support calling
	     * CFRunLoopTimerSetNextFireDate() from within the callout and
	     * honor that new time here if it is a later date, otherwise
	     * it is completely ignored. */
	    __CFRunLoopTimerFireTSRLock();
	    int64_t currentFireTSR = rlt->_fireTSR;
	    if (oldFireTSR < currentFireTSR) {
		/* Next fire TSR was set, and set to a date after the previous
		 * fire date, so we honor it. */
		if (__CFRunLoopTimerDidFire(rlt)) {
		    __CFRunLoopTimerRescheduleWithAllModes(rlt);
		    __CFRunLoopTimerUnsetDidFire(rlt);
		}
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
	        rlt->_fireTSR = currentFireTSR;
	        __CFRunLoopTimerRescheduleWithAllModes(rlt);
	    }
	    __CFRunLoopTimerFireTSRUnlock();
	}
    }
    if (__CFIsValid(rlt) && timerEarly) {
	int64_t now2 = (int64_t)mach_absolute_time();
	CFAbsoluteTime now1 = CFAbsoluteTimeGetCurrent();
	__CFRunLoopTimerFireTSRLock();
	if (rlt->_nextFireDate < now1) {
	    rlt->_fireTSR = now2;
	} else if (now1 + __CFTSRToTimeInterval(LLONG_MAX) < rlt->_nextFireDate) {
	    rlt->_fireTSR = LLONG_MAX;
	} else {
	    rlt->_fireTSR = now2 + __CFTimeIntervalToTSR(rlt->_nextFireDate - now1);
	}
	__CFRunLoopTimerRescheduleWithAllModes(rlt);
	__CFRunLoopTimerFireTSRUnlock();
    }
    CFRelease(rlt);
    __CFRunLoopModeLock(rlm);
    return timerHandled;
}

CF_EXPORT Boolean _CFRunLoopFinished(CFRunLoopRef rl, CFStringRef modeName) {
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    Boolean result = false;
    __CFRunLoopLock(rl);
    rlm = __CFRunLoopFindMode(rl, modeName, false);
    if (NULL == rlm || __CFRunLoopModeIsEmpty(rl, rlm, NULL)) {
	result = true;
    }
    __CFRunLoopUnlock(rl);
    if (rlm) __CFRunLoopModeUnlock(rlm);
    return result;
}

// rl is locked, rlm is locked on entry and exit
static void __CFRunLoopModeAddPortsToPortSet(CFRunLoopRef rl, CFRunLoopModeRef rlm, __CFPortSet portSet) {
    CFIndex idx, cnt;
    const void **list, *buffer[256];

    // Timers and version 1 sources go into the portSet currently
    if (NULL != rlm->_sources) {
	cnt = CFSetGetCount(rlm->_sources);
	list = (const void **)((cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0));
	CFSetGetValues(rlm->_sources, list);
	for (idx = 0; idx < cnt; idx++) {
	    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)list[idx];
	    if (1 == rls->_context.version0.version) {
		__CFPort port = rls->_context.version1.getPort(rls->_context.version1.info);	/* CALLOUT */
		if (CFPORT_NULL != port) {
		    __CFPortSetInsert(port, portSet);
		}
	    }
	}
	if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
    }
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
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
#endif
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

static __CFPortSet _LastMainWaitSet = 0;

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
// return NO if we're the main runloop and there are no messages waiting on the port set
int _CFRunLoopInputsReady(void) {
    CHECK_FOR_FORK();
    if (!pthread_main_np()) return true;

    // XXX_PCB:  can't be any messages waiting if the wait set is NULL.
    if (_LastMainWaitSet == MACH_PORT_NULL) return false;
 
    // prepare a message header with no space for any data, nor a trailer
    mach_msg_header_t msg;
    msg.msgh_size = sizeof(msg);    // just the header, ma'am
    // need the waitset, actually XXX
    msg.msgh_local_port = _LastMainWaitSet;
    msg.msgh_remote_port = MACH_PORT_NULL;
    msg.msgh_id = 0;
    
    kern_return_t ret = mach_msg(&msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_LARGE, 0, msg.msgh_size, _LastMainWaitSet, 0, MACH_PORT_NULL);
    
    return (MACH_RCV_TOO_LARGE == ret);
    return true;
}
#endif

#if 0
static void print_msg_scan_header(void) {
    printf("======== ======== ======== ========\n");
    printf("description\tport\tport type\t\treferences\n");
}

static void print_one_port_info(const char *desc, mach_port_t port, mach_msg_type_name_t type) {
    mach_port_urefs_t refs;
    kern_return_t ret = mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &refs);
    if (ret != KERN_SUCCESS) refs = 0;
    const char *type_name = "???";
    switch (type) {
    case MACH_MSG_TYPE_MOVE_SEND: type_name = "MACH_MSG_TYPE_MOVE_SEND"; break;
    case MACH_MSG_TYPE_MOVE_SEND_ONCE: type_name = "MACH_MSG_TYPE_MOVE_SEND_ONCE"; break;
    case MACH_MSG_TYPE_MOVE_RECEIVE: type_name = "MACH_MSG_TYPE_MOVE_RECEIVE"; break;
    case MACH_MSG_TYPE_MAKE_SEND: type_name = "MACH_MSG_TYPE_MAKE_SEND"; break;
    case MACH_MSG_TYPE_MAKE_SEND_ONCE: type_name = "MACH_MSG_TYPE_MAKE_SEND_ONCE"; break;
    }
    printf("%s\t%p\t%-20s\t%u\n", desc, port, type_name, refs);
}

static void mach_msg_scan(mach_msg_header_t *msg, int clean) {
    Boolean printed_header = false;
    /*
     *	The msgh_local_port field doesn't hold a port right.
     *	The receive operation consumes the destination port right.
     */
    if (MACH_PORT_NULL != msg->msgh_remote_port) {
	if (! printed_header) print_msg_scan_header();
	printed_header = true;
	print_one_port_info("msg->msgh_remote_port", msg->msgh_remote_port, MACH_MSGH_BITS_REMOTE(msg->msgh_bits));
    }
    if (msg->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
    	mach_msg_body_t *body = (mach_msg_body_t *) (msg + 1);
    	mach_msg_descriptor_t *saddr = (mach_msg_descriptor_t *) ((mach_msg_base_t *) msg + 1);
    	mach_msg_descriptor_t *eaddr =  saddr + body->msgh_descriptor_count;
	for  ( ; saddr < eaddr; saddr++) {
	    switch (saddr->type.type) {
	    case MACH_MSG_PORT_DESCRIPTOR:;
		mach_msg_port_descriptor_t *dsc = &saddr->port;
		if (! printed_header) print_msg_scan_header();
		printed_header = true;
		print_one_port_info("port in body", dsc->name, dsc->disposition);
//		if (clean) mach_port_deallocate(mach_task_self(), dsc->name);
		break;
	    case MACH_MSG_OOL_PORTS_DESCRIPTOR:;
		    mach_msg_ool_ports_descriptor_t *dsc2 = &saddr->ool_ports;
		    mach_port_t *ports = (mach_port_t *) dsc2->address;
		    for (mach_msg_type_number_t j = 0; j < dsc2->count; j++, ports++)  {
			if (! printed_header) print_msg_scan_header();
			printed_header = true;
			print_one_port_info("port in OOL ports", *ports, dsc2->disposition);
		    }
		    break;
	    }
	}
    }
}
#endif

static int32_t __CFRunLoopRun(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFTimeInterval seconds, Boolean stopAfterHandle, CFRunLoopModeRef previousMode, Boolean waitIfEmpty) __attribute__((noinline));
#if DEPLOYMENT_TARGET_WINDOWS
static int64_t __CFRunLoopGetNextTimerFireTSR(CFRunLoopRef rl, CFRunLoopModeRef rlm);
#endif

/* rl is unlocked, rlm locked on entrance and exit */
static int32_t __CFRunLoopRun(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFTimeInterval seconds, Boolean stopAfterHandle, CFRunLoopModeRef previousMode, Boolean waitIfEmpty) {  /* DOES CALLOUT */
    int64_t termTSR;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    mach_port_name_t timeoutPort = MACH_PORT_NULL;
    mach_port_name_t dispatchPort = MACH_PORT_NULL;
    Boolean libdispatchQSafe = pthread_main_np() && ((HANDLE_DISPATCH_ON_BASE_INVOCATION_ONLY && NULL == previousMode) || (!HANDLE_DISPATCH_ON_BASE_INVOCATION_ONLY && 0 == pthread_getspecific(__CFTSDKeyIsInGCDMainQ)));
    if (libdispatchQSafe && (CFRunLoopGetMain() == rl) && CFSetContainsValue(rl->_commonModes, rlm->_name)) dispatchPort = _dispatch_get_main_queue_port_4CF();
#endif
    Boolean poll = false;
    Boolean firstPass = true;

    if (__CFRunLoopIsStopped(rl)) {
        __CFRunLoopUnsetStopped(rl);
	return kCFRunLoopRunStopped;
    } else if (rlm->_stopped) {
	rlm->_stopped = false;
	return kCFRunLoopRunStopped;
    }
    if (seconds <= 0.0) {
	termTSR = 0;
    } else if (TIMER_INTERVAL_LIMIT < seconds) {
	termTSR = LLONG_MAX;
    } else {
	termTSR = (int64_t)__CFReadTSR() + __CFTimeIntervalToTSR(seconds);
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	timeoutPort = mk_timer_create();
	mk_timer_arm(timeoutPort, __CFUInt64ToAbsoluteTime(termTSR));
#endif
    }
    if (seconds <= 0.0) {
	poll = true;
    }
    if (rl == CFRunLoopGetMain()) _LastMainWaitSet = CFPORT_NULL;
    for (;;) {
        __CFPortSet waitSet = CFPORT_NULL;
        Boolean destroyWaitSet = false;
        CFRunLoopSourceRef rls;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	mach_msg_header_t *msg;
	kern_return_t ret;
        uint8_t buffer[2048];
#else
        CFArrayRef timersToCall = NULL;
#endif
	int32_t returnValue = 0;
        Boolean sourceHandledThisLoop = false;

	// Do not handle blocks here, as there is already a handling at
        // the end of this loop, and that just makes for two handlings
        // in a row when the loop cycles.
        if (rlm->_observerMask & kCFRunLoopBeforeTimers || rlm->_submodes) __CFRunLoopDoObservers(rl, rlm, kCFRunLoopBeforeTimers);
	if (rlm->_observerMask & kCFRunLoopBeforeSources || rlm->_submodes) __CFRunLoopDoObservers(rl, rlm, kCFRunLoopBeforeSources);
	if (rl->_blocks_head) {
	    __CFRunLoopModeUnlock(rlm);
            __CFRunLoopLock(rl);
	    __CFRunLoopDoBlocks(rl, rlm->_name);
            __CFRunLoopModeLock(rlm);
	    __CFRunLoopUnlock(rl);
	}

        sourceHandledThisLoop = __CFRunLoopDoSources0(rl, rlm, stopAfterHandle) || sourceHandledThisLoop;

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
        if (MACH_PORT_NULL != dispatchPort) {
            msg = (mach_msg_header_t *)buffer;
            msg->msgh_size = sizeof(buffer);
            msg->msgh_bits = 0;
            msg->msgh_local_port = dispatchPort;
            msg->msgh_remote_port = MACH_PORT_NULL;
            msg->msgh_id = 0;
            ret = mach_msg(msg, MACH_RCV_MSG|MACH_RCV_LARGE|MACH_RCV_TIMEOUT|MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)|MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AV), 0, msg->msgh_size, dispatchPort, 0, MACH_PORT_NULL);

            if (MACH_MSG_SUCCESS == ret) {
                __CFRunLoopModeUnlock(rlm);
                pthread_setspecific(__CFTSDKeyIsInGCDMainQ, (void *)3);
                _dispatch_main_queue_callback_4CF(msg);
                sourceHandledThisLoop = true;
                pthread_setspecific(__CFTSDKeyIsInGCDMainQ, (void *)0);
                __CFRunLoopLock(rl);
                __CFRunLoopModeLock(rlm);
	        __CFRunLoopUnlock(rl);
            } else if (MACH_RCV_TIMED_OUT == ret) {
                msg = NULL;
            } else {
                HALT;
            }
        }
#endif

	if (sourceHandledThisLoop) {
	    poll = true;
	    if (rl->_blocks_head) {
		__CFRunLoopModeUnlock(rlm);
		__CFRunLoopLock(rl);
		__CFRunLoopDoBlocks(rl, rlm->_name);
		__CFRunLoopModeLock(rlm);
		__CFRunLoopUnlock(rl);
	    }
	}

	if (!poll) {
	    if (rlm->_observerMask & kCFRunLoopBeforeWaiting || rlm->_submodes) __CFRunLoopDoObservers(rl, rlm, kCFRunLoopBeforeWaiting);
	    // do not do any user callouts after this point (after notifying of sleeping)
	    __CFRunLoopSetSleeping(rl);
	}
	if (NULL != rlm->_submodes) {
	    // !!! what do we do if this doesn't succeed?
            waitSet = __CFPortSetAllocate();
            if (CFPORT_NULL == waitSet) HALT;
	    __CFRunLoopModeUnlock(rlm);
	    __CFRunLoopLock(rl);
	    __CFRunLoopModeLock(rlm);
	    __CFRunLoopModeAddPortsToPortSet(rl, rlm, waitSet);
	    __CFRunLoopUnlock(rl);
            destroyWaitSet = true;
	} else {
	    waitSet = rlm->_portSet;
	}
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	if (CFPORT_NULL != timeoutPort) {
	     __CFPortSetInsert(timeoutPort, waitSet);
	}
	if (CFPORT_NULL != dispatchPort) {
	     __CFPortSetInsert(dispatchPort, waitSet);
	}
#endif
	if (rl == CFRunLoopGetMain()) _LastMainWaitSet = waitSet;
	__CFRunLoopModeUnlock(rlm);

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
        if (kCFUseCollectableAllocator && !poll) {
	    memset(buffer, 0, sizeof(buffer));
	    objc_clear_stack(0);
	}
        
        msg = (mach_msg_header_t *)buffer;
	msg->msgh_size = sizeof(buffer);

//static uint64_t _last_value = 0;
//uint64_t new_val = mach_absolute_time();
//printf(". %d before %qd (%qd)\n", getpid(), new_val, new_val - _last_value);
//_last_value = new_val;

	/* In that sleep of death what nightmares may come ... */
	try_receive:;
	msg->msgh_bits = 0;
	msg->msgh_local_port = waitSet;
	msg->msgh_remote_port = MACH_PORT_NULL;
	msg->msgh_id = 0;
	ret = mach_msg(msg, MACH_RCV_MSG|MACH_RCV_LARGE|(poll ? MACH_RCV_TIMEOUT : 0)|MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)|MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AV), 0, msg->msgh_size, waitSet, 0, MACH_PORT_NULL);

	if (MACH_RCV_TOO_LARGE == ret) {
	    uint32_t newSize = round_msg(msg->msgh_size + MAX_TRAILER_SIZE);
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
//new_val = mach_absolute_time();
//printf(". %d after %qd (%qd)\n", getpid(), new_val, new_val - _last_value);
//_last_value = new_val;
#elif DEPLOYMENT_TARGET_WINDOWS
        DWORD waitResult = WAIT_TIMEOUT;
        HANDLE handleBuf[MAXIMUM_WAIT_OBJECTS];
        HANDLE *handles;
        uint32_t handleCount;
        Boolean freeHandles;
        if (destroyWaitSet) {
            // wait set is a local, no one else could modify it, no need to copy handles
            handles = waitSet->handles;
            handleCount = waitSet->used;
            freeHandles = FALSE;
        } else {
            // copy out the handles to be safe from other threads at work
            handles = __CFPortSetGetPorts(waitSet, handleBuf, MAXIMUM_WAIT_OBJECTS, &handleCount);
            freeHandles = (handles != handleBuf);
        }
        // should msgQMask be an OR'ing of this and all submodes' masks?
	if (0 == GetQueueStatus(rlm->_msgQMask)) {
	    DWORD timeout;
	    if (poll)
		timeout = 0;
	    else {
		int64_t nextStop;
                __CFRunLoopModeLock(rlm);
                nextStop = __CFRunLoopGetNextTimerFireTSR(rl, rlm);
		if (nextStop <= 0)
		    nextStop = termTSR;
		else if (nextStop > termTSR)
		    nextStop = termTSR;
		// else the next stop is dictated by the next timer
		int64_t timeoutTSR = nextStop - __CFReadTSR();
		if (timeoutTSR < 0)
		    timeout = 0;
		else {
		    CFTimeInterval timeoutCF = __CFTSRToTimeInterval(timeoutTSR) * 1000;
		    if (timeoutCF > MAXDWORD)
			timeout = INFINITE;
		    else
			timeout = (DWORD)timeoutCF;
		}
	    }
            waitResult = WaitForMultipleObjects(__CFMin(handleCount, MAXIMUM_WAIT_OBJECTS), handles, false, timeout);
	}
	ResetEvent(rl->_wakeUpPort);
#endif
	__CFRunLoopLock(rl);
	__CFRunLoopModeLock(rlm);
	__CFRunLoopUnlock(rl);
	if (destroyWaitSet) {
            __CFPortSetFree(waitSet);
	    if (rl == CFRunLoopGetMain()) _LastMainWaitSet = 0;
	} else {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	    if (CFPORT_NULL != timeoutPort) {
		__CFPortSetRemove(timeoutPort, waitSet);
	    }
	    if (CFPORT_NULL != dispatchPort) {
		__CFPortSetRemove(dispatchPort, waitSet);
	    }
#endif
	}

	if (!poll) {
	    __CFRunLoopUnsetSleeping(rl);
	    if (rlm->_observerMask & kCFRunLoopAfterWaiting || rlm->_submodes) __CFRunLoopDoObservers(rl, rlm, kCFRunLoopAfterWaiting);
	}
	poll = false;
	__CFRunLoopModeUnlock(rlm);
	__CFRunLoopLock(rl);
	__CFRunLoopModeLock(rlm);

        __CFPort livePort = CFPORT_NULL;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	if (NULL != msg) {
            livePort = msg->msgh_local_port;
        }
#elif DEPLOYMENT_TARGET_WINDOWS
        CFAssert2(waitResult != WAIT_FAILED, __kCFLogAssertion, "%s(): error %d from MsgWaitForMultipleObjects", __PRETTY_FUNCTION__, GetLastError());
        if (waitResult == WAIT_TIMEOUT) {
            // do nothing, just return to caller
        } else if (waitResult >= WAIT_OBJECT_0 && waitResult < WAIT_OBJECT_0+handleCount) {
            // a handle was signaled
            livePort = handles[waitResult-WAIT_OBJECT_0];
        } else if (waitResult == WAIT_OBJECT_0+handleCount) {
            // windows message received - the CFWindowsMessageQueue will pick this up when
            // the v0 RunLoopSources get their chance
        } else if (waitResult >= WAIT_ABANDONED_0 && waitResult < WAIT_ABANDONED_0+handleCount) {
            // an "abandoned mutex object"
            livePort = handles[waitResult-WAIT_ABANDONED_0];
        } else {
            CFAssert2(waitResult == WAIT_FAILED, __kCFLogAssertion, "%s(): unexpected result from MsgWaitForMultipleObjects: %d", __PRETTY_FUNCTION__, waitResult);
        }
        if (freeHandles)
            CFAllocatorDeallocate(kCFAllocatorSystemDefault, handles);
        timersToCall = __CFRunLoopTimersToFire(rl, rlm);
#endif

	if (CFPORT_NULL == livePort) {
	    __CFRunLoopUnlock(rl);
	} else if (livePort == rl->_wakeUpPort) {
	    // wakeup
		if (_LogCFRunLoop) { CFLog(kCFLogLevelDebug, CFSTR("wakeupPort was signalled")); }
	    __CFRunLoopUnlock(rl); // leave run loop unlocked
	}
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	else if (livePort == timeoutPort) {
	    returnValue = kCFRunLoopRunTimedOut;
	    __CFRunLoopUnlock(rl);
	} else if (dispatchPort == livePort) {
	    __CFRunLoopModeUnlock(rlm);
	    __CFRunLoopUnlock(rl);
            pthread_setspecific(__CFTSDKeyIsInGCDMainQ, (void *)3);
 	    _dispatch_main_queue_callback_4CF(msg);
 	    sourceHandledThisLoop = true;
            pthread_setspecific(__CFTSDKeyIsInGCDMainQ, (void *)0);
	    __CFRunLoopLock(rl);
	    __CFRunLoopModeLock(rlm);
	    __CFRunLoopUnlock(rl); // leave run loop unlocked
	} else if (NULL != (rls = __CFRunLoopModeFindSourceForMachPort(rl, rlm, livePort))) {
	    mach_msg_header_t *reply = NULL;
	    __CFRunLoopUnlock(rl);
//		mach_msg_scan(msg, 0);
	    if (__CFRunLoopDoSource1(rl, rlm, rls, msg, msg->msgh_size, &reply)) {
		sourceHandledThisLoop = true;
	    }
//		mach_msg_scan(msg, 1);
	    if (NULL != reply) {
		ret = mach_msg(reply, MACH_SEND_MSG, reply->msgh_size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
//#warning CF: what should be done with the return value?
		CFAllocatorDeallocate(kCFAllocatorSystemDefault, reply);
	    }
	} else {
	    CFRunLoopTimerRef rlt;
	    rlt = __CFRunLoopModeFindTimerForMachPort(rlm, livePort);
	    __CFRunLoopUnlock(rl);
	    if (NULL != rlt) {
		__CFRunLoopDoTimer(rl, rlm, rlt);
	    }
	}
	if (msg != (mach_msg_header_t *)buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, msg);
#else
	else if (NULL != (rls = __CFRunLoopModeFindSourceForMachPort(rl, rlm, livePort))) {
	    __CFRunLoopUnlock(rl);
		if (_LogCFRunLoop) { CFLog(kCFLogLevelDebug, CFSTR("Source %@ was signalled"), rls); }
	    if (__CFRunLoopDoSource1(rl, rlm, rls)) {
		sourceHandledThisLoop = true;
	    }
	}
#endif

#if DEPLOYMENT_TARGET_WINDOWS
        if (NULL != timersToCall) {
            int i;
            for (i = CFArrayGetCount(timersToCall)-1; i >= 0; i--)
                __CFRunLoopDoTimer(rl, rlm, (CFRunLoopTimerRef)CFArrayGetValueAtIndex(timersToCall, i));
            CFRelease(timersToCall);
        }
#endif
        
	if (rl->_blocks_head) {
	    __CFRunLoopModeUnlock(rlm);	// locks must be taken in order
	    __CFRunLoopLock(rl);
	    __CFRunLoopDoBlocks(rl, rlm->_name);
	    __CFRunLoopModeLock(rlm);
	    __CFRunLoopUnlock(rl);
	}
	if (sourceHandledThisLoop && stopAfterHandle) {
	    returnValue = kCFRunLoopRunHandledSource;
        // If we're about to timeout, but we just did a zero-timeout poll that only found our own
        // internal wakeup signal on the first look at the portset, we'll go around the loop one
        // more time, so as not to starve a v1 source that was just added along with a runloop wakeup.
        } else if (0 != returnValue || (uint64_t)termTSR <= __CFReadTSR()) {
	    returnValue = kCFRunLoopRunTimedOut;
	} else if (__CFRunLoopIsStopped(rl)) {
            __CFRunLoopUnsetStopped(rl);
	    returnValue = kCFRunLoopRunStopped;
	} else if (rlm->_stopped) {
	    rlm->_stopped = false;
	    returnValue = kCFRunLoopRunStopped;
	} else if (!waitIfEmpty && __CFRunLoopModeIsEmpty(rl, rlm, previousMode)) {
	    returnValue = kCFRunLoopRunFinished;
	}
	if (0 != returnValue) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	    if (MACH_PORT_NULL != timeoutPort) {
		mk_timer_destroy(timeoutPort);
	    }
#endif
	    return returnValue;
	}
        firstPass = false;
    }
}

SInt32 CFRunLoopRunSpecific(CFRunLoopRef rl, CFStringRef modeName, CFTimeInterval seconds, Boolean returnAfterSourceHandled) {     /* DOES CALLOUT */
    CHECK_FOR_FORK();
    if (__CFRunLoopIsDeallocating(rl)) return kCFRunLoopRunFinished;
    __CFRunLoopLock(rl);
    CFRunLoopModeRef currentMode = __CFRunLoopFindMode(rl, modeName, false);
    if (NULL == currentMode || __CFRunLoopModeIsEmpty(rl, currentMode, rl->_currentMode)) {
	Boolean did = false;
	if (currentMode) __CFRunLoopModeUnlock(currentMode);
#if 0
	if (rl->_blocks_head) {
	    __CFRunLoopDoBlocks(rl, modeName);
	}
#endif
	__CFRunLoopUnlock(rl);
	return did ? kCFRunLoopRunHandledSource : kCFRunLoopRunFinished;
    }
    uint32_t *previousStopped = (uint32_t *)rl->_stopped;
    rl->_stopped = NULL;
    CFRunLoopModeRef previousMode = rl->_currentMode;
    rl->_currentMode = currentMode;
#if DEPLOYMENT_TARGET_WINDOWS
    if (previousMode && currentMode != previousMode) {
        if (__CFIsNonMachRunLoopMarryMsgQueueEnabled())
            __CFRunLoopUpdateMsgWaitMarryMsgQueue(rl);
        else
            __CFRunLoopUpdateMsgWait(rl);
    }
#endif
    __CFRunLoopUnlock(rl);
    int32_t result;
    @try {
	if (currentMode->_observerMask & kCFRunLoopEntry || currentMode->_submodes) __CFRunLoopDoObservers(rl, currentMode, kCFRunLoopEntry);
	result = __CFRunLoopRun(rl, currentMode, seconds, returnAfterSourceHandled, previousMode, false);
	if (currentMode->_observerMask & kCFRunLoopExit || currentMode->_submodes) __CFRunLoopDoObservers(rl, currentMode, kCFRunLoopExit);
    } @finally {
	// this kvetches if an exception was raised during a run loop callout, because the mode is not locked
	if (0 != currentMode->_lock) {
	    __CFRunLoopModeUnlock(currentMode);
	}
	__CFRunLoopLock(rl);
	if (rl->_stopped) CFAllocatorDeallocate(kCFAllocatorSystemDefault, (uint32_t *)rl->_stopped);
	rl->_stopped = previousStopped;
	rl->_currentMode = previousMode;
#if DEPLOYMENT_TARGET_WINDOWS
        if (previousMode && currentMode != previousMode) {
            if (__CFIsNonMachRunLoopMarryMsgQueueEnabled())
                __CFRunLoopUpdateMsgWaitMarryMsgQueue(rl);
            else
                __CFRunLoopUpdateMsgWait(rl);
        }
#endif
	__CFRunLoopUnlock(rl);
    }
    return result;
}

void CFRunLoopRun(void) {	/* DOES CALLOUT */
    int32_t result;
    do {
        result = CFRunLoopRunSpecific(CFRunLoopGetCurrent(), kCFRunLoopDefaultMode, 1.0e10, false);
        CHECK_FOR_FORK();
    } while (kCFRunLoopRunStopped != result && kCFRunLoopRunFinished != result);
}

SInt32 CFRunLoopRunInMode(CFStringRef modeName, CFTimeInterval seconds, Boolean returnAfterSourceHandled) {     /* DOES CALLOUT */
    CHECK_FOR_FORK();
    return CFRunLoopRunSpecific(CFRunLoopGetCurrent(), modeName, seconds, returnAfterSourceHandled);
}

static void __CFRunLoopFindMinTimer(const void *value, void *ctx) {
    CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)value;
    if (__CFIsValid(rlt)) {
        CFRunLoopTimerRef *result = (CFRunLoopTimerRef *)ctx;
        if (NULL == *result || rlt->_fireTSR < (*result)->_fireTSR) {
            *result = rlt;
        }
    }
}

#if DEPLOYMENT_TARGET_WINDOWS
static int64_t __CFRunLoopGetNextTimerFireTSR(CFRunLoopRef rl, CFRunLoopModeRef rlm) {
    CFRunLoopTimerRef result = NULL;
    int64_t fireTime = 0;
    if (rlm) {
        if (NULL != rlm->_timers && 0 < CFSetGetCount(rlm->_timers)) {
            __CFRunLoopTimerFireTSRLock();
            CFSetApplyFunction(rlm->_timers, (__CFRunLoopFindMinTimer), &result);
            if (result)
                fireTime = result->_fireTSR;
            __CFRunLoopTimerFireTSRUnlock();
        }
        if (NULL != rlm->_submodes) {
            CFIndex idx, cnt;
            for (idx = 0, cnt = CFArrayGetCount(rlm->_submodes); idx < cnt; idx++) {
                CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
                CFRunLoopModeRef subrlm;
                subrlm = __CFRunLoopFindMode(rl, modeName, false);
                if (NULL != subrlm) {
                    int64_t newFireTime = __CFRunLoopGetNextTimerFireTSR(rl, subrlm);
                    __CFRunLoopModeUnlock(subrlm);
                    if (fireTime == 0 || (newFireTime != 0 && newFireTime < fireTime))
                        fireTime = newFireTime;
                }
            }
        }
        __CFRunLoopModeUnlock(rlm);
    }
    return fireTime;
}
#endif
    
// called with rlm locked, if not NULL
static CFAbsoluteTime __CFRunLoopGetNextTimerFireDate(CFRunLoopRef rl, CFRunLoopModeRef rlm) {
    CFRunLoopTimerRef result = NULL;
    CFAbsoluteTime fireDate = 0.0;
    int64_t now2 = (int64_t)mach_absolute_time();
    CFAbsoluteTime now1 = CFAbsoluteTimeGetCurrent();
    if (rlm) {
	if (NULL != rlm->_timers && 0 < CFSetGetCount(rlm->_timers)) {
	    __CFRunLoopTimerFireTSRLock();
	    CFSetApplyFunction(rlm->_timers, (__CFRunLoopFindMinTimer), &result);
            if (result) {
		fireDate = (0 == result->_fireTSR) ? 0.0 : (now1 + __CFTSRToTimeInterval(result->_fireTSR - now2));
	    }
	    __CFRunLoopTimerFireTSRUnlock();
	}
        if (NULL != rlm->_submodes) {
            CFIndex idx, cnt;
            for (idx = 0, cnt = CFArrayGetCount(rlm->_submodes); idx < cnt; idx++) {
                CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
                CFRunLoopModeRef subrlm;
                subrlm = __CFRunLoopFindMode(rl, modeName, false);
                if (NULL != subrlm) {
                    CFAbsoluteTime newFireDate = __CFRunLoopGetNextTimerFireDate(rl, subrlm);
                    __CFRunLoopModeUnlock(subrlm);
                    if (fireDate == 0 || (newFireDate != 0.0 && newFireDate < fireDate))
                        fireDate = newFireDate;
                }
            }
        }
        __CFRunLoopModeUnlock(rlm);
    }
    return fireDate;
}

CFAbsoluteTime CFRunLoopGetNextTimerFireDate(CFRunLoopRef rl, CFStringRef modeName) {
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    rlm = __CFRunLoopFindMode(rl, modeName, false);
    __CFRunLoopUnlock(rl);
    return __CFRunLoopGetNextTimerFireDate(rl, rlm);
}

Boolean CFRunLoopIsWaiting(CFRunLoopRef rl) {
    CHECK_FOR_FORK();
    return __CFRunLoopIsSleeping(rl);
}

void CFRunLoopWakeUp(CFRunLoopRef rl) {
    CHECK_FOR_FORK();
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    kern_return_t ret;
    /* We unconditionally try to send the message, since we don't want
     * to lose a wakeup, but the send may fail if there is already a
     * wakeup pending, since the queue length is 1. */
    ret = __CFSendTrivialMachMessage(rl->_wakeUpPort, 0, MACH_SEND_TIMEOUT, 0);
    if (ret != MACH_MSG_SUCCESS && ret != MACH_SEND_TIMED_OUT) {
	HALT;
    }
#else
    SetEvent(rl->_wakeUpPort);
    __CFRunLoopLock(rl);
    if (rl->_threadID == GetCurrentThreadId()) {
        PostMessageW(NULL, __kCFRunLoopWakeUpMessage, NULL, NULL);
    } else {
        PostThreadMessageW(rl->_threadID, __kCFRunLoopWakeUpMessage, NULL, NULL);
    }
    __CFRunLoopUnlock(rl);
#endif
}

void CFRunLoopStop(CFRunLoopRef rl) {
    Boolean doWake = false;
    CHECK_FOR_FORK();
    __CFRunLoopLock(rl);
    if (rl->_currentMode) {
        __CFRunLoopSetStopped(rl);
        doWake = true;
    }
    __CFRunLoopUnlock(rl);
    if (doWake) {
        CFRunLoopWakeUp(rl);
    }
}

CF_EXPORT void _CFRunLoopStopMode(CFRunLoopRef rl, CFStringRef modeName) {
    CHECK_FOR_FORK();
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
    CHECK_FOR_FORK();
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
    CHECK_FOR_FORK();
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
		rlm->_submodes = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
	    }
	    if (!CFArrayContainsValue(rlm->_submodes, CFRangeMake(0, CFArrayGetCount(rlm->_submodes)), modeName)) {
		CFArrayAppendValue(rlm->_submodes, modeName);
	    }
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}

CF_EXPORT void _CFRunLoopRemoveModeFromMode(CFRunLoopRef rl, CFStringRef modeName, CFStringRef fromModeName) {
    CHECK_FOR_FORK();
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

void CFRunLoopPerformBlock(CFRunLoopRef rl, CFTypeRef mode, void (^block)(void)) {
    CHECK_FOR_FORK();
    if (CFStringGetTypeID() == CFGetTypeID(mode)) {
	mode = CFStringCreateCopy(kCFAllocatorSystemDefault, (CFStringRef)mode);
        __CFRunLoopLock(rl);
	// ensure mode exists
        CFRunLoopModeRef currentMode = __CFRunLoopFindMode(rl, (CFStringRef)mode, true);
        if (currentMode) __CFRunLoopModeUnlock(currentMode);
        __CFRunLoopUnlock(rl);
    } else if (CFArrayGetTypeID() == CFGetTypeID(mode)) {
        CFIndex cnt = CFArrayGetCount((CFArrayRef)mode);
	const void **values = (const void **)malloc(sizeof(const void *) * cnt);
        CFArrayGetValues((CFArrayRef)mode, CFRangeMake(0, cnt), values);
	mode = CFSetCreate(kCFAllocatorSystemDefault, values, cnt, &kCFTypeSetCallBacks);
        __CFRunLoopLock(rl);
	// ensure modes exist
	for (CFIndex idx = 0; idx < cnt; idx++) {
            CFRunLoopModeRef currentMode = __CFRunLoopFindMode(rl, (CFStringRef)values[idx], true);
            if (currentMode) __CFRunLoopModeUnlock(currentMode);
	}
        __CFRunLoopUnlock(rl);
	free(values);
    } else if (CFSetGetTypeID() == CFGetTypeID(mode)) {
        CFIndex cnt = CFSetGetCount((CFSetRef)mode);
	const void **values = (const void **)malloc(sizeof(const void *) * cnt);
        CFSetGetValues((CFSetRef)mode, values);
	mode = CFSetCreate(kCFAllocatorSystemDefault, values, cnt, &kCFTypeSetCallBacks);
        __CFRunLoopLock(rl);
	// ensure modes exist
	for (CFIndex idx = 0; idx < cnt; idx++) {
            CFRunLoopModeRef currentMode = __CFRunLoopFindMode(rl, (CFStringRef)values[idx], true);
            if (currentMode) __CFRunLoopModeUnlock(currentMode);
	}
        __CFRunLoopUnlock(rl);
	free(values);
    } else {
	mode = NULL;
    }
#if DEPLOYMENT_TARGET_WINDOWS
    // <rdar://problem/6496715> Block.h uses "typeof", which is not available when using Visual Studio on Windows
    block = (void (^)(void))_Block_copy(block);
#else
    block = Block_copy(block);
#endif
    if (!mode || !block) {
	if (mode) CFRelease(mode);
	if (block) Block_release(block);
	return;
    }
    __CFRunLoopLock(rl);
    struct _block_item *new_item = (struct _block_item *)malloc(sizeof(struct _block_item));
    new_item->_next = NULL;
    new_item->_mode = mode;
    new_item->_block = block;
    if (!rl->_blocks_tail) {
	rl->_blocks_head = new_item;
    } else {
	rl->_blocks_tail->_next = new_item;
    }
    rl->_blocks_tail = new_item;
    __CFRunLoopUnlock(rl);
}

Boolean CFRunLoopContainsSource(CFRunLoopRef rl, CFRunLoopSourceRef rls, CFStringRef modeName) {
    CHECK_FOR_FORK();
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
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    if (__CFRunLoopIsDeallocating(rl)) return;
    if (!__CFIsValid(rls)) return;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
	if (NULL == rl->_commonModeItems) {
	    rl->_commonModeItems = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
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
	    rlm->_sources = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
	    rlm->_portToV1SourceMap = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, NULL);
	    _CFSetSetCapacity(rlm->_sources, 10);
	}
	if (NULL != rlm && !CFSetContainsValue(rlm->_sources, rls)) {
	    CFSetAddValue(rlm->_sources, rls);
	    if (1 == rls->_context.version0.version) {
		__CFPort src_port = rls->_context.version1.getPort(rls->_context.version1.info);
		CFDictionarySetValue(rlm->_portToV1SourceMap, (const void *)(uintptr_t)src_port, rls);
	    }
	    __CFRunLoopModeUnlock(rlm);
	    __CFRunLoopSourceSchedule(rls, rl, rlm);	/* DOES CALLOUT */
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}

void CFRunLoopRemoveSource(CFRunLoopRef rl, CFRunLoopSourceRef rls, CFStringRef modeName) {	/* DOES CALLOUT */
    CHECK_FOR_FORK();
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
	    if (1 == rls->_context.version0.version) {
		__CFPort src_port = rls->_context.version1.getPort(rls->_context.version1.info);
		CFDictionaryRemoveValue(rlm->_portToV1SourceMap, (const void *)(uintptr_t)src_port);
	    }
	    CFSetRemoveValue(rlm->_sources, rls);
	    __CFRunLoopModeUnlock(rlm);
	    __CFRunLoopSourceCancel(rls, rl, rlm);	/* DOES CALLOUT */
	    CFRelease(rls);
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}

static void __CFRunLoopRemoveSourcesFromCommonMode(const void *value, void *ctx) {
    CFStringRef modeName = (CFStringRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)ctx;
    __CFRunLoopRemoveAllSources(rl, modeName);
}

static void __CFRunLoopRemoveSourceFromMode(const void *value, void *ctx) {
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)(((CFTypeRef *)ctx)[0]);
    CFStringRef modeName = (CFStringRef)(((CFTypeRef *)ctx)[1]);
    CFRunLoopRemoveSource(rl, rls, modeName);
}

static void __CFRunLoopRemoveAllSources(CFRunLoopRef rl, CFStringRef modeName) {
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	if (NULL != rl->_commonModeItems) {
	    CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
	    __CFRunLoopUnlock(rl);
	    if (NULL != set) {
                CFSetApplyFunction(set, (__CFRunLoopRemoveSourcesFromCommonMode), (void *)rl);
		CFRelease(set);
	    }
	} else {
	    __CFRunLoopUnlock(rl);
	}
    } else {
	rlm = __CFRunLoopFindMode(rl, modeName, false);
	__CFRunLoopUnlock(rl);
	if (NULL != rlm && NULL != rlm->_sources) {
	    CFSetRef set = CFSetCreateCopy(kCFAllocatorSystemDefault, rlm->_sources);
	    __CFRunLoopModeUnlock(rlm);
            CFTypeRef context[2] = {rl, modeName};
            CFSetApplyFunction(set, (__CFRunLoopRemoveSourceFromMode), (void *)context);
	    CFRelease(set);
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}

Boolean CFRunLoopContainsObserver(CFRunLoopRef rl, CFRunLoopObserverRef rlo, CFStringRef modeName) {
    CHECK_FOR_FORK();
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
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    if (__CFRunLoopIsDeallocating(rl)) return;
    if (!__CFIsValid(rlo) || (NULL != rlo->_runLoop && rlo->_runLoop != rl)) return;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
	if (NULL == rl->_commonModeItems) {
	    rl->_commonModeItems = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
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
	    rlm->_observers = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
	}
	if (NULL != rlm && !CFSetContainsValue(rlm->_observers, rlo)) {
	    CFSetAddValue(rlm->_observers, rlo);
	    rlm->_observerMask |= rlo->_activities;
	    __CFRunLoopModeUnlock(rlm);
	    __CFRunLoopObserverSchedule(rlo, rl, rlm);
	} else if (NULL != rlm) {
	    __CFRunLoopModeUnlock(rlm);
	}
    }
}

void CFRunLoopRemoveObserver(CFRunLoopRef rl, CFRunLoopObserverRef rlo, CFStringRef modeName) {
    CHECK_FOR_FORK();
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
    CHECK_FOR_FORK();
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
    CHECK_FOR_FORK();
//CFLog(4, CFSTR("CFRunLoopAddTimer(%p, timer %p, %@)"), rl, rlt, modeName);
    CFRunLoopModeRef rlm;
    if (__CFRunLoopIsDeallocating(rl)) return;
    if (!__CFIsValid(rlt) || (NULL != rlt->_runLoop && rlt->_runLoop != rl)) return;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
	CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
	if (NULL == rl->_commonModeItems) {
	    rl->_commonModeItems = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
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
	    rlm->_timers = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
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
    CHECK_FOR_FORK();
//CFLog(4, CFSTR("CFRunLoopRemoveTimer(%p, timer %p, %@)"), rl, rlt, modeName);
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
    if (__CFIsValid(rls1) != __CFIsValid(rls2)) return false;
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
	void *addr = rls->_context.version0.version == 0 ? (void *)rls->_context.version0.perform : (rls->_context.version0.version == 1 ? (void *)rls->_context.version1.perform : NULL);
#if DEPLOYMENT_TARGET_WINDOWS
	contextDesc = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopSource context>{version = %ld, info = %p, callout = %p}"), rls->_context.version0.version, rls->_context.version0.info, addr);
#elif DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	Dl_info info;
	const char *name = (dladdr(addr, &info) && info.dli_saddr == addr && info.dli_sname) ? info.dli_sname : "???";
	contextDesc = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopSource context>{version = %ld, info = %p, callout = %s (%p)}"), rls->_context.version0.version, rls->_context.version0.info, name, addr);
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif
    }
#if DEPLOYMENT_TARGET_WINDOWS
    result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopSource %p [%p]>{locked = %s, signalled = %s, valid = %s, order = %d, context = %@}"), cf, CFGetAllocator(rls), "unknown", __CFRunLoopSourceIsSignaled(rls) ? "Yes" : "No", __CFIsValid(rls) ? "Yes" : "No", rls->_order, contextDesc);
#else
    result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopSource %p [%p]>{locked = %s, signalled = %s, valid = %s, order = %d, context = %@}"), cf, CFGetAllocator(rls), lockCount(rls->_lock) ? "Yes" : "No", __CFRunLoopSourceIsSignaled(rls) ? "Yes" : "No", __CFIsValid(rls) ? "Yes" : "No", rls->_order, contextDesc);
#endif
    CFRelease(contextDesc);
    return result;
}

static void __CFRunLoopSourceDeallocate(CFTypeRef cf) {	/* DOES CALLOUT */
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)cf;
    CFRunLoopSourceInvalidate(rls);
    if (rls->_context.version0.release) {
	rls->_context.version0.release(rls->_context.version0.info);
    }
    memset((char *)cf + sizeof(CFRuntimeBase), 0, sizeof(struct __CFRunLoopSource) - sizeof(CFRuntimeBase));
}

static const CFRuntimeClass __CFRunLoopSourceClass = {
    _kCFRuntimeScannedObject,
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
    CHECK_FOR_FORK();
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
    CF_SPINLOCK_INIT_FOR_STRUCTS(memory->_lock);
    memory->_bits = 0;
    memory->_order = order;
    memory->_runLoops = NULL;
    size = 0;
    switch (context->version) {
    case 0:
	size = sizeof(CFRunLoopSourceContext);
	break;
    case 1:
	size = sizeof(CFRunLoopSourceContext1);
	break;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    case 2:
	size = sizeof(CFRunLoopSourceContext2);
	break;
#endif
    }
    objc_memmove_collectable(&memory->_context, context, size);
    if (context->retain) {
	memory->_context.version0.info = (void *)context->retain(context->info);
    }
    return memory;
}

CFIndex CFRunLoopSourceGetOrder(CFRunLoopSourceRef rls) {
    CHECK_FOR_FORK();
    __CFGenericValidateType(rls, __kCFRunLoopSourceTypeID);
    return rls->_order;
}

static void __CFRunLoopSourceWakeUpLoop(const void *value, void *context) {
    CFRunLoopWakeUp((CFRunLoopRef)value);
}

static void __CFRunLoopSourceRemoveFromRunLoop(const void *value, void *context) {
    CFRunLoopRef rl = (CFRunLoopRef)value;
    CFTypeRef *params = (CFTypeRef *)context;
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)params[0];
    CFArrayRef array;
    CFIndex idx;
    if (rl == params[1]) return;
    array = CFRunLoopCopyAllModes(rl);
    for (idx = CFArrayGetCount(array); idx--;) {
	CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(array, idx);
	CFRunLoopRemoveSource(rl, rls, modeName);
    }
    CFRunLoopRemoveSource(rl, rls, kCFRunLoopCommonModes);
    CFRelease(array);
    params[1] = rl;
}

void CFRunLoopSourceInvalidate(CFRunLoopSourceRef rls) {
    CHECK_FOR_FORK();
    __CFGenericValidateType(rls, __kCFRunLoopSourceTypeID);
    CFRetain(rls);
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls)) {
	__CFUnsetValid(rls);
        __CFRunLoopSourceUnsetSignaled(rls);
	if (NULL != rls->_runLoops) {
	    CFTypeRef params[2] = {rls, NULL};
	    CFBagRef bag = rls->_runLoops;
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
    CHECK_FOR_FORK();
    __CFGenericValidateType(rls, __kCFRunLoopSourceTypeID);
    return __CFIsValid(rls);
}

void CFRunLoopSourceGetContext(CFRunLoopSourceRef rls, CFRunLoopSourceContext *context) {
    CHECK_FOR_FORK();
    __CFGenericValidateType(rls, __kCFRunLoopSourceTypeID);
    CFAssert1(0 == context->version || 1 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0 or 1", __PRETTY_FUNCTION__);
    CFIndex size = 0;
    switch (context->version) {
    case 0:
	size = sizeof(CFRunLoopSourceContext);
	break;
    case 1:
	size = sizeof(CFRunLoopSourceContext1);
	break;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    case 2:
	size = sizeof(CFRunLoopSourceContext2);
	break;
#endif
    }
    memmove(context, &rls->_context, size);
}

void CFRunLoopSourceSignal(CFRunLoopSourceRef rls) {
    CHECK_FOR_FORK();
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls)) {
	__CFRunLoopSourceSetSignaled(rls);
    }
    __CFRunLoopSourceUnlock(rls);
}

Boolean CFRunLoopSourceIsSignalled(CFRunLoopSourceRef rls) {
    CHECK_FOR_FORK();
    __CFRunLoopSourceLock(rls);
    Boolean ret = __CFRunLoopSourceIsSignaled(rls) ? true : false;
    __CFRunLoopSourceUnlock(rls);
    return ret;
}

__private_extern__ void _CFRunLoopSourceWakeUpRunLoops(CFRunLoopSourceRef rls) {
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls) && NULL != rls->_runLoops) {
	CFBagApplyFunction(rls->_runLoops, __CFRunLoopSourceWakeUpLoop, NULL);
    }
    __CFRunLoopSourceUnlock(rls);
}

/* CFRunLoopObserver */

static CFStringRef __CFRunLoopObserverCopyDescription(CFTypeRef cf) {	/* DOES CALLOUT */
    CFRunLoopObserverRef rlo = (CFRunLoopObserverRef)cf;
    CFStringRef result;
    CFStringRef contextDesc = NULL;
    if (NULL != rlo->_context.copyDescription) {
	contextDesc = rlo->_context.copyDescription(rlo->_context.info);
    }
    if (!contextDesc) {
	contextDesc = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopObserver context %p>"), rlo->_context.info);
    }
#if DEPLOYMENT_TARGET_WINDOWS
    result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopObserver %p [%p]>{locked = %s, valid = %s, activities = 0x%x, repeats = %s, order = %d, callout = %p, context = %@}"), cf, CFGetAllocator(rlo), "unknown", __CFIsValid(rlo) ? "Yes" : "No", rlo->_activities, __CFRunLoopObserverRepeats(rlo) ? "Yes" : "No", rlo->_order, rlo->_callout, contextDesc);    
#elif DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    void *addr = rlo->_callout;
    Dl_info info;
    const char *name = (dladdr(addr, &info) && info.dli_saddr == addr && info.dli_sname) ? info.dli_sname : "???";
    result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopObserver %p [%p]>{locked = %s, valid = %s, activities = 0x%x, repeats = %s, order = %d, callout = %s (%p), context = %@}"), cf, CFGetAllocator(rlo), lockCount(rlo->_lock) ? "Yes" : "No", __CFIsValid(rlo) ? "Yes" : "No", rlo->_activities, __CFRunLoopObserverRepeats(rlo) ? "Yes" : "No", rlo->_order, name, addr, contextDesc);
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif
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
    CHECK_FOR_FORK();
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
    CF_SPINLOCK_INIT_FOR_STRUCTS(memory->_lock);
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
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlo, __kCFRunLoopObserverTypeID);
    return rlo->_activities;
}

CFIndex CFRunLoopObserverGetOrder(CFRunLoopObserverRef rlo) {
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlo, __kCFRunLoopObserverTypeID);
    return rlo->_order;
}

Boolean CFRunLoopObserverDoesRepeat(CFRunLoopObserverRef rlo) {
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlo, __kCFRunLoopObserverTypeID);
    return __CFRunLoopObserverRepeats(rlo);
}

void CFRunLoopObserverInvalidate(CFRunLoopObserverRef rlo) {	/* DOES CALLOUT */
    CHECK_FOR_FORK();
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
		CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(array, idx);
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
    CHECK_FOR_FORK();
    return __CFIsValid(rlo);
}

void CFRunLoopObserverGetContext(CFRunLoopObserverRef rlo, CFRunLoopObserverContext *context) {
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlo, __kCFRunLoopObserverTypeID);
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    *context = rlo->_context;
}

/* CFRunLoopTimer */

static CFStringRef __CFRunLoopTimerCopyDescription(CFTypeRef cf) {	/* DOES CALLOUT */
    CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)cf;
    CFStringRef result;
    int64_t now2 = (int64_t)mach_absolute_time();
    CFAbsoluteTime now1 = CFAbsoluteTimeGetCurrent();
    __CFRunLoopTimerFireTSRLock();
    int64_t fireTime = rlt->_fireTSR;
    __CFRunLoopTimerFireTSRUnlock();
    CFAbsoluteTime fireDate = now1 + __CFTSRToTimeInterval(fireTime - now2);
    CFStringRef contextDesc = NULL;
    if (NULL != rlt->_context.copyDescription) {
	contextDesc = rlt->_context.copyDescription(rlt->_context.info);
    }
    if (NULL == contextDesc) {
	contextDesc = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopTimer context %p>"), rlt->_context.info);
    }
#if DEPLOYMENT_TARGET_WINDOWS
    result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopTimer %p [%p]>{locked = %s, valid = %s, interval = %0.09g, next fire date = %0.09g, order = %d, callout = %p, context = %@}"), cf, CFGetAllocator(rlt), "unknown", __CFIsValid(rlt) ? "Yes" : "No", __CFTSRToTimeInterval(rlt->_intervalTSR), now1 + __CFTSRToTimeInterval(fireTime - now2), rlt->_order, rlt->_callout, contextDesc);
#elif DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    void *addr = rlt->_callout;
    Dl_info info;
    const char *name = (dladdr(addr, &info) && info.dli_saddr == addr && info.dli_sname) ? info.dli_sname : "???";
    result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopTimer %p [%p]>{locked = %s, valid = %s, interval = %0.09g, next fire date = %0.09g, order = %d, callout = %s (%p), context = %@}"), cf, CFGetAllocator(rlt), lockCount(rlt->_lock) ? "Yes" : "No", __CFIsValid(rlt) ? "Yes" : "No", rlt->_interval, fireDate, rlt->_order, name, addr, contextDesc);
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif
    CFRelease(contextDesc);
    return result;
}

static void __CFRunLoopTimerDeallocate(CFTypeRef cf) {	/* DOES CALLOUT */
//CFLog(6, CFSTR("__CFRunLoopTimerDeallocate(%p)"), cf);
    CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)cf;
    __CFRunLoopTimerSetDeallocating(rlt);
    CFRunLoopTimerInvalidate(rlt);	/* DOES CALLOUT */
    CFRelease(rlt->_rlModes);
    rlt->_rlModes = NULL;
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
    CHECK_FOR_FORK();
    CFRunLoopTimerRef memory;
    UInt32 size;
    size = sizeof(struct __CFRunLoopTimer) - sizeof(CFRuntimeBase);
    memory = (CFRunLoopTimerRef)_CFRuntimeCreateInstance(allocator, __kCFRunLoopTimerTypeID, size, NULL);
    if (NULL == memory) {
	return NULL;
    }
    __CFSetValid(memory);
    __CFRunLoopTimerUnsetFiring(memory);
    __CFRunLoopTimerUnsetDidFire(memory);
    CF_SPINLOCK_INIT_FOR_STRUCTS(memory->_lock);
    memory->_runLoop = NULL;
    memory->_rlModes = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    memory->_port = MACH_PORT_NULL;
#endif
    memory->_order = order;
    if (interval < 0.0) interval = 0.0;
    memory->_interval = interval;
    if (TIMER_DATE_LIMIT < fireDate) fireDate = TIMER_DATE_LIMIT;
    if (TIMER_INTERVAL_LIMIT < interval) interval = TIMER_INTERVAL_LIMIT;
    memory->_nextFireDate = fireDate;
    int64_t now2 = (int64_t)mach_absolute_time();
    CFAbsoluteTime now1 = CFAbsoluteTimeGetCurrent();
    if (fireDate < now1) {
	memory->_fireTSR = now2;
    } else if (now1 + __CFTSRToTimeInterval(LLONG_MAX) < fireDate) {
	memory->_fireTSR = LLONG_MAX;
    } else {
	memory->_fireTSR = now2 + __CFTimeIntervalToTSR(fireDate - now1);
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
//CFLog(4, CFSTR("CFRunLoopTimerCreate(%p, %f, %f, 0x%lx, %ld, %p, %p) => %p"), allocator, fireDate, interval, flags, order, callout, context, memory);
    return memory;
}

CFAbsoluteTime CFRunLoopTimerGetNextFireDate(CFRunLoopTimerRef rlt) {
    CHECK_FOR_FORK();
    CF_OBJC_FUNCDISPATCH0(__kCFRunLoopTimerTypeID, CFAbsoluteTime, rlt, "_cffireTime");
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    int64_t fireTime, result = 0;
    __CFRunLoopTimerFireTSRLock();
    fireTime = rlt->_fireTSR;
    __CFRunLoopTimerFireTSRUnlock();
    __CFRunLoopTimerLock(rlt);
    if (__CFIsValid(rlt)) {
	result = fireTime;
    }
    __CFRunLoopTimerUnlock(rlt);
    int64_t now2 = (int64_t)mach_absolute_time();
    CFAbsoluteTime now1 = CFAbsoluteTimeGetCurrent();
    return (0 == result) ? 0.0 : now1 + __CFTSRToTimeInterval(result - now2);
}

void CFRunLoopTimerSetNextFireDate(CFRunLoopTimerRef rlt, CFAbsoluteTime fireDate) {
//CFLog(6, CFSTR("CFRunLoopTimerSetNextFireDate(rlt %p, fireDate %f)"), rlt, fireDate);
    CHECK_FOR_FORK();
//CFLog(4, CFSTR("CFRunLoopTimerSetNextFireDate(%p, %f)  [limit: %f]"), rlt, fireDate, TIMER_DATE_LIMIT);
    if (TIMER_DATE_LIMIT < fireDate) fireDate = TIMER_DATE_LIMIT;
    rlt->_nextFireDate = fireDate;
    int64_t now2 = (int64_t)mach_absolute_time();
    CFAbsoluteTime now1 = CFAbsoluteTimeGetCurrent();
    __CFRunLoopTimerFireTSRLock();
    if (fireDate < now1) {
	rlt->_fireTSR = now2;
    } else if (now1 + __CFTSRToTimeInterval(LLONG_MAX) < fireDate) {
	rlt->_fireTSR = LLONG_MAX;
    } else {
	rlt->_fireTSR = now2 + __CFTimeIntervalToTSR(fireDate - now1);
    }
    if (rlt->_runLoop != NULL) {
	__CFRunLoopTimerRescheduleWithAllModes(rlt);
    }
    __CFRunLoopTimerFireTSRUnlock();
}

CFTimeInterval CFRunLoopTimerGetInterval(CFRunLoopTimerRef rlt) {
    CHECK_FOR_FORK();
    CF_OBJC_FUNCDISPATCH0(__kCFRunLoopTimerTypeID, CFTimeInterval, rlt, "timeInterval");
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    return rlt->_interval;
}

Boolean CFRunLoopTimerDoesRepeat(CFRunLoopTimerRef rlt) {
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    return (0.0 < rlt->_interval);
}

CFIndex CFRunLoopTimerGetOrder(CFRunLoopTimerRef rlt) {
    CHECK_FOR_FORK();
    CF_OBJC_FUNCDISPATCH0(__kCFRunLoopTimerTypeID, CFIndex, rlt, "order");
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    return rlt->_order;
}

void CFRunLoopTimerInvalidate(CFRunLoopTimerRef rlt) {	/* DOES CALLOUT */
    CHECK_FOR_FORK();
//CFLog(4, CFSTR("CFRunLoopTimerInvalidate(%p)"), rlt);
    CF_OBJC_FUNCDISPATCH0(__kCFRunLoopTimerTypeID, void, rlt, "invalidate");
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    if (!__CFRunLoopTimerIsDeallocating(rlt)) {
        CFRetain(rlt);
    }
    __CFRunLoopTimerLock(rlt);
    if (__CFIsValid(rlt)) {
	CFRunLoopRef rl = rlt->_runLoop;
	void *info = rlt->_context.info;
	__CFUnsetValid(rlt);
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	__CFRunLoopTimerPortMapLock();
	if (NULL != __CFRLTPortMap) {
	    CFDictionaryRemoveValue(__CFRLTPortMap, (void *)(uintptr_t)rlt->_port);
	}
	__CFRunLoopTimerPortMapUnlock();
	mk_timer_destroy(rlt->_port);
	rlt->_port = MACH_PORT_NULL;
#endif
	rlt->_context.info = NULL;
	__CFRunLoopTimerUnlock(rlt);
	if (NULL != rl) {
	    CFIndex cnt = CFSetGetCount(rlt->_rlModes);
	    STACK_BUFFER_DECL(CFStringRef, modes, cnt);
	    CFSetGetValues(rlt->_rlModes, (const void **)modes);
	    for (CFIndex idx = 0; idx < cnt; idx++) {
		CFRunLoopRemoveTimer(rl, rlt, modes[idx]);
	    }
	    CFRunLoopRemoveTimer(rl, rlt, kCFRunLoopCommonModes);
	}
	if (NULL != rlt->_context.release) {
	    rlt->_context.release(info);	/* CALLOUT */
	}
    } else {
	__CFRunLoopTimerUnlock(rlt);
    }
    if (!__CFRunLoopTimerIsDeallocating(rlt)) {
        CFRelease(rlt);
    }
}

Boolean CFRunLoopTimerIsValid(CFRunLoopTimerRef rlt) {
    CHECK_FOR_FORK();
    CF_OBJC_FUNCDISPATCH0(__kCFRunLoopTimerTypeID, Boolean, rlt, "isValid");
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    return __CFIsValid(rlt);
}

void CFRunLoopTimerGetContext(CFRunLoopTimerRef rlt, CFRunLoopTimerContext *context) {
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlt, __kCFRunLoopTimerTypeID);
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    *context = rlt->_context;
}


#if DEPLOYMENT_TARGET_WINDOWS

//#if MARRY_MESSAGE_QUEUE

// Do we need to make sure it's not CFRunLoopRun processing the message queue? 
CF_EXPORT LRESULT CALLBACK pumpRunLoopFromMessageQueue(int nCode, WPARAM wParam, LPARAM lParam) { 
    //if nCode < 0, we're supposed to blindly call CallNexthookEx instead of processing the message ourselves
    if (nCode >= 0) {
        MSG *msgPtr = (MSG *)lParam;
        CFRunLoopRef rl = CFRunLoopGetCurrent();
        CFStringRef currMode;
        
        if (msgPtr->message == __kCFRunLoopV1SourceReadyMessage) {
            CFRunLoopModeRef rlm;
            CFRunLoopSourceRef rls;
            __CFRunLoopLock(rl);
            rlm = rl->_currentMode;
            if (!rlm) {
                rlm = __CFRunLoopFindMode(rl, kCFRunLoopDefaultMode, true); // returns the mode locked
            } else {
                __CFRunLoopModeLock(rlm);
            }
            rls = __CFRunLoopModeFindSourceForMachPort(rl, rlm, (__CFPort)msgPtr->lParam);
            __CFRunLoopUnlock(rl);
            if (rls) {
                // Must still hold the mode lock when calling DoSource1
                __CFRunLoopDoSource1(rl, rlm, rls);
            }
            __CFRunLoopModeUnlock(rlm);
        }
        if (msgPtr->message == __kCFRunLoopV1SourceReadyMessage || msgPtr->message == __kCFRunLoopWakeUpMessage) {
            // Must always pump the RL regardless of whether we received the wakeup message because whomever processed the v1 source may be assuming that the RL is running and will see further callouts.
            currMode = CFRunLoopCopyCurrentMode(rl);
            if (!currMode) {
                currMode = kCFRunLoopDefaultMode;
                CFRetain(currMode);
            }
            while (CFRunLoopRunInMode(currMode, 0.0, true) == kCFRunLoopRunHandledSource) {
                ;
            }
            CFRelease(currMode);
        }
    } 
    return CallNextHookEx(__CFGetThreadSpecificData_inline()->_messageHook, nCode, wParam, lParam); 
} 

void __CFRunLoopMsgWaitThread(void *theRL) {
    CFRunLoopRef rl = (CFRunLoopRef)theRL;
    Boolean allDone = FALSE;
    
    while (!allDone) {
        
        CFRunLoopModeRef rlm;
        __CFPortSet waitSet;
        DWORD waitResult = WAIT_TIMEOUT;
        DWORD qMask;
        HANDLE handleBuf[MAXIMUM_WAIT_OBJECTS];
        HANDLE *handles, *currHandle;
        uint32_t handleCount, handleIndex;
        Boolean freeHandles;
        
        __CFRunLoopLock(rl);
        rlm = rl->_currentMode;
        if (!rlm) {
            rlm = __CFRunLoopFindMode(rl, kCFRunLoopDefaultMode, true); // Returns a locked mode
        } else {
            __CFRunLoopModeLock(rlm);
        }
        CFRetain(rlm);
        waitSet = rlm->_portSet;
        
        // copy out the handles to be safe from other threads at work
        handles = __CFPortSetGetPorts(waitSet, handleBuf, MAXIMUM_WAIT_OBJECTS, &handleCount);
        freeHandles = (handles != handleBuf);
        // Replace the wakeup port with our own update port
        for (handleIndex = 0, currHandle = handles; handleIndex < handleCount; handleIndex ++, currHandle++) {
            if (*currHandle == rl->_wakeUpPort) {
                *currHandle = rl->_msgUpdatePort;
                break;
            }
        }
        
        // Not sure what to do with this....
        //        handles[handleCount] = rl->_msgShutdownPort;
        //        handleCount ++;
        
        qMask = rlm->_msgQMask;
        
        while (1) {
            Boolean shuttingDown = false;
            __CFRunLoopModeUnlock(rlm);
            __CFRunLoopUnlock(rl);
            
            // What do we do if there are more than MAXIMUM_WAIT_OBJECTS 
            //waitResult = MsgWaitForMultipleObjects(__CFMin(handleCount, MAXIMUM_WAIT_OBJECTS), handles, false, INFINITE, qMask);
            waitResult = WaitForMultipleObjects(__CFMin(handleCount, MAXIMUM_WAIT_OBJECTS), handles, false, INFINITE);
            // Need to poll here to see if we've been told to die; if so, we must not touch rl.  I think.
            __CFRunLoopLock(rl);
            __CFRunLoopModeLock(rlm);
            ResetEvent(rl->_msgUpdatePort);
            
            __CFPort livePort = CFPORT_NULL;
            if (waitResult == WAIT_FAILED) {
                DWORD error = GetLastError();
                if (error == ERROR_INVALID_HANDLE) {
                    // A handle got freed out from underneath us.  Force an update of our handle watch list.
                    livePort = rl->_msgUpdatePort;
                } else {
                    CFAssert2(true, __kCFLogAssertion, "%s(): error %d from MsgWaitForMultipleObjects\n", __PRETTY_FUNCTION__,error);
                }
            }
            if (waitResult >= WAIT_OBJECT_0 && waitResult < WAIT_OBJECT_0+handleCount) {
                // a handle was signaled
                livePort = handles[waitResult-WAIT_OBJECT_0];
            } else if (waitResult >= WAIT_ABANDONED_0 && waitResult < WAIT_ABANDONED_0+handleCount) {
                // an "abandoned mutex object"
                livePort = handles[waitResult-WAIT_ABANDONED_0];
            }
            if (freeHandles){
                CFAllocatorDeallocate(kCFAllocatorSystemDefault, handles);
            }
            
            if (livePort == rl->_msgUpdatePort) {
                break;
                //            } else if (livePort == rl->_msgShutdownPort) {
                //                allDone = true;
                //                break;
            } else {
                //                OutputDebugStr(L"Posting v1 source ready msg\n");
                ResetEvent(livePort);
                PostThreadMessageW(rl->_threadID, __kCFRunLoopV1SourceReadyMessage, NULL, (LPARAM)livePort);
            }
        }
        __CFRunLoopModeUnlock(rlm);
        CFRelease(rlm);
        __CFRunLoopUnlock(rl);
    }
}

// #endif

// Called while holding the run loop lock

//#if MARRY_MESSAGE_QUEUE

void __CFRunLoopUpdateMsgWaitMarryMsgQueue(CFRunLoopRef rl) {
    if (rl->_msgUpdatePort == CFPORT_NULL) {
        rl->_msgUpdatePort = __CFPortAllocate();
        if (CFPORT_NULL == rl->_msgUpdatePort) HALT;
        
        // Kick off the MsgWaitThread
        _beginthread(__CFRunLoopMsgWaitThread, 0, rl);
    }
    SetEvent(rl->_msgUpdatePort);
}
//#else
CF_INLINE void __CFRunLoopUpdateMsgWait(CFRunLoopRef rl) {}
//#endif
    
#endif
