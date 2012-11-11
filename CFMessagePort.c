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
/*	CFMessagePort.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFMessagePort.h>
#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFMachPort.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFByteOrder.h>
#include <limits.h>
#include "CFInternal.h"
#include <mach/mach.h>
#include <mach/message.h>
#include <mach/mach_error.h>
#include <bootstrap_priv.h>
#include <math.h>
#include <mach/mach_time.h>
#include <dlfcn.h>


#define __kCFMessagePortMaxNameLengthMax 255

#if defined(BOOTSTRAP_MAX_NAME_LEN)
    #define __kCFMessagePortMaxNameLength BOOTSTRAP_MAX_NAME_LEN
#else
    #define __kCFMessagePortMaxNameLength 128
#endif

#if __kCFMessagePortMaxNameLengthMax < __kCFMessagePortMaxNameLength
    #undef __kCFMessagePortMaxNameLength
    #define __kCFMessagePortMaxNameLength __kCFMessagePortMaxNameLengthMax
#endif

static CFSpinLock_t __CFAllMessagePortsLock = CFSpinLockInit;
static CFMutableDictionaryRef __CFAllLocalMessagePorts = NULL;
static CFMutableDictionaryRef __CFAllRemoteMessagePorts = NULL;

struct __CFMessagePort {
    CFRuntimeBase _base;
    CFSpinLock_t _lock;
    CFStringRef _name;
    CFMachPortRef _port;		/* immutable; invalidated */
    CFMutableDictionaryRef _replies;
    int32_t _convCounter;
    int32_t _perPID;			/* zero if not per-pid, else pid */
    CFMachPortRef _replyPort;		/* only used by remote port; immutable once created; invalidated */
    CFRunLoopSourceRef _source;		/* only used by local port; immutable once created; invalidated */
    CFMessagePortInvalidationCallBack _icallout;
    CFMessagePortCallBack _callout;	/* only used by local port; immutable */
    CFMessagePortContext _context;	/* not part of remote port; immutable; invalidated */
};

/* Bit 0 in the base reserved bits is used for invalid state */
/* Bit 1 of the base reserved bits is used for has-extra-port-refs state */
/* Bit 2 of the base reserved bits is used for is-remote state */
/* Bit 3 in the base reserved bits is used for is-deallocing state */

CF_INLINE Boolean __CFMessagePortIsValid(CFMessagePortRef ms) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)ms)->_cfinfo[CF_INFO_BITS], 0, 0);
}

CF_INLINE void __CFMessagePortSetValid(CFMessagePortRef ms) {
    __CFBitfieldSetValue(((CFRuntimeBase *)ms)->_cfinfo[CF_INFO_BITS], 0, 0, 1);
}

CF_INLINE void __CFMessagePortUnsetValid(CFMessagePortRef ms) {
    __CFBitfieldSetValue(((CFRuntimeBase *)ms)->_cfinfo[CF_INFO_BITS], 0, 0, 0);
}

CF_INLINE Boolean __CFMessagePortExtraMachRef(CFMessagePortRef ms) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)ms)->_cfinfo[CF_INFO_BITS], 1, 1);
}

CF_INLINE void __CFMessagePortSetExtraMachRef(CFMessagePortRef ms) {
    __CFBitfieldSetValue(((CFRuntimeBase *)ms)->_cfinfo[CF_INFO_BITS], 1, 1, 1);
}

CF_INLINE void __CFMessagePortUnsetExtraMachRef(CFMessagePortRef ms) {
    __CFBitfieldSetValue(((CFRuntimeBase *)ms)->_cfinfo[CF_INFO_BITS], 1, 1, 0);
}

CF_INLINE Boolean __CFMessagePortIsRemote(CFMessagePortRef ms) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)ms)->_cfinfo[CF_INFO_BITS], 2, 2);
}

CF_INLINE void __CFMessagePortSetRemote(CFMessagePortRef ms) {
    __CFBitfieldSetValue(((CFRuntimeBase *)ms)->_cfinfo[CF_INFO_BITS], 2, 2, 1);
}

CF_INLINE void __CFMessagePortUnsetRemote(CFMessagePortRef ms) {
    __CFBitfieldSetValue(((CFRuntimeBase *)ms)->_cfinfo[CF_INFO_BITS], 2, 2, 0);
}

CF_INLINE Boolean __CFMessagePortIsDeallocing(CFMessagePortRef ms) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)ms)->_cfinfo[CF_INFO_BITS], 3, 3);
}

CF_INLINE void __CFMessagePortSetIsDeallocing(CFMessagePortRef ms) {
    __CFBitfieldSetValue(((CFRuntimeBase *)ms)->_cfinfo[CF_INFO_BITS], 3, 3, 1);
}

CF_INLINE void __CFMessagePortLock(CFMessagePortRef ms) {
    __CFSpinLock(&(ms->_lock));
}

CF_INLINE void __CFMessagePortUnlock(CFMessagePortRef ms) {
    __CFSpinUnlock(&(ms->_lock));
}

// Just a heuristic
#define __CFMessagePortMaxInlineBytes 4096*10

struct __CFMessagePortMachMsg0 {
    int32_t msgid;
    int32_t byteslen;
    uint8_t bytes[__CFMessagePortMaxInlineBytes];
};

struct __CFMessagePortMachMsg1 {
    mach_msg_descriptor_t desc;
    int32_t msgid;
};

struct __CFMessagePortMachMessage {
    mach_msg_header_t head;
    mach_msg_body_t body;
    union {
	struct __CFMessagePortMachMsg0 msg0;
	struct __CFMessagePortMachMsg1 msg1;
    } contents;
};

static struct __CFMessagePortMachMessage *__CFMessagePortCreateMessage(CFAllocatorRef allocator, bool reply, mach_port_t port, mach_port_t replyPort, int32_t convid, int32_t msgid, const uint8_t *bytes, int32_t byteslen) {
    struct __CFMessagePortMachMessage *msg;
    int32_t size = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t);
    if (byteslen < __CFMessagePortMaxInlineBytes) {
	size += 2 * sizeof(int32_t) + ((byteslen + 3) & ~0x3);
    } else {
	size += sizeof(struct __CFMessagePortMachMsg1);
    }
    msg = CFAllocatorAllocate(allocator, size, 0);
    msg->head.msgh_id = convid;
    msg->head.msgh_size = size;
    msg->head.msgh_remote_port = port;
    msg->head.msgh_local_port = replyPort;
    msg->head.msgh_reserved = 0;
//    msg->head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, (replyPort ? MACH_MSG_TYPE_MAKE_SEND : 0));
    msg->head.msgh_bits = MACH_MSGH_BITS((reply ? MACH_MSG_TYPE_MOVE_SEND_ONCE : MACH_MSG_TYPE_COPY_SEND), (MACH_PORT_NULL != replyPort ? MACH_MSG_TYPE_MAKE_SEND_ONCE : 0));
    if (byteslen < __CFMessagePortMaxInlineBytes) {
	msg->body.msgh_descriptor_count = 0;
	msg->contents.msg0.msgid = CFSwapInt32HostToLittle(msgid);
	msg->contents.msg0.byteslen = CFSwapInt32HostToLittle(byteslen);
	if (NULL != bytes && 0 < byteslen) {
	    memmove(msg->contents.msg0.bytes, bytes, byteslen);
	}
	memset(msg->contents.msg0.bytes + byteslen, 0, ((byteslen + 3) & ~0x3) - byteslen);
    } else {
	msg->head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
	msg->body.msgh_descriptor_count = 1;
	msg->contents.msg1.desc.out_of_line.deallocate = false;
	msg->contents.msg1.desc.out_of_line.copy = MACH_MSG_VIRTUAL_COPY;
	msg->contents.msg1.desc.out_of_line.address = (void *)bytes;
	msg->contents.msg1.desc.out_of_line.size = byteslen;
	msg->contents.msg1.desc.out_of_line.type = MACH_MSG_OOL_DESCRIPTOR;
	msg->contents.msg1.msgid = CFSwapInt32HostToLittle(msgid);
    }
    return msg;
}

static CFStringRef __CFMessagePortCopyDescription(CFTypeRef cf) {
    CFMessagePortRef ms = (CFMessagePortRef)cf;
    CFStringRef result;
    const char *locked;
    CFStringRef contextDesc = NULL;
    locked = ms->_lock ? "Yes" : "No";
    if (!__CFMessagePortIsRemote(ms)) {
	if (NULL != ms->_context.info && NULL != ms->_context.copyDescription) {
	    contextDesc = ms->_context.copyDescription(ms->_context.info);
	}
	if (NULL == contextDesc) {
	    contextDesc = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFMessagePort context %p>"), ms->_context.info);
	}
	result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFMessagePort %p [%p]>{locked = %s, valid = %s, remote = %s, name = %@}"), cf, CFGetAllocator(ms), locked, (__CFMessagePortIsValid(ms) ? "Yes" : "No"), (__CFMessagePortIsRemote(ms) ? "Yes" : "No"), ms->_name);
    } else {
	void *addr = ms->_callout;
	Dl_info info;
	const char *name = (dladdr(addr, &info) && info.dli_saddr == addr && info.dli_sname) ? info.dli_sname : "???";
	result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFMessagePort %p [%p]>{locked = %s, valid = %s, remote = %s, name = %@, source = %p, callout = %s (%p), context = %@}"), cf, CFGetAllocator(ms), locked, (__CFMessagePortIsValid(ms) ? "Yes" : "No"), (__CFMessagePortIsRemote(ms) ? "Yes" : "No"), ms->_name, ms->_source, name, addr, (NULL != contextDesc ? contextDesc : CFSTR("<no description>")));
    }
    if (NULL != contextDesc) {
	CFRelease(contextDesc);
    }
    return result;
}

static void __CFMessagePortDeallocate(CFTypeRef cf) {
    CFMessagePortRef ms = (CFMessagePortRef)cf;
    __CFMessagePortSetIsDeallocing(ms);
    CFMessagePortInvalidate(ms);
    // Delay cleanup of _replies until here so that invalidation during
    // SendRequest does not cause _replies to disappear out from under that function.
    if (NULL != ms->_replies) {
	CFRelease(ms->_replies);
    }
    if (NULL != ms->_name) {
	CFRelease(ms->_name);
    }
    if (NULL != ms->_port) {
	if (__CFMessagePortExtraMachRef(ms)) {
	    mach_port_mod_refs(mach_task_self(), CFMachPortGetPort(ms->_port), MACH_PORT_RIGHT_SEND, -1);
	    mach_port_mod_refs(mach_task_self(), CFMachPortGetPort(ms->_port), MACH_PORT_RIGHT_RECEIVE, -1);
	}
	CFMachPortInvalidate(ms->_port);
	CFRelease(ms->_port);
    }

    // A remote message port for a local message port in the same process will get the
    // same mach port, and the remote port will keep the mach port from being torn down,
    // thus keeping the remote port from getting any sort of death notification and
    // auto-invalidating; so we manually implement the 'auto-invalidation' here by
    // tickling each remote port to check its state after any message port is destroyed,
    // but most importantly after local message ports are destroyed.
    __CFSpinLock(&__CFAllMessagePortsLock);
    CFMessagePortRef *remotePorts = NULL;
    CFIndex cnt = 0;
    if (NULL != __CFAllRemoteMessagePorts) {
	cnt = CFDictionaryGetCount(__CFAllRemoteMessagePorts);
	remotePorts = CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(CFMessagePortRef), __kCFAllocatorGCScannedMemory);
	CFDictionaryGetKeysAndValues(__CFAllRemoteMessagePorts, NULL, (const void **)remotePorts);
	for (CFIndex idx = 0; idx < cnt; idx++) {
	    CFRetain(remotePorts[idx]);
	}
    }
    __CFSpinUnlock(&__CFAllMessagePortsLock);
    if (remotePorts) {
	for (CFIndex idx = 0; idx < cnt; idx++) {
	    // as a side-effect, this will auto-invalidate the CFMessagePort if the CFMachPort is invalid
	    CFMessagePortIsValid(remotePorts[idx]);
	    CFRelease(remotePorts[idx]);
	}
	CFAllocatorDeallocate(kCFAllocatorSystemDefault, remotePorts);
    }
}

static CFTypeID __kCFMessagePortTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFMessagePortClass = {
    0,
    "CFMessagePort",
    NULL,      // init
    NULL,      // copy
    __CFMessagePortDeallocate,
    NULL,
    NULL,
    NULL,      // 
    __CFMessagePortCopyDescription
};

__private_extern__ void __CFMessagePortInitialize(void) {
    __kCFMessagePortTypeID = _CFRuntimeRegisterClass(&__CFMessagePortClass);
}

CFTypeID CFMessagePortGetTypeID(void) {
    return __kCFMessagePortTypeID;
}

static CFStringRef __CFMessagePortSanitizeStringName(CFAllocatorRef allocator, CFStringRef name, uint8_t **utfnamep, CFIndex *utfnamelenp) {
    uint8_t *utfname;
    CFIndex utflen;
    CFStringRef result;
    utfname = CFAllocatorAllocate(allocator, __kCFMessagePortMaxNameLength + 1, 0);
    CFStringGetBytes(name, CFRangeMake(0, CFStringGetLength(name)), kCFStringEncodingUTF8, 0, false, utfname, __kCFMessagePortMaxNameLength, &utflen);
    utfname[utflen] = '\0';
    /* A new string is created, because the original string may have been
       truncated to the max length, and we want the string name to definitely
       match the raw UTF-8 chunk that has been created. Also, this is useful
       to get a constant string in case the original name string was mutable. */
    result = CFStringCreateWithBytes(allocator, utfname, utflen, kCFStringEncodingUTF8, false);
    if (NULL != utfnamep) {
	*utfnamep = utfname;
    } else {
	CFAllocatorDeallocate(allocator, utfname);
    }
    if (NULL != utfnamelenp) {
	*utfnamelenp = utflen;
    }
    return result;
}

static void __CFMessagePortDummyCallback(CFMachPortRef port, void *msg, CFIndex size, void *info) {
    // not supposed to be implemented
}

static void __CFMessagePortInvalidationCallBack(CFMachPortRef port, void *info) {
    // info has been setup as the CFMessagePort owning the CFMachPort
    CFMessagePortInvalidate(info);
}

static CFMessagePortRef __CFMessagePortCreateLocal(CFAllocatorRef allocator, CFStringRef name, CFMessagePortCallBack callout, CFMessagePortContext *context, Boolean *shouldFreeInfo, Boolean perPID) {
    CFMessagePortRef memory;
    uint8_t *utfname = NULL;

    if (shouldFreeInfo) *shouldFreeInfo = true;
    if (NULL != name) {
	name = __CFMessagePortSanitizeStringName(allocator, name, &utfname, NULL);
    }
    __CFSpinLock(&__CFAllMessagePortsLock);
    if (!perPID && NULL != name) {
	CFMessagePortRef existing;
	if (NULL != __CFAllLocalMessagePorts && CFDictionaryGetValueIfPresent(__CFAllLocalMessagePorts, name, (const void **)&existing)) {
	    CFRetain(existing);
	    __CFSpinUnlock(&__CFAllMessagePortsLock);
	    CFRelease(name);
	    CFAllocatorDeallocate(allocator, utfname);
	    return (CFMessagePortRef)(existing);
	}
    }
    __CFSpinUnlock(&__CFAllMessagePortsLock);
    CFIndex size = sizeof(struct __CFMessagePort) - sizeof(CFRuntimeBase);
    memory = (CFMessagePortRef)_CFRuntimeCreateInstance(allocator, __kCFMessagePortTypeID, size, NULL);
    if (NULL == memory) {
	if (NULL != name) {
	    CFRelease(name);
	}
	CFAllocatorDeallocate(allocator, utfname);
	return NULL;
    }
    __CFMessagePortUnsetValid(memory);
    __CFMessagePortUnsetExtraMachRef(memory);
    __CFMessagePortUnsetRemote(memory);
    memory->_lock = CFSpinLockInit;
    memory->_name = name;
    memory->_port = NULL;
    memory->_replies = NULL;
    memory->_convCounter = 0;
    memory->_perPID = perPID ? getpid() : 0;	// actual value not terribly useful for local ports
    memory->_replyPort = NULL;
    memory->_source = NULL;
    memory->_icallout = NULL;
    memory->_callout = callout;
    memory->_context.info = NULL;
    memory->_context.retain = NULL;
    memory->_context.release = NULL;
    memory->_context.copyDescription = NULL;

    if (NULL != name) {
	CFMachPortRef native = NULL;
	kern_return_t ret;
	mach_port_t bs, mp;
	task_get_bootstrap_port(mach_task_self(), &bs);
	if (!perPID) {
	    ret = bootstrap_check_in(bs, (char *)utfname, &mp); /* If we're started by launchd or the old mach_init */
	    if (ret == KERN_SUCCESS) {
		ret = mach_port_insert_right(mach_task_self(), mp, mp, MACH_MSG_TYPE_MAKE_SEND);
		if (KERN_SUCCESS == ret) {
		    CFMachPortContext ctx = {0, memory, NULL, NULL, NULL};
		    native = CFMachPortCreateWithPort(allocator, mp, __CFMessagePortDummyCallback, &ctx, NULL);
		    __CFMessagePortSetExtraMachRef(memory);
		} else {
		    CFLog(kCFLogLevelWarning, CFSTR("*** CFMessagePort: mach_port_insert_member() after bootstrap_check_in(): failed %d (0x%x) '%s', port = 0x%x, name = '%s'"), ret, ret, bootstrap_strerror(ret), mp, utfname);
		    mach_port_destroy(mach_task_self(), mp);
		    CFAllocatorDeallocate(allocator, utfname);
		    // name is released by deallocation
		    CFRelease(memory);
		    return NULL;
		}
	    }
	}
	if (!native) {
	    CFMachPortContext ctx = {0, memory, NULL, NULL, NULL};
	    native = CFMachPortCreate(allocator, __CFMessagePortDummyCallback, &ctx, NULL);
	    mp = CFMachPortGetPort(native);
	    ret = bootstrap_register2(bs, (char *)utfname, mp, perPID ? BOOTSTRAP_PER_PID_SERVICE : 0);
	    if (ret != KERN_SUCCESS) {
		CFLog(kCFLogLevelWarning, CFSTR("*** CFMessagePort: bootstrap_register(): failed %d (0x%x) '%s', port = 0x%x, name = '%s'\nSee /usr/include/servers/bootstrap_defs.h for the error codes."), ret, ret, bootstrap_strerror(ret), mp, utfname);
		CFMachPortInvalidate(native);
		CFRelease(native);
		CFAllocatorDeallocate(allocator, utfname);
		// name is released by deallocation
		CFRelease(memory);
		return NULL;
	    }
	}
	CFMachPortSetInvalidationCallBack(native, __CFMessagePortInvalidationCallBack);
	memory->_port = native;
    }

    CFAllocatorDeallocate(allocator, utfname);
    __CFMessagePortSetValid(memory);
    if (NULL != context) {
	memmove(&memory->_context, context, sizeof(CFMessagePortContext));
	memory->_context.info = context->retain ? (void *)context->retain(context->info) : context->info;
    }
    __CFSpinLock(&__CFAllMessagePortsLock);
    if (!perPID && NULL != name) {
        CFMessagePortRef existing;
        if (NULL != __CFAllLocalMessagePorts && CFDictionaryGetValueIfPresent(__CFAllLocalMessagePorts, name, (const void **)&existing)) {
            CFRetain(existing);
            __CFSpinUnlock(&__CFAllMessagePortsLock); 
	    CFRelease(memory);
            return (CFMessagePortRef)(existing);
        }       
	if (NULL == __CFAllLocalMessagePorts) {
	    __CFAllLocalMessagePorts = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeDictionaryKeyCallBacks, NULL);
	}
	CFDictionaryAddValue(__CFAllLocalMessagePorts, name, memory);
    }
    __CFSpinUnlock(&__CFAllMessagePortsLock);
    if (shouldFreeInfo) *shouldFreeInfo = false;
    return memory;
}

CFMessagePortRef CFMessagePortCreateLocal(CFAllocatorRef allocator, CFStringRef name, CFMessagePortCallBack callout, CFMessagePortContext *context, Boolean *shouldFreeInfo) {
    return __CFMessagePortCreateLocal(allocator, name, callout, context, shouldFreeInfo, false);
}

CFMessagePortRef CFMessagePortCreatePerProcessLocal(CFAllocatorRef allocator, CFStringRef name, CFMessagePortCallBack callout, CFMessagePortContext *context, Boolean *shouldFreeInfo) {
    return __CFMessagePortCreateLocal(allocator, name, callout, context, shouldFreeInfo, true);
}

static CFMessagePortRef __CFMessagePortCreateRemote(CFAllocatorRef allocator, CFStringRef name, Boolean perPID, CFIndex pid) {
    CFMessagePortRef memory;
    CFMachPortRef native;
    CFMachPortContext ctx;
    uint8_t *utfname = NULL;
    CFIndex size;
    mach_port_t bp, port;
    kern_return_t ret;

    name = __CFMessagePortSanitizeStringName(allocator, name, &utfname, NULL);
    if (NULL == name) {
	return NULL;
    }
    __CFSpinLock(&__CFAllMessagePortsLock);
    if (!perPID && NULL != name) {
	CFMessagePortRef existing;
	if (NULL != __CFAllRemoteMessagePorts && CFDictionaryGetValueIfPresent(__CFAllRemoteMessagePorts, name, (const void **)&existing)) {
	    CFRetain(existing);
	    __CFSpinUnlock(&__CFAllMessagePortsLock);
	    CFRelease(name);
	    CFAllocatorDeallocate(allocator, utfname);
	    return (CFMessagePortRef)(existing);
	}
    }
    __CFSpinUnlock(&__CFAllMessagePortsLock);
    size = sizeof(struct __CFMessagePort) - sizeof(CFMessagePortContext) - sizeof(CFRuntimeBase);
    memory = (CFMessagePortRef)_CFRuntimeCreateInstance(allocator, __kCFMessagePortTypeID, size, NULL);
    if (NULL == memory) {
	if (NULL != name) {
	    CFRelease(name);
	}
	CFAllocatorDeallocate(allocator, utfname);
	return NULL;
    }
    __CFMessagePortUnsetValid(memory);
    __CFMessagePortUnsetExtraMachRef(memory);
    __CFMessagePortSetRemote(memory);
    memory->_lock = CFSpinLockInit;
    memory->_name = name;
    memory->_port = NULL;
    memory->_replies = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, NULL);
    memory->_convCounter = 0;
    memory->_perPID = perPID ? pid : 0;
    memory->_replyPort = NULL;
    memory->_source = NULL;
    memory->_icallout = NULL;
    memory->_callout = NULL;
    ctx.version = 0;
    ctx.info = memory;
    ctx.retain = NULL;
    ctx.release = NULL;
    ctx.copyDescription = NULL;
    task_get_bootstrap_port(mach_task_self(), &bp);
    ret = bootstrap_look_up2(bp, (char *)utfname, &port, perPID ? (pid_t)pid : 0, perPID ? BOOTSTRAP_PER_PID_SERVICE : 0);
    native = (KERN_SUCCESS == ret) ? CFMachPortCreateWithPort(allocator, port, __CFMessagePortDummyCallback, &ctx, NULL) : NULL;
    CFAllocatorDeallocate(allocator, utfname);
    if (NULL == native) {
	// name is released by deallocation
	CFRelease(memory);
	return NULL;
    }
    memory->_port = native;
    CFMachPortSetInvalidationCallBack(native, __CFMessagePortInvalidationCallBack);
    __CFMessagePortSetValid(memory);
    __CFSpinLock(&__CFAllMessagePortsLock);
    if (!perPID && NULL != name) {
	CFMessagePortRef existing;
	if (NULL != __CFAllRemoteMessagePorts && CFDictionaryGetValueIfPresent(__CFAllRemoteMessagePorts, name, (const void **)&existing)) {
	    CFRetain(existing);
	    __CFSpinUnlock(&__CFAllMessagePortsLock);
	    CFRelease(memory);
	    return (CFMessagePortRef)(existing);
	}
	if (NULL == __CFAllRemoteMessagePorts) {
	    __CFAllRemoteMessagePorts = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeDictionaryKeyCallBacks, NULL);
	}
	CFDictionaryAddValue(__CFAllRemoteMessagePorts, name, memory);
    }
    __CFSpinUnlock(&__CFAllMessagePortsLock);
    return (CFMessagePortRef)memory;
}

CFMessagePortRef CFMessagePortCreateRemote(CFAllocatorRef allocator, CFStringRef name) {
    return __CFMessagePortCreateRemote(allocator, name, false, 0);
}

CFMessagePortRef CFMessagePortCreatePerProcessRemote(CFAllocatorRef allocator, CFStringRef name, CFIndex pid) {
    return __CFMessagePortCreateRemote(allocator, name, true, pid);
}

Boolean CFMessagePortIsRemote(CFMessagePortRef ms) {
    __CFGenericValidateType(ms, __kCFMessagePortTypeID);
    return __CFMessagePortIsRemote(ms);
}

CFStringRef CFMessagePortGetName(CFMessagePortRef ms) {
    __CFGenericValidateType(ms, __kCFMessagePortTypeID);
    return ms->_name;
}

Boolean CFMessagePortSetName(CFMessagePortRef ms, CFStringRef name) {
    CFAllocatorRef allocator = CFGetAllocator(ms);
    uint8_t *utfname = NULL;

    __CFGenericValidateType(ms, __kCFMessagePortTypeID);
    if (ms->_perPID || __CFMessagePortIsRemote(ms)) return false;
    name = __CFMessagePortSanitizeStringName(allocator, name, &utfname, NULL);
    if (NULL == name) {
	return false;
    }
    __CFSpinLock(&__CFAllMessagePortsLock);
    if (NULL != name) {
	CFMessagePortRef existing;
	if (NULL != __CFAllLocalMessagePorts && CFDictionaryGetValueIfPresent(__CFAllLocalMessagePorts, name, (const void **)&existing)) {
	    __CFSpinUnlock(&__CFAllMessagePortsLock);
	    CFRelease(name);
	    CFAllocatorDeallocate(allocator, utfname);
	    return false;
	}
    }
    __CFSpinUnlock(&__CFAllMessagePortsLock);

    if (NULL != name && (NULL == ms->_name || !CFEqual(ms->_name, name))) {
	CFMachPortRef oldPort = ms->_port;
        CFMachPortRef native = NULL;
        kern_return_t ret;
        mach_port_t bs, mp;
        task_get_bootstrap_port(mach_task_self(), &bs);
        ret = bootstrap_check_in(bs, (char *)utfname, &mp); /* If we're started by launchd or the old mach_init */
        if (ret == KERN_SUCCESS) {
            ret = mach_port_insert_right(mach_task_self(), mp, mp, MACH_MSG_TYPE_MAKE_SEND);
            if (KERN_SUCCESS == ret) {
                CFMachPortContext ctx = {0, ms, NULL, NULL, NULL};
                native = CFMachPortCreateWithPort(allocator, mp, __CFMessagePortDummyCallback, &ctx, NULL);
		__CFMessagePortSetExtraMachRef(ms);
            } else {
                mach_port_destroy(mach_task_self(), mp);
                CFAllocatorDeallocate(allocator, utfname);
		CFRelease(name);
                return false;
            }
        } 
        if (!native) {
            CFMachPortContext ctx = {0, ms, NULL, NULL, NULL};
            native = CFMachPortCreate(allocator, __CFMessagePortDummyCallback, &ctx, NULL);
            mp = CFMachPortGetPort(native);
            ret = bootstrap_register2(bs, (char *)utfname, mp, 0);
            if (ret != KERN_SUCCESS) {
                CFLog(kCFLogLevelWarning, CFSTR("*** CFMessagePort: bootstrap_register(): failed %d (0x%x) '%s', port = 0x%x, name = '%s'\nSee /usr/include/servers/bootstrap_defs.h for the error codes."), ret, ret, bootstrap_strerror(ret), mp, utfname);
                CFMachPortInvalidate(native);
                CFRelease(native);
                CFAllocatorDeallocate(allocator, utfname);
                CFRelease(name);
                return false;
            }
        }
        CFMachPortSetInvalidationCallBack(native, __CFMessagePortInvalidationCallBack);
        ms->_port = native;
	if (NULL != oldPort && oldPort != native) {
	    if (__CFMessagePortExtraMachRef(ms)) {
		mach_port_mod_refs(mach_task_self(), CFMachPortGetPort(oldPort), MACH_PORT_RIGHT_SEND, -1);
		mach_port_mod_refs(mach_task_self(), CFMachPortGetPort(oldPort), MACH_PORT_RIGHT_RECEIVE, -1);
	    }
	    CFMachPortInvalidate(oldPort);
	    CFRelease(oldPort);
	}
	__CFSpinLock(&__CFAllMessagePortsLock);
	// This relocking without checking to see if something else has grabbed
	// that name in the cache is rather suspect, but what would that even
	// mean has happened?  We'd expect the bootstrap_* calls above to have
	// failed for this one and not gotten this far, or failed for all of the
	// other simultaneous attempts to get the name (and having succeeded for
	// this one, gotten here).  So we're not going to try very hard here
	// with the thread-safety.
	if (NULL == __CFAllLocalMessagePorts) {
	    __CFAllLocalMessagePorts = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeDictionaryKeyCallBacks, NULL);
	}
	if (NULL != ms->_name) {
	    CFDictionaryRemoveValue(__CFAllLocalMessagePorts, ms->_name);
	    CFRelease(ms->_name);
	}
	ms->_name = name;
	CFDictionaryAddValue(__CFAllLocalMessagePorts, name, ms);
	__CFSpinUnlock(&__CFAllMessagePortsLock);
    }

    CFAllocatorDeallocate(allocator, utfname);
    return true;
}

void CFMessagePortGetContext(CFMessagePortRef ms, CFMessagePortContext *context) {
    __CFGenericValidateType(ms, __kCFMessagePortTypeID);
//#warning CF: assert that this is a local port
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    memmove(context, &ms->_context, sizeof(CFMessagePortContext));
}

void CFMessagePortInvalidate(CFMessagePortRef ms) {
    __CFGenericValidateType(ms, __kCFMessagePortTypeID);
    if (!__CFMessagePortIsDeallocing(ms)) {
	CFRetain(ms);
    }
    __CFMessagePortLock(ms);
    if (__CFMessagePortIsValid(ms)) {
	CFMessagePortInvalidationCallBack callout = ms->_icallout;
	CFRunLoopSourceRef source = ms->_source;
	CFMachPortRef replyPort = ms->_replyPort;
	CFMachPortRef port = ms->_port;
	CFStringRef name = ms->_name;
	void *info = NULL;

	__CFMessagePortUnsetValid(ms);
	if (!__CFMessagePortIsRemote(ms)) {
	    info = ms->_context.info;
	    ms->_context.info = NULL;
	}
	ms->_source = NULL;
	ms->_replyPort = NULL;
	__CFMessagePortUnlock(ms);

	__CFSpinLock(&__CFAllMessagePortsLock);
	if (0 == ms->_perPID && NULL != (__CFMessagePortIsRemote(ms) ? __CFAllRemoteMessagePorts : __CFAllLocalMessagePorts)) {
	    CFDictionaryRemoveValue(__CFMessagePortIsRemote(ms) ? __CFAllRemoteMessagePorts : __CFAllLocalMessagePorts, name);
	}
	__CFSpinUnlock(&__CFAllMessagePortsLock);
	if (NULL != callout) {
	    callout(ms, info);
	}
	// We already know we're going invalid, don't need this callback
	// anymore; plus, this solves a reentrancy deadlock; also, this
	// must be done before the deallocate of the Mach port, to
	// avoid a race between the notification message which could be
	// handled in another thread, and this NULL'ing out.
	CFMachPortSetInvalidationCallBack(port, NULL);
	// For hashing and equality purposes, cannot get rid of _port here
	if (!__CFMessagePortIsRemote(ms) && NULL != ms->_context.release) {
	    ms->_context.release(info);
	}
	if (NULL != source) {
	    CFRunLoopSourceInvalidate(source);
	    CFRelease(source);
	}
	if (NULL != replyPort) {
	    CFMachPortInvalidate(replyPort);
	    CFRelease(replyPort);
	}
	if (__CFMessagePortIsRemote(ms)) {
	    // Get rid of our extra ref on the Mach port gotten from bs server
	    mach_port_deallocate(mach_task_self(), CFMachPortGetPort(port));
	}
    } else {
	__CFMessagePortUnlock(ms);
    }
    if (!__CFMessagePortIsDeallocing(ms)) {
	CFRelease(ms);
    }
}

Boolean CFMessagePortIsValid(CFMessagePortRef ms) {
    __CFGenericValidateType(ms, __kCFMessagePortTypeID);
    if (!__CFMessagePortIsValid(ms)) return false;
    if (NULL != ms->_port && !CFMachPortIsValid(ms->_port)) {
	CFMessagePortInvalidate(ms);
	return false;
    }
    if (NULL != ms->_replyPort && !CFMachPortIsValid(ms->_replyPort)) {
	CFMessagePortInvalidate(ms);
	return false;
    }
    if (NULL != ms->_source && !CFRunLoopSourceIsValid(ms->_source)) {
	CFMessagePortInvalidate(ms);
	return false;
    }
    return true;
}

CFMessagePortInvalidationCallBack CFMessagePortGetInvalidationCallBack(CFMessagePortRef ms) {
    __CFGenericValidateType(ms, __kCFMessagePortTypeID);
    return ms->_icallout;
}

void CFMessagePortSetInvalidationCallBack(CFMessagePortRef ms, CFMessagePortInvalidationCallBack callout) {
    __CFGenericValidateType(ms, __kCFMessagePortTypeID);
    if (!__CFMessagePortIsValid(ms) && NULL != callout) {
	callout(ms, ms->_context.info);
    } else {
	ms->_icallout = callout;
    }
}

static void __CFMessagePortReplyCallBack(CFMachPortRef port, void *msg, CFIndex size, void *info) {
    CFMessagePortRef ms = info;
    struct __CFMessagePortMachMessage *msgp = msg;
    struct __CFMessagePortMachMessage *replymsg;
    __CFMessagePortLock(ms);
    if (!__CFMessagePortIsValid(ms)) {
	__CFMessagePortUnlock(ms);
	return;
    }
// assert: (int32_t)msgp->head.msgh_id < 0
    if (CFDictionaryContainsKey(ms->_replies, (void *)(uintptr_t)msgp->head.msgh_id)) {
	CFDataRef reply = NULL;
	replymsg = (struct __CFMessagePortMachMessage *)msg;
	if (0 == replymsg->body.msgh_descriptor_count) {
	    int32_t byteslen = CFSwapInt32LittleToHost(replymsg->contents.msg0.byteslen);
	    if (0 <= byteslen) {
		reply = CFDataCreate(kCFAllocatorSystemDefault, replymsg->contents.msg0.bytes, byteslen);
	    } else {
		reply = (void *)~0;	// means NULL data
	    }
	} else {
//#warning CF: should create a no-copy data here that has a custom VM-freeing allocator, and not vm_dealloc here
	    reply = CFDataCreate(kCFAllocatorSystemDefault, replymsg->contents.msg1.desc.out_of_line.address, replymsg->contents.msg1.desc.out_of_line.size);
	    vm_deallocate(mach_task_self(), (vm_address_t)replymsg->contents.msg1.desc.out_of_line.address, replymsg->contents.msg1.desc.out_of_line.size);
	}
	CFDictionarySetValue(ms->_replies, (void *)(uintptr_t)msgp->head.msgh_id, (void *)reply);
    } else {	/* discard message */
	if (1 == msgp->body.msgh_descriptor_count) {
	    vm_deallocate(mach_task_self(), (vm_address_t)msgp->contents.msg1.desc.out_of_line.address, msgp->contents.msg1.desc.out_of_line.size);
	}
    }
    __CFMessagePortUnlock(ms);
}

SInt32 CFMessagePortSendRequest(CFMessagePortRef remote, SInt32 msgid, CFDataRef data, CFTimeInterval sendTimeout, CFTimeInterval rcvTimeout, CFStringRef replyMode, CFDataRef *returnDatap) {
    struct __CFMessagePortMachMessage *sendmsg;
    CFRunLoopRef currentRL = CFRunLoopGetCurrent();
    CFRunLoopSourceRef source = NULL;
    CFDataRef reply = NULL;
    int64_t termTSR;
    uint32_t sendOpts = 0, sendTimeOut = 0;
    int32_t desiredReply;
    Boolean didRegister = false;
    kern_return_t ret;

    //#warning CF: This should be an assert
    // if (!__CFMessagePortIsRemote(remote)) return -999;
    if (!__CFMessagePortIsValid(remote)) return kCFMessagePortIsInvalid;
    __CFMessagePortLock(remote);
    if (NULL == remote->_replyPort) {
	CFMachPortContext context;
	context.version = 0;
	context.info = remote;
	context.retain = (const void *(*)(const void *))CFRetain;
	context.release = (void (*)(const void *))CFRelease;
	context.copyDescription = (CFStringRef (*)(const void *))__CFMessagePortCopyDescription;
	remote->_replyPort = CFMachPortCreate(CFGetAllocator(remote), __CFMessagePortReplyCallBack, &context, NULL);
    }
    remote->_convCounter++;
    desiredReply = -remote->_convCounter;
    sendmsg = __CFMessagePortCreateMessage(kCFAllocatorSystemDefault, false, CFMachPortGetPort(remote->_port), (replyMode != NULL ? CFMachPortGetPort(remote->_replyPort) : MACH_PORT_NULL), -desiredReply, msgid, (data ? CFDataGetBytePtr(data) : NULL), (data ? CFDataGetLength(data) : 0));
    __CFMessagePortUnlock(remote);
    if (replyMode != NULL) {
        CFDictionarySetValue(remote->_replies, (void *)(uintptr_t)desiredReply, NULL);
        source = CFMachPortCreateRunLoopSource(CFGetAllocator(remote), remote->_replyPort, -100);
        didRegister = !CFRunLoopContainsSource(currentRL, source, replyMode);
	if (didRegister) {
            CFRunLoopAddSource(currentRL, source, replyMode);
	}
    }
    if (sendTimeout < 10.0*86400) {
	// anything more than 10 days is no timeout!
	sendOpts = MACH_SEND_TIMEOUT;
	sendTimeout *= 1000.0;
	if (sendTimeout < 1.0) sendTimeout = 0.0;
	sendTimeOut = floor(sendTimeout);
    }
    ret = mach_msg((mach_msg_header_t *)sendmsg, MACH_SEND_MSG|sendOpts, sendmsg->head.msgh_size, 0, MACH_PORT_NULL, sendTimeOut, MACH_PORT_NULL);
    if (KERN_SUCCESS != ret) {
	// need to deallocate the send-once right that might have been created
	if (replyMode != NULL) mach_port_deallocate(mach_task_self(), ((mach_msg_header_t *)sendmsg)->msgh_local_port);
	if (didRegister) {
	    CFRunLoopRemoveSource(currentRL, source, replyMode);
	}
	if (source) CFRelease(source);
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, sendmsg);
	return (MACH_SEND_TIMED_OUT == ret) ? kCFMessagePortSendTimeout : kCFMessagePortTransportError;
    }
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, sendmsg);
    if (replyMode == NULL) {
	return kCFMessagePortSuccess;
    }
    CFRetain(remote); // retain during run loop to avoid invalidation causing freeing
    _CFMachPortInstallNotifyPort(currentRL, replyMode);
    termTSR = mach_absolute_time() + __CFTimeIntervalToTSR(rcvTimeout);
    for (;;) {
	CFRunLoopRunInMode(replyMode, __CFTSRToTimeInterval(termTSR - mach_absolute_time()), true);
	// warning: what, if anything, should be done if remote is now invalid?
	reply = CFDictionaryGetValue(remote->_replies, (void *)(uintptr_t)desiredReply);
	if (NULL != reply || termTSR < (int64_t)mach_absolute_time()) {
	    break;
	}
	if (!CFMessagePortIsValid(remote)) {
	    // no reason that reply port alone should go invalid so we don't check for that
	    break;
	}
    }
    // Should we uninstall the notify port?  A complex question...
    if (didRegister) {
        CFRunLoopRemoveSource(currentRL, source, replyMode);
    }
    if (source) CFRelease(source);
    if (NULL == reply) {
	CFDictionaryRemoveValue(remote->_replies, (void *)(uintptr_t)desiredReply);
	CFRelease(remote);
	return CFMessagePortIsValid(remote) ? kCFMessagePortReceiveTimeout : -5;
    }
    if (NULL != returnDatap) {
	*returnDatap = ((void *)~0 == reply) ? NULL : reply;
    } else if ((void *)~0 != reply) {
	CFRelease(reply);
    }
    CFDictionaryRemoveValue(remote->_replies, (void *)(uintptr_t)desiredReply);
    CFRelease(remote);
    return kCFMessagePortSuccess;
}

static mach_port_t __CFMessagePortGetPort(void *info) {
    CFMessagePortRef ms = info;
    if (!ms->_port) CFLog(kCFLogLevelWarning, CFSTR("*** Warning: A local CFMessagePort (%p) is being put in a run loop, but it has not been named yet, so this will be a no-op and no messages are going to be received, even if named later."), info);
    return ms->_port ? CFMachPortGetPort(ms->_port) : MACH_PORT_NULL;
}

static void *__CFMessagePortPerform(void *msg, CFIndex size, CFAllocatorRef allocator, void *info) {
    CFMessagePortRef ms = info;
    struct __CFMessagePortMachMessage *msgp = msg;
    struct __CFMessagePortMachMessage *replymsg;
    void *context_info;
    void (*context_release)(const void *);
    CFDataRef returnData, data = NULL;
    void *return_bytes = NULL;
    CFIndex return_len = 0;
    int32_t msgid;

    __CFMessagePortLock(ms);
    if (!__CFMessagePortIsValid(ms)) {
	__CFMessagePortUnlock(ms);
	return NULL;
    }
// assert: 0 < (int32_t)msgp->head.msgh_id
    if (NULL != ms->_context.retain) {
	context_info = (void *)ms->_context.retain(ms->_context.info);
	context_release = ms->_context.release;
    } else {
	context_info = ms->_context.info;
	context_release = NULL;
    }
    __CFMessagePortUnlock(ms);
    /* Create no-copy, no-free-bytes wrapper CFData */
    if (0 == msgp->body.msgh_descriptor_count) {
	int32_t byteslen = CFSwapInt32LittleToHost(msgp->contents.msg0.byteslen);
	msgid = CFSwapInt32LittleToHost(msgp->contents.msg0.msgid);
	if (0 <= byteslen) {
	    data = CFDataCreateWithBytesNoCopy(kCFAllocatorSystemDefault, msgp->contents.msg0.bytes, byteslen, kCFAllocatorNull);
	}
    } else {
	msgid = CFSwapInt32LittleToHost(msgp->contents.msg1.msgid);
	data = CFDataCreateWithBytesNoCopy(kCFAllocatorSystemDefault, msgp->contents.msg1.desc.out_of_line.address, msgp->contents.msg1.desc.out_of_line.size, kCFAllocatorNull);
    }
    returnData = ms->_callout(ms, msgid, data, context_info);
    /* Now, returnData could be (1) NULL, (2) an ordinary data < MAX_INLINE,
    (3) ordinary data >= MAX_INLINE, (4) a no-copy data < MAX_INLINE,
    (5) a no-copy data >= MAX_INLINE. In cases (2) and (4), we send the return
    bytes inline in the Mach message, so can release the returnData object
    here. In cases (3) and (5), we'll send the data out-of-line, we need to
    create a copy of the memory, which we'll have the kernel autodeallocate
    for us on send. In case (4) also, the bytes in the return data may be part
    of the bytes in "data" that we sent into the callout, so if the incoming
    data was received out of line, we wouldn't be able to clean up the out-of-line
    wad until the message was sent either, if we didn't make the copy. */
    if (NULL != returnData) {
	return_len = CFDataGetLength(returnData);
	if (return_len < __CFMessagePortMaxInlineBytes) {
	    return_bytes = (void *)CFDataGetBytePtr(returnData);
	} else {
	    return_bytes = NULL;
	    vm_allocate(mach_task_self(), (vm_address_t *)&return_bytes, return_len, VM_FLAGS_ANYWHERE | VM_MAKE_TAG(VM_MEMORY_MACH_MSG));
	    /* vm_copy would only be a win here if the source address
		is page aligned; it is a lose in all other cases, since
		the kernel will just do the memmove for us (but not in
		as simple a way). */
	    memmove(return_bytes, CFDataGetBytePtr(returnData), return_len);
	}
    }
    replymsg = __CFMessagePortCreateMessage(allocator, true, msgp->head.msgh_remote_port, MACH_PORT_NULL, -1 * (int32_t)msgp->head.msgh_id, msgid, return_bytes, return_len);
    if (1 == replymsg->body.msgh_descriptor_count) {
	replymsg->contents.msg1.desc.out_of_line.deallocate = true;
    }
    if (data) CFRelease(data);
    if (1 == msgp->body.msgh_descriptor_count) {
	vm_deallocate(mach_task_self(), (vm_address_t)msgp->contents.msg1.desc.out_of_line.address, msgp->contents.msg1.desc.out_of_line.size);
    }
    if (returnData) CFRelease(returnData);
    if (context_release) {
	context_release(context_info);
    }
    return replymsg;
}

CFRunLoopSourceRef CFMessagePortCreateRunLoopSource(CFAllocatorRef allocator, CFMessagePortRef ms, CFIndex order) {
    CFRunLoopSourceRef result = NULL;
    __CFGenericValidateType(ms, __kCFMessagePortTypeID);
//#warning CF: This should be an assert
   // if (__CFMessagePortIsRemote(ms)) return NULL;
    __CFMessagePortLock(ms);
    if (NULL == ms->_source && __CFMessagePortIsValid(ms)) {
	CFRunLoopSourceContext1 context;
	context.version = 1;
	context.info = (void *)ms;
	context.retain = (const void *(*)(const void *))CFRetain;
	context.release = (void (*)(const void *))CFRelease;
	context.copyDescription = (CFStringRef (*)(const void *))__CFMessagePortCopyDescription;
	context.equal = NULL;
	context.hash = NULL;
	context.getPort = __CFMessagePortGetPort;
	context.perform = __CFMessagePortPerform;
	ms->_source = CFRunLoopSourceCreate(allocator, order, (CFRunLoopSourceContext *)&context);
    }
    if (NULL != ms->_source) {
	result = (CFRunLoopSourceRef)CFRetain(ms->_source);
    }
    __CFMessagePortUnlock(ms);
    return result;
}


