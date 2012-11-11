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
/*	CFMachPort.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

/* 
   [The following dissertation was written mostly for the
   benefit of open source developers.]

   Writing a run loop source can be a tricky business, but
   for CFMachPort that part is relatively straightforward.
   Thus, it makes a good example for study.  Particularly
   interesting for examination is the process of caching
   the objects in a non-retaining cache, the invalidation
   and deallocation sequences, locking for thread-safety,
   and how the invalidation callback is used.

   CFMachPort is a version 1 CFRunLoopSource, implemented
   by a few functions.  See CFMachPortCreateRunLoopSource()
   for details on how the run loop source is setup.  Note
   how the source is kept track of by the CFMachPort, so
   that it can be returned again and again from that function.
   This helps not only reduce the amount of memory expended
   in run loop source objects, but eliminates redundant
   registrations with the run loop and the excess time and
   memory that would consume.  It also allows the CFMachPort
   to propogate its own invalidation to the run loop source
   representing it.

   CFMachPortCreateWithPort() is the funnel point for the
   creation of CFMachPort instances.  The cache is first
   probed to see if an instance with that port is already
   available, and return that.  The object is next allocated
   and mostly initialized, before it is registered for death
   notification.  This is because cleaning up the memory is
   simpler than trying to get rid of the registration if
   memory allocation later fails.  The new object must be at
   least partially initialized (into a harmless state) so
   that it can be safely invalidated/deallocated if something
   fails later in creation.  Any object allocated with
   _CFRuntimeCreateInstance() may only be disposed by using
   CFRelease() (never CFAllocatorDeallocate!) so the class
   deallocation function __CFMachPortDeallocate() must be
   able to handle that, and initializing the object to have
   NULL fields and whatnot makes that possible.  The creation
   eventually inserts the new object in the cache.

   A CFMachPort may be explicitly invalidated, autoinvalidated
   due to the death of the port (that process is not discussed
   further here), or invalidated as part of the deallocation
   process when the last reference is released.  For
   convenience, in all cases this is done through
   CFMachPortInvalidate().  To prevent the CFMachPort from
   being freed in mid-function due to the callouts, the object
   is retained at the beginning of the function.  But if this
   invalidation is due to the object being deallocated, a
   retain and then release at the end of the function would
   cause a recursive call to __CFMachPortDeallocate().  The
   retain protection should be immaterial though at that stage.
   Invalidation also removes the object from the cache; though
   the object itself is not yet destroyed, invalidation makes
   it "useless".
   
   The best way to learn about the locking is to look through
   the code -- it's fairly straightforward.  The one thing
   worth calling attention to is how locks must be unlocked
   before invoking any user-defined callout function, and
   usually retaken after it returns.  This supports reentrancy
   (which is distinct from thread-safety).

   The invalidation callback, if one has been set, is called
   at invalidation time, but before the object has been torn
   down so that the port and other properties may be retrieved
   from the object in the callback.  Note that if the callback
   is attempted to be set after the CFMachPort is invalid,
   the function is simply called.  This helps with certain
   race conditions where the invalidation notification might
   be lost.  Only the owner/creator of a CFMachPort should
   really be setting the invalidation callback.

   Now, the CFMachPort is not retained/released around all
   callouts, but the callout may release the last reference.
   Also, sometimes it is friendly to retain/release the
   user-defined "info" around callouts, so that clients
   don't have to worry about that.  These may be some things
   to think about in the future, but is usually overkill.

   In general, with higher level functionalities in the system,
   it isn't even possible for a process to fork() and the child
   not exec(), but continue running, since the higher levels
   have done one-time initializations that aren't going to
   happen again.

	- Chris Kane

*/

#include <CoreFoundation/CFMachPort.h>
#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFByteOrder.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/notify.h>
#include <unistd.h>
#include "CFInternal.h"
#include <dlfcn.h>

static CFSpinLock_t __CFAllMachPortsLock = CFSpinLockInit;
static CFMutableDictionaryRef __CFAllMachPorts = NULL;
static mach_port_t __CFNotifyRawMachPort = MACH_PORT_NULL;
static CFMachPortRef __CFNotifyMachPort = NULL;

struct __CFMachPort {
    CFRuntimeBase _base;
    CFSpinLock_t _lock;
    mach_port_t _port;			/* immutable; invalidated */
    mach_port_t _oldnotify;		/* immutable; invalidated */
    CFRunLoopSourceRef _source;		/* immutable, once created; invalidated */
    CFMachPortInvalidationCallBack _icallout;
    CFMachPortCallBack _callout;	/* immutable */
    CFMachPortContext _context;		/* immutable; invalidated */
};

/* Bit 0 in the base reserved bits is used for invalid state */
/* Bit 1 in the base reserved bits is used for has-receive-ref state */
/* Bit 2 in the base reserved bits is used for has-send-ref state */
/* Bit 3 in the base reserved bits is used for is-deallocing state */

CF_INLINE Boolean __CFMachPortIsValid(CFMachPortRef mp) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)mp)->_cfinfo[CF_INFO_BITS], 0, 0);
}

CF_INLINE void __CFMachPortSetValid(CFMachPortRef mp) {
    __CFBitfieldSetValue(((CFRuntimeBase *)mp)->_cfinfo[CF_INFO_BITS], 0, 0, 1);
}

CF_INLINE void __CFMachPortUnsetValid(CFMachPortRef mp) {
    __CFBitfieldSetValue(((CFRuntimeBase *)mp)->_cfinfo[CF_INFO_BITS], 0, 0, 0);
}

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

CF_INLINE Boolean __CFMachPortIsDeallocing(CFMachPortRef mp) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)mp)->_cfinfo[CF_INFO_BITS], 3, 3);
}

CF_INLINE void __CFMachPortSetIsDeallocing(CFMachPortRef mp) {
    __CFBitfieldSetValue(((CFRuntimeBase *)mp)->_cfinfo[CF_INFO_BITS], 3, 3, 1);
}

CF_INLINE void __CFMachPortLock(CFMachPortRef mp) {
    __CFSpinLock(&(mp->_lock));
}

CF_INLINE void __CFMachPortUnlock(CFMachPortRef mp) {
    __CFSpinUnlock(&(mp->_lock));
}

void _CFMachPortInstallNotifyPort(CFRunLoopRef rl, CFStringRef mode) {
    CFRunLoopSourceRef source;
    if (NULL == __CFNotifyMachPort) return;
    source = CFMachPortCreateRunLoopSource(kCFAllocatorSystemDefault, __CFNotifyMachPort, -1000);
    CFRunLoopAddSource(rl, source, mode);
    CFRelease(source);
}

static void __CFNotifyDeadMachPort(CFMachPortRef port, void *msg, CFIndex size, void *info) {
    mach_msg_header_t *header = (mach_msg_header_t *)msg;
    mach_port_t dead_port = MACH_PORT_NULL;
    if (header && header->msgh_id == MACH_NOTIFY_DEAD_NAME) {
	dead_port = ((mach_dead_name_notification_t *)msg)->not_port;
	if (((mach_dead_name_notification_t *)msg)->NDR.int_rep != NDR_record.int_rep) {
	    dead_port = CFSwapInt32(dead_port);	
	}
    } else if (header && header->msgh_id == MACH_NOTIFY_PORT_DELETED) {
	dead_port = ((mach_port_deleted_notification_t *)msg)->not_port;
	if (((mach_port_deleted_notification_t *)msg)->NDR.int_rep != NDR_record.int_rep) {
	    dead_port = CFSwapInt32(dead_port);	
	}
    } else {
	return;
    }

    CFMachPortRef existing;
    /* If the CFMachPort has already been invalidated, it won't be found here. */
    __CFSpinLock(&__CFAllMachPortsLock);
    if (NULL != __CFAllMachPorts && CFDictionaryGetValueIfPresent(__CFAllMachPorts, (void *)(uintptr_t)dead_port, (const void **)&existing)) {
	CFDictionaryRemoveValue(__CFAllMachPorts, (void *)(uintptr_t)dead_port);
	CFRetain(existing);
	__CFSpinUnlock(&__CFAllMachPortsLock);
	__CFMachPortLock(existing);
	mach_port_t old_port = existing->_oldnotify;
	existing->_oldnotify = MACH_PORT_NULL;
	__CFMachPortUnlock(existing);
	if (MACH_PORT_NULL != old_port) {
	    header->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
	    header->msgh_local_port = MACH_PORT_NULL;
	    header->msgh_remote_port = old_port;
	    mach_msg(header, MACH_SEND_MSG, header->msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	}
	CFMachPortInvalidate(existing);
	CFRelease(existing);
    } else {
	__CFSpinUnlock(&__CFAllMachPortsLock);
    }

    if (header && header->msgh_id == MACH_NOTIFY_DEAD_NAME) {
	/* Delete port reference we got for this notification */
	mach_port_deallocate(mach_task_self(), dead_port);
    }
}

static Boolean __CFMachPortEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFMachPortRef mp1 = (CFMachPortRef)cf1;
    CFMachPortRef mp2 = (CFMachPortRef)cf2;
    return (mp1->_port == mp2->_port);
}

static CFHashCode __CFMachPortHash(CFTypeRef cf) {
    CHECK_FOR_FORK();
    CFMachPortRef mp = (CFMachPortRef)cf;
    return (CFHashCode)mp->_port;
}

static CFStringRef __CFMachPortCopyDescription(CFTypeRef cf) {
    CFMachPortRef mp = (CFMachPortRef)cf;
    CFStringRef result;
    const char *locked;
    CFStringRef contextDesc = NULL;
    locked = mp->_lock ? "Yes" : "No";
    if (NULL != mp->_context.info && NULL != mp->_context.copyDescription) {
	contextDesc = mp->_context.copyDescription(mp->_context.info);
    }
    if (NULL == contextDesc) {
	contextDesc = CFStringCreateWithFormat(CFGetAllocator(mp), NULL, CFSTR("<CFMachPort context %p>"), mp->_context.info);
    }
    void *addr = mp->_callout;
    Dl_info info;
    const char *name = (dladdr(addr, &info) && info.dli_saddr == addr && info.dli_sname) ? info.dli_sname : "???";
    result = CFStringCreateWithFormat(CFGetAllocator(mp), NULL, CFSTR("<CFMachPort %p [%p]>{locked = %s, valid = %s, port = %p, source = %p, callout = %s (%p), context = %@}"), cf, CFGetAllocator(mp), locked, (__CFMachPortIsValid(mp) ? "Yes" : "No"), mp->_port, mp->_source, name, addr, (NULL != contextDesc ? contextDesc : CFSTR("<no description>")));
    if (NULL != contextDesc) {
	CFRelease(contextDesc);
    }
    return result;
}

static void __CFMachPortDeallocate(CFTypeRef cf) {
    CHECK_FOR_FORK();
    CFMachPortRef mp = (CFMachPortRef)cf;
    __CFMachPortSetIsDeallocing(mp);
    CFMachPortInvalidate(mp);
    // MUST deallocate the send right FIRST if necessary,
    // then the receive right if necessary.  Don't ask me why;
    // if it's done in the other order the port will leak.
    if (__CFMachPortHasSend(mp)) {
	mach_port_mod_refs(mach_task_self(), mp->_port, MACH_PORT_RIGHT_SEND, -1);
    }
    if (__CFMachPortHasReceive(mp)) {
	mach_port_mod_refs(mach_task_self(), mp->_port, MACH_PORT_RIGHT_RECEIVE, -1);
    }
}

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

CFMachPortRef CFMachPortCreate(CFAllocatorRef allocator, CFMachPortCallBack callout, CFMachPortContext *context, Boolean *shouldFreeInfo) {
    CFMachPortRef result;
    mach_port_t port;
    kern_return_t ret;
    if (shouldFreeInfo) *shouldFreeInfo = true;
    ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (KERN_SUCCESS != ret) {
	return NULL;
    }
    ret = mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    if (KERN_SUCCESS != ret) {
	mach_port_destroy(mach_task_self(), port);
	return NULL;
    }
    result = CFMachPortCreateWithPort(allocator, port, callout, context, shouldFreeInfo);
    if (NULL != result) {
	__CFMachPortSetHasReceive(result);
	__CFMachPortSetHasSend(result);
    }
    return result;
}

/* Note: any receive or send rights that the port contains coming in will
 * not be cleaned up by CFMachPort; it will increment and decrement
 * references on the port if the kernel ever allows that in the future,
 * but will not cleanup any references you got when you got the port. */
CFMachPortRef CFMachPortCreateWithPort(CFAllocatorRef allocator, mach_port_t port, CFMachPortCallBack callout, CFMachPortContext *context, Boolean *shouldFreeInfo) {
    CHECK_FOR_FORK();
    CFMachPortRef memory;
    SInt32 size;
    Boolean didCreateNotifyPort = false;
    CFRunLoopSourceRef source;
    if (shouldFreeInfo) *shouldFreeInfo = true;
    __CFSpinLock(&__CFAllMachPortsLock);
    if (NULL != __CFAllMachPorts && CFDictionaryGetValueIfPresent(__CFAllMachPorts, (void *)(uintptr_t)port, (const void **)&memory)) {
	CFRetain(memory);
	__CFSpinUnlock(&__CFAllMachPortsLock);
	return (CFMachPortRef)(memory);
    }
    size = sizeof(struct __CFMachPort) - sizeof(CFRuntimeBase);
    memory = (CFMachPortRef)_CFRuntimeCreateInstance(allocator, __kCFMachPortTypeID, size, NULL);
    if (NULL == memory) {
	__CFSpinUnlock(&__CFAllMachPortsLock);
	return NULL;
    }
    __CFMachPortUnsetValid(memory);
    memory->_lock = CFSpinLockInit;
    memory->_port = port;
    memory->_source = NULL;
    memory->_icallout = NULL;
    memory->_context.info = NULL;
    memory->_context.retain = NULL;
    memory->_context.release = NULL;
    memory->_context.copyDescription = NULL;
    if (MACH_PORT_NULL == __CFNotifyRawMachPort) {
	kern_return_t ret;
	ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &__CFNotifyRawMachPort);
	if (KERN_SUCCESS != ret) {
	    __CFSpinUnlock(&__CFAllMachPortsLock);
	    CFRelease(memory);
	    return NULL;
	}
	didCreateNotifyPort = true;
    }
    // Do not register for notifications on the notify port
    if (MACH_PORT_NULL != __CFNotifyRawMachPort && port != __CFNotifyRawMachPort) {
	mach_port_t old_port;
	kern_return_t ret;
	old_port = MACH_PORT_NULL;
	ret = mach_port_request_notification(mach_task_self(), port, MACH_NOTIFY_DEAD_NAME, 0, __CFNotifyRawMachPort, MACH_MSG_TYPE_MAKE_SEND_ONCE, &old_port);
	if (ret != KERN_SUCCESS) {
	    __CFSpinUnlock(&__CFAllMachPortsLock);
	    CFRelease(memory);
	    return NULL;
	}
	memory->_oldnotify = old_port;
    }
    __CFMachPortSetValid(memory);
    memory->_callout = callout;
    if (NULL != context) {
	CF_WRITE_BARRIER_MEMMOVE(&memory->_context, context, sizeof(CFMachPortContext));
	memory->_context.info = context->retain ? (void *)context->retain(context->info) : context->info;
    }
    if (NULL == __CFAllMachPorts) {
	__CFAllMachPorts = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, NULL);
	_CFDictionarySetCapacity(__CFAllMachPorts, 20);
    }
    CFDictionaryAddValue(__CFAllMachPorts, (void *)(uintptr_t)port, memory);
    __CFSpinUnlock(&__CFAllMachPortsLock);
    if (didCreateNotifyPort) {
	// __CFNotifyMachPort ends up in cache
	CFMachPortRef mp = CFMachPortCreateWithPort(kCFAllocatorSystemDefault, __CFNotifyRawMachPort, __CFNotifyDeadMachPort, NULL, NULL);
	__CFMachPortSetHasReceive(mp);
	__CFNotifyMachPort = mp;
    }
    if (NULL != __CFNotifyMachPort) {
	// We do this so that it gets into each thread's run loop, since
	// we don't know which run loop is the main thread's, and that's
	// not necessarily the "right" one anyway.  This won't happen for
	// the call which creates the __CFNotifyMachPort itself, but that's
	// OK since it will happen in the invocation of this function
	// from which that call was triggered.
	source = CFMachPortCreateRunLoopSource(kCFAllocatorSystemDefault, __CFNotifyMachPort, -1000);
	CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
	CFRelease(source);
    }
    if (shouldFreeInfo) *shouldFreeInfo = false;
    return memory;
}

mach_port_t CFMachPortGetPort(CFMachPortRef mp) {
    CHECK_FOR_FORK();
    CF_OBJC_FUNCDISPATCH0(__kCFMachPortTypeID, mach_port_t, mp, "machPort");
    __CFGenericValidateType(mp, __kCFMachPortTypeID);
    return mp->_port;
}

void CFMachPortGetContext(CFMachPortRef mp, CFMachPortContext *context) {
    __CFGenericValidateType(mp, __kCFMachPortTypeID);
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    CF_WRITE_BARRIER_MEMMOVE(context, &mp->_context, sizeof(CFMachPortContext));
}

void CFMachPortInvalidate(CFMachPortRef mp) {
    CHECK_FOR_FORK();
    CF_OBJC_FUNCDISPATCH0(__kCFMachPortTypeID, void, mp, "invalidate");
    __CFGenericValidateType(mp, __kCFMachPortTypeID);
    if (!__CFMachPortIsDeallocing(mp)) {
	CFRetain(mp);
    }
    __CFSpinLock(&__CFAllMachPortsLock);
    if (NULL != __CFAllMachPorts) {
	CFDictionaryRemoveValue(__CFAllMachPorts, (void *)(uintptr_t)(mp->_port));
    }
    __CFSpinUnlock(&__CFAllMachPortsLock);
    __CFMachPortLock(mp);
    if (__CFMachPortIsValid(mp)) {
	CFRunLoopSourceRef source;
	void *info;
	mach_port_t old_port = mp->_oldnotify;
	CFMachPortInvalidationCallBack callout = mp->_icallout;
	__CFMachPortUnsetValid(mp);
	__CFMachPortUnlock(mp);
	if (NULL != callout) {
	    callout(mp, mp->_context.info);
	}
	__CFMachPortLock(mp);
	// For hashing and equality purposes, cannot get rid of _port here
	source = mp->_source;
	mp->_source = NULL;
	info = mp->_context.info;
	mp->_context.info = NULL;
	__CFMachPortUnlock(mp);
	if (NULL != mp->_context.release) {
	    mp->_context.release(info);
	}
	if (NULL != source) {
	    CFRunLoopSourceInvalidate(source);
	    CFRelease(source);
	}
	if (MACH_PORT_NULL != old_port) {
	    mach_port_deallocate(mach_task_self(), old_port);
	}
    } else {
	__CFMachPortUnlock(mp);
    }
    if (!__CFMachPortIsDeallocing(mp)) {
	CFRelease(mp);
    }
}

Boolean CFMachPortIsValid(CFMachPortRef mp) {
    CF_OBJC_FUNCDISPATCH0(__kCFMachPortTypeID, Boolean, mp, "isValid");
    __CFGenericValidateType(mp, __kCFMachPortTypeID);
    return __CFMachPortIsValid(mp);
}

CFMachPortInvalidationCallBack CFMachPortGetInvalidationCallBack(CFMachPortRef mp) {
    __CFGenericValidateType(mp, __kCFMachPortTypeID);
    return mp->_icallout;
}

void CFMachPortSetInvalidationCallBack(CFMachPortRef mp, CFMachPortInvalidationCallBack callout) {
    __CFGenericValidateType(mp, __kCFMachPortTypeID);
    if (!__CFMachPortIsValid(mp) && NULL != callout) {
	callout(mp, mp->_context.info);
    } else {
	mp->_icallout = callout;
    }
}

/* Returns the number of messages queued for a receive port.  */
CFIndex CFMachPortGetQueuedMessageCount(CFMachPortRef mp) {  
    CHECK_FOR_FORK();
    mach_port_status_t status;
    mach_msg_type_number_t num = MACH_PORT_RECEIVE_STATUS_COUNT;
    kern_return_t ret;
    ret = mach_port_get_attributes(mach_task_self(), mp->_port, MACH_PORT_RECEIVE_STATUS, (mach_port_info_t)&status, &num);
    return (KERN_SUCCESS != ret) ? 0 : status.mps_msgcount;
}

static mach_port_t __CFMachPortGetPort(void *info) {
    CFMachPortRef mp = info;
    return mp->_port;
}

static void *__CFMachPortPerform(void *msg, CFIndex size, CFAllocatorRef allocator, void *info) {
    CHECK_FOR_FORK();
    CFMachPortRef mp = info;
    void *context_info;
    void (*context_release)(const void *);
    __CFMachPortLock(mp);
    if (!__CFMachPortIsValid(mp)) {
	__CFMachPortUnlock(mp);
	return NULL;
    }
    if (NULL != mp->_context.retain) {
	context_info = (void *)mp->_context.retain(mp->_context.info);
	context_release = mp->_context.release;
    } else {
	context_info = mp->_context.info;
	context_release = NULL;
    }
    __CFMachPortUnlock(mp);
    mp->_callout(mp, msg, size, mp->_context.info);
    CHECK_FOR_FORK();
    if (context_release) {
	context_release(context_info);
    }
    return NULL;
}

CFRunLoopSourceRef CFMachPortCreateRunLoopSource(CFAllocatorRef allocator, CFMachPortRef mp, CFIndex order) {
    CHECK_FOR_FORK();
    CFRunLoopSourceRef result = NULL;
    __CFGenericValidateType(mp, __kCFMachPortTypeID);
    __CFMachPortLock(mp);
    if (!__CFMachPortIsValid(mp)) {
        __CFMachPortUnlock(mp);
        return NULL;
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
    if (NULL != mp->_source) {
	result = (CFRunLoopSourceRef)CFRetain(mp->_source);
    }
    __CFMachPortUnlock(mp);
    return result;
}


