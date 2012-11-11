/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
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

/*	CFStream.c
	Copyright (c) 2000-2009, Apple Inc. All rights reserved.
	Responsibility: Becky Willrich
*/

#include <CoreFoundation/CFRuntime.h>
#include <CoreFoundation/CFNumber.h>
#include <string.h>
#include "CFStreamInternal.h"
#include "CFInternal.h"
#include <stdio.h>

struct CFStreamAux {
	CFSpinLock_t streamLock;
};

enum {
	MIN_STATUS_CODE_BIT	= 0,
        // ..status bits...
	MAX_STATUS_CODE_BIT	= 4,
    
	CONSTANT_CALLBACKS	= 5,
	CALLING_CLIENT		= 6,	// MUST remain 6 since it's value is used elsewhere.
	
    HAVE_CLOSED			= 7,
    
    // Values above used to be defined and others may rely on their values
    
    // Values below should not matter if they are re-ordered or shift
    
    SHARED_SOURCE
};


/* CALLING_CLIENT really determines whether stream events will be sent to the client immediately, or posted for the next time through the runloop.  Since the client may not be prepared for re-entrancy, we must always set/clear this bit around public entry points. -- REW, 9/5/2001 
    Also, CALLING_CLIENT is now used from CFFilteredStream.c (which has a copy of the #define above).  Really gross.  We should find a way to avoid that.... -- REW, 3/27/2002  */
// Used in CFNetwork too

/* sSharesSources holds two mappings, one the inverse of the other, between a stream and the
   RunLoop+RunLoopMode pair that it's scheduled in.  If the stream is scheduled in more than
   one loop or mode, it can't share RunLoopSources with others, and is not in this dict.
*/
static CFSpinLock_t sSourceLock = CFSpinLockInit;
static CFMutableDictionaryRef sSharedSources = NULL;

static CFTypeID __kCFReadStreamTypeID = _kCFRuntimeNotATypeID;
static CFTypeID __kCFWriteStreamTypeID = _kCFRuntimeNotATypeID;

// Just reads the bits, for those cases where we don't want to go through any callback checking
#define __CFStreamGetStatus(x) __CFBitfieldGetValue((x)->flags, MAX_STATUS_CODE_BIT, MIN_STATUS_CODE_BIT)

__private_extern__ CFStreamStatus _CFStreamGetStatus(struct _CFStream *stream);
static Boolean _CFStreamRemoveRunLoopAndModeFromArray(CFMutableArrayRef runLoopsAndModes, CFRunLoopRef rl, CFStringRef mode);
static void _wakeUpRunLoop(struct _CFStream *stream);

CF_INLINE void* _CFStreamCreateReserved(CFAllocatorRef alloc) {
	struct CFStreamAux* aux = (struct CFStreamAux*) CFAllocatorAllocate(alloc, sizeof(struct CFStreamAux), 0);
	if (aux) {
		aux->streamLock = CFSpinLockInit;
	}
	return aux;
}

CF_INLINE void _CFStreamDestroyReserved(CFAllocatorRef alloc, void* aux) {
	CFAllocatorDeallocate(alloc, aux);
}

CF_INLINE struct CFStreamAux* _CFStreamGetAuxRecord(struct _CFStream* stream) {
	return (struct CFStreamAux*) stream->_reserved1;
}

CF_INLINE void _CFStreamLock(struct _CFStream* stream) {
	__CFSpinLock(&_CFStreamGetAuxRecord(stream)->streamLock);
}

CF_INLINE void _CFStreamUnlock(struct _CFStream* stream) {
	__CFSpinUnlock(&_CFStreamGetAuxRecord(stream)->streamLock);
}

CF_INLINE CFRunLoopSourceRef _CFStreamCopySource(struct _CFStream* stream) {
	CFRunLoopSourceRef source = NULL;
	
	if (stream) {
		_CFStreamLock(stream);
		
		if (stream->client)
			source = stream->client->rlSource;

		if (source)
			CFRetain(source);
		
		_CFStreamUnlock(stream);
	}
	
	return source;
}

CF_INLINE void _CFStreamSetSource(struct _CFStream* stream, CFRunLoopSourceRef source, Boolean invalidateOldSource) {
	CFRunLoopSourceRef oldSource = NULL;
	
	if (stream) {
		_CFStreamLock(stream);
		if (stream->client) {
			oldSource = stream->client->rlSource;
			if (oldSource != NULL)
				CFRetain(oldSource);
			
			stream->client->rlSource = source;
			if (source != NULL)
				CFRetain(source);
		}
		_CFStreamUnlock(stream);
	}
	
	if (oldSource) {
		// Lose our extra retain
		CFRelease(oldSource);
		
		if (invalidateOldSource)
			CFRunLoopSourceInvalidate(oldSource);
		
		// And lose the one that held it in our stream as well
		CFRelease(oldSource);
	}
}

CF_INLINE const struct _CFStreamCallBacks *_CFStreamGetCallBackPtr(struct _CFStream *stream) {
    return stream->callBacks;
}

CF_INLINE void _CFStreamSetStatusCode(struct _CFStream *stream, CFStreamStatus newStatus) {
    CFStreamStatus status = __CFStreamGetStatus(stream);
    if (((status != kCFStreamStatusClosed) && (status != kCFStreamStatusError)) ||
        ((status == kCFStreamStatusClosed) && (newStatus == kCFStreamStatusError)))
    {
        __CFBitfieldSetValue(stream->flags, MAX_STATUS_CODE_BIT, MIN_STATUS_CODE_BIT, newStatus);
    }
}

CF_INLINE void _CFStreamScheduleEvent(struct _CFStream *stream, CFStreamEventType event) {
    if (stream->client && (stream->client->when & event)) {
		CFRunLoopSourceRef source = _CFStreamCopySource(stream);
        if (source) {
            stream->client->whatToSignal |= event;

			CFRunLoopSourceSignal(source);
			CFRelease(source);
	        _wakeUpRunLoop(stream);
	    }
	}
}

CF_INLINE void _CFStreamSetStreamError(struct _CFStream *stream, CFStreamError *err) {
    if (!stream->error) {
        stream->error = (CFErrorRef)CFAllocatorAllocate(CFGetAllocator(stream), sizeof(CFStreamError), 0);
    }
    memmove(stream->error, err, sizeof(CFStreamError));
}

static CFStringRef __CFStreamCopyDescription(CFTypeRef cf) {
    struct _CFStream *stream = (struct _CFStream *)cf;
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    CFStringRef contextDescription;
    CFStringRef desc;
    if (cb->copyDescription) {
        if (cb->version == 0) {
            contextDescription = ((CFStringRef(*)(void *))cb->copyDescription)(_CFStreamGetInfoPointer(stream));
        } else {
            contextDescription = cb->copyDescription(stream, _CFStreamGetInfoPointer(stream));
        }
    } else {
        contextDescription = CFStringCreateWithFormat(CFGetAllocator(stream), NULL, CFSTR("info = %p"), _CFStreamGetInfoPointer(stream));
    }
    if (CFGetTypeID(cf) == __kCFReadStreamTypeID) {
        desc = CFStringCreateWithFormat(CFGetAllocator(stream), NULL, CFSTR("<CFReadStream %p>{%@}"), stream, contextDescription);
    } else {
        desc = CFStringCreateWithFormat(CFGetAllocator(stream), NULL, CFSTR("<CFWriteStream %p>{%@}"), stream, contextDescription);
    }
    CFRelease(contextDescription);
    return desc;
}

static void _CFStreamDetachSource(struct _CFStream* stream) {
	if (stream && stream->client && stream->client->rlSource) {
        if (!__CFBitIsSet(stream->flags, SHARED_SOURCE)) {
			_CFStreamSetSource(stream, NULL, TRUE);
        }
        else {
            
            CFArrayRef runLoopAndSourceKey;
            CFMutableArrayRef list;
			CFIndex count;
			CFIndex i;
            
            __CFSpinLock(&sSourceLock);
            
            runLoopAndSourceKey = (CFArrayRef)CFDictionaryGetValue(sSharedSources, stream);
            list = (CFMutableArrayRef)CFDictionaryGetValue(sSharedSources, runLoopAndSourceKey);
            
			count = CFArrayGetCount(list);
			i = CFArrayGetFirstIndexOfValue(list, CFRangeMake(0, count), stream);
            if (i != kCFNotFound) {
                CFArrayRemoveValueAtIndex(list, i);
				count--;
            }
            
            CFAssert(CFArrayGetFirstIndexOfValue(list, CFRangeMake(0, CFArrayGetCount(list)), stream) == kCFNotFound, __kCFLogAssertion, "CFStreamClose: stream found twice in its shared source's list");

			if (count == 0) {
				CFRunLoopSourceRef source = _CFStreamCopySource(stream);
				if (source) {
					CFRunLoopRemoveSource((CFRunLoopRef)CFArrayGetValueAtIndex(runLoopAndSourceKey, 0), source, (CFStringRef)CFArrayGetValueAtIndex(runLoopAndSourceKey, 1));
					CFRelease(source);
				}
                CFDictionaryRemoveValue(sSharedSources, runLoopAndSourceKey);
            }
            
            CFDictionaryRemoveValue(sSharedSources, stream);
            
			_CFStreamSetSource(stream, NULL, count == 0);
			
            __CFBitClear(stream->flags, SHARED_SOURCE);

            __CFSpinUnlock(&sSourceLock);
        }
    }
}

__private_extern__ void _CFStreamClose(struct _CFStream *stream) {
    CFStreamStatus status = _CFStreamGetStatus(stream);
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    if (status == kCFStreamStatusNotOpen || status == kCFStreamStatusClosed || (status == kCFStreamStatusError && __CFBitIsSet(stream->flags, HAVE_CLOSED))) {
        // Stream is not open from the client's perspective; do not callout and do not update our status to "closed"
        return;
    }
    __CFBitSet(stream->flags, HAVE_CLOSED);
    __CFBitSet(stream->flags, CALLING_CLIENT);
    if (cb->close) {
        cb->close(stream, _CFStreamGetInfoPointer(stream));
    }
    if (stream->client) {
        _CFStreamDetachSource(stream);
    }
    _CFStreamSetStatusCode(stream, kCFStreamStatusClosed);
    __CFBitClear(stream->flags, CALLING_CLIENT);
}

//static int numStreamInstances = 0;

static void __CFStreamDeallocate(CFTypeRef cf) {
    struct _CFStream *stream = (struct _CFStream *)cf;
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    CFAllocatorRef alloc = CFGetAllocator(stream);
//    numStreamInstances --;

    // Close the stream
    _CFStreamClose(stream);

    if (stream->client) {
        CFStreamClientContext *cbContext;
        cbContext = &(stream->client->cbContext);
        if (cbContext->info && cbContext->release) {
            cbContext->release(cbContext->info);
        }
        _CFStreamDetachSource(stream);
        if (stream->client->runLoopsAndModes) {
            CFRelease(stream->client->runLoopsAndModes);
        }
        
        CFAllocatorDeallocate(alloc, stream->client);
        stream->client = NULL; // Just in case finalize, below, calls back in to us
    }
    if (cb->finalize) {
        if (cb->version == 0) {
            ((void(*)(void *))cb->finalize)(_CFStreamGetInfoPointer(stream));
        } else {
            cb->finalize(stream, _CFStreamGetInfoPointer(stream));
        }
    }
    if (stream->error) {
        if (cb->version < 2) {
            CFAllocatorDeallocate(alloc, stream->error);
        } else {
            CFRelease(stream->error);
        }
    }
    if (!__CFBitIsSet(stream->flags, CONSTANT_CALLBACKS)) {
        CFAllocatorDeallocate(alloc, (void *)stream->callBacks);
    }
	if (stream->_reserved1)
		_CFStreamDestroyReserved(alloc, stream->_reserved1);
}

static const CFRuntimeClass __CFReadStreamClass = {
    0,
    "CFReadStream",
    NULL,      // init
    NULL,      // copy
    __CFStreamDeallocate,
    NULL,
    NULL,
    NULL,      // copyHumanDesc
    __CFStreamCopyDescription
};

static const CFRuntimeClass __CFWriteStreamClass = {
    0,
    "CFWriteStream",
    NULL,      // init
    NULL,      // copy
    __CFStreamDeallocate,
    NULL,
    NULL,
    NULL,      // copyHumanDesc
    __CFStreamCopyDescription
};

CONST_STRING_DECL(kCFStreamPropertySocketNativeHandle, "kCFStreamPropertySocketNativeHandle")
CONST_STRING_DECL(kCFStreamPropertySocketRemoteHostName, "kCFStreamPropertySocketRemoteHostName")
CONST_STRING_DECL(kCFStreamPropertySocketRemotePortNumber, "kCFStreamPropertySocketRemotePortNumber")
CONST_STRING_DECL(kCFStreamPropertyDataWritten, "kCFStreamPropertyDataWritten")
CONST_STRING_DECL(kCFStreamPropertyAppendToFile, "kCFStreamPropertyAppendToFile")

__private_extern__ void __CFStreamInitialize(void) {
    __kCFReadStreamTypeID = _CFRuntimeRegisterClass(&__CFReadStreamClass);
    __kCFWriteStreamTypeID = _CFRuntimeRegisterClass(&__CFWriteStreamClass);
}


CF_EXPORT CFTypeID CFReadStreamGetTypeID(void) {
    return __kCFReadStreamTypeID;
}

CF_EXPORT CFTypeID CFWriteStreamGetTypeID(void) {
    return __kCFWriteStreamTypeID;
}

static struct _CFStream *_CFStreamCreate(CFAllocatorRef allocator, Boolean isReadStream) {
    struct _CFStream *newStream = (struct _CFStream *)_CFRuntimeCreateInstance(allocator, isReadStream ? __kCFReadStreamTypeID : __kCFWriteStreamTypeID, sizeof(struct _CFStream) - sizeof(CFRuntimeBase), NULL);
    if (newStream) {
//        numStreamInstances ++;
        newStream->flags = 0;
        _CFStreamSetStatusCode(newStream, kCFStreamStatusNotOpen);
        newStream->error = NULL;
        newStream->client = NULL;
        newStream->info = NULL;
        newStream->callBacks = NULL;
		
		newStream->_reserved1 = _CFStreamCreateReserved(allocator);
    }
    return newStream;
}

__private_extern__ struct _CFStream *_CFStreamCreateWithConstantCallbacks(CFAllocatorRef alloc, void *info,  const struct _CFStreamCallBacks *cb, Boolean isReading) {
    struct _CFStream *newStream;
    if (cb->version != 1) return NULL;
    newStream = _CFStreamCreate(alloc, isReading);
    if (newStream) {
        __CFBitSet(newStream->flags, CONSTANT_CALLBACKS);
        newStream->callBacks = cb;
        if (cb->create) {
            newStream->info = cb->create(newStream, info);
        } else {
            newStream->info = info;
        }
    }
    return newStream;
}

CF_EXPORT void _CFStreamSetInfoPointer(struct _CFStream *stream, void *info, const struct _CFStreamCallBacks *cb) {
    if (info != stream->info) {
        if (stream->callBacks->finalize) {
            stream->callBacks->finalize(stream, stream->info);
        }
        if (cb->create) {
            stream->info = cb->create(stream, info);
        } else {
            stream->info = info;
        }
    }
    stream->callBacks = cb;
}


CF_EXPORT CFReadStreamRef CFReadStreamCreate(CFAllocatorRef alloc, const CFReadStreamCallBacks *callbacks, void *info) {
    struct _CFStream *newStream = _CFStreamCreate(alloc, TRUE);
    struct _CFStreamCallBacks *cb;
    if (!newStream) return NULL;
    cb = (struct _CFStreamCallBacks *)CFAllocatorAllocate(alloc, sizeof(struct _CFStreamCallBacks), 0);
    if (!cb) {
        CFRelease(newStream);
        return NULL;
    }
    if (callbacks->version == 0) {
        CFReadStreamCallBacksV0 *cbV0 = (CFReadStreamCallBacksV0 *)callbacks;
        CFStreamClientContext *ctxt = (CFStreamClientContext *)info;
        newStream->info = ctxt->retain ? (void *)ctxt->retain(ctxt->info) : ctxt->info;
        cb->version = 0;
        cb->create = (void *(*)(struct _CFStream *, void *))ctxt->retain;
        cb->finalize = (void(*)(struct _CFStream *, void *))ctxt->release;
        cb->copyDescription = (CFStringRef(*)(struct _CFStream *, void *))ctxt->copyDescription;
        cb->open = (Boolean(*)(struct _CFStream *, CFErrorRef *, Boolean *, void *))cbV0->open;
        cb->openCompleted = (Boolean (*)(struct _CFStream *, CFErrorRef *, void *))cbV0->openCompleted;
        cb->read = (CFIndex (*)(CFReadStreamRef, UInt8 *, CFIndex, CFErrorRef *, Boolean *, void *))cbV0->read;
        cb->getBuffer = (const UInt8 *(*)(CFReadStreamRef, CFIndex, CFIndex *, CFErrorRef *, Boolean *, void *))cbV0->getBuffer;
        cb->canRead = (Boolean (*)(CFReadStreamRef, CFErrorRef*, void*))cbV0->canRead;
        cb->write = NULL;
        cb->canWrite = NULL;
        cb->close = (void (*)(struct _CFStream *, void *))cbV0->close;
        cb->copyProperty = (CFTypeRef (*)(struct _CFStream *, CFStringRef, void *))cbV0->copyProperty;
        cb->setProperty = NULL;
        cb->requestEvents = NULL;
        cb->schedule = (void (*)(struct _CFStream *, CFRunLoopRef, CFStringRef, void *))cbV0->schedule;
        cb->unschedule = (void (*)(struct _CFStream *, CFRunLoopRef, CFStringRef, void *))cbV0->unschedule;
    } else if (callbacks->version == 1) {
        CFReadStreamCallBacksV1 *cbV1 = (CFReadStreamCallBacksV1 *)callbacks;
        newStream->info = cbV1->create ? cbV1->create((CFReadStreamRef)newStream, info) : info;
        cb->version = 1;
        cb->create = (void *(*)(struct _CFStream *, void *))cbV1->create;
        cb->finalize = (void(*)(struct _CFStream *, void *))cbV1->finalize;
        cb->copyDescription = (CFStringRef(*)(struct _CFStream *, void *))cbV1->copyDescription;
        cb->open = (Boolean(*)(struct _CFStream *, CFErrorRef *, Boolean *, void *))cbV1->open;
        cb->openCompleted = (Boolean (*)(struct _CFStream *, CFErrorRef *, void *))cbV1->openCompleted;
        cb->read = (CFIndex (*)(CFReadStreamRef, UInt8 *, CFIndex, CFErrorRef *, Boolean *, void *))cbV1->read;
        cb->getBuffer = (const UInt8 *(*)(CFReadStreamRef, CFIndex, CFIndex *, CFErrorRef *, Boolean *, void *))cbV1->getBuffer;
        cb->canRead = (Boolean (*)(CFReadStreamRef, CFErrorRef*, void*))cbV1->canRead;
        cb->write = NULL;
        cb->canWrite = NULL;
        cb->close = (void (*)(struct _CFStream *, void *))cbV1->close;
        cb->copyProperty = (CFTypeRef (*)(struct _CFStream *, CFStringRef, void *))cbV1->copyProperty;
        cb->setProperty = (Boolean(*)(struct _CFStream *, CFStringRef, CFTypeRef, void *))cbV1->setProperty;
        cb->requestEvents = (void(*)(struct _CFStream *, CFOptionFlags, void *))cbV1->requestEvents;
        cb->schedule = (void (*)(struct _CFStream *, CFRunLoopRef, CFStringRef, void *))cbV1->schedule;
        cb->unschedule = (void (*)(struct _CFStream *, CFRunLoopRef, CFStringRef, void *))cbV1->unschedule;
    } else {
        newStream->info = callbacks->create ? callbacks->create((CFReadStreamRef)newStream, info) : info;
        cb->version = 2;
        cb->create = (void *(*)(struct _CFStream *, void *))callbacks->create;
        cb->finalize = (void(*)(struct _CFStream *, void *))callbacks->finalize;
        cb->copyDescription = (CFStringRef(*)(struct _CFStream *, void *))callbacks->copyDescription;
        cb->open = (Boolean(*)(struct _CFStream *, CFErrorRef *, Boolean *, void *))callbacks->open;
        cb->openCompleted = (Boolean (*)(struct _CFStream *, CFErrorRef *, void *))callbacks->openCompleted;
        cb->read = callbacks->read;
        cb->getBuffer = callbacks->getBuffer;
        cb->canRead = callbacks->canRead;
        cb->write = NULL;
        cb->canWrite = NULL;
        cb->close = (void (*)(struct _CFStream *, void *))callbacks->close;
        cb->copyProperty = (CFTypeRef (*)(struct _CFStream *, CFStringRef, void *))callbacks->copyProperty;
        cb->setProperty = (Boolean(*)(struct _CFStream *, CFStringRef, CFTypeRef, void *))callbacks->setProperty;
        cb->requestEvents = (void(*)(struct _CFStream *, CFOptionFlags, void *))callbacks->requestEvents;
        cb->schedule = (void (*)(struct _CFStream *, CFRunLoopRef, CFStringRef, void *))callbacks->schedule;
        cb->unschedule = (void (*)(struct _CFStream *, CFRunLoopRef, CFStringRef, void *))callbacks->unschedule;
   }
    
    newStream->callBacks = cb;
    return (CFReadStreamRef)newStream;
}

CF_EXPORT CFWriteStreamRef CFWriteStreamCreate(CFAllocatorRef alloc, const CFWriteStreamCallBacks *callbacks, void *info) {
    struct _CFStream *newStream = _CFStreamCreate(alloc, FALSE);
    struct _CFStreamCallBacks *cb;
    if (!newStream) return NULL;
    cb = (struct _CFStreamCallBacks *)CFAllocatorAllocate(alloc, sizeof(struct _CFStreamCallBacks), 0);
    if (!cb) {
        CFRelease(newStream);
        return NULL;
    }
    if (callbacks->version == 0) {
        CFWriteStreamCallBacksV0 *cbV0 = (CFWriteStreamCallBacksV0 *)callbacks;
        CFStreamClientContext *ctxt = (CFStreamClientContext *)info;
        newStream->info = ctxt->retain ? (void *)ctxt->retain(ctxt->info) : ctxt->info;
        cb->version = 0;
        cb->create = (void *(*)(struct _CFStream *, void *))ctxt->retain;
        cb->finalize = (void(*)(struct _CFStream *, void *))ctxt->release;
        cb->copyDescription = (CFStringRef(*)(struct _CFStream *, void *))ctxt->copyDescription;
        cb->open = (Boolean(*)(struct _CFStream *, CFErrorRef *, Boolean *, void *))cbV0->open;
        cb->openCompleted = (Boolean (*)(struct _CFStream *, CFErrorRef *, void *))cbV0->openCompleted;
        cb->read = NULL;
        cb->getBuffer = NULL;
        cb->canRead = NULL;
        cb->write = (CFIndex(*)(CFWriteStreamRef stream, const UInt8 *buffer, CFIndex bufferLength, CFErrorRef *error, void *info))cbV0->write;
        cb->canWrite = (Boolean(*)(CFWriteStreamRef stream, CFErrorRef *error, void *info))cbV0->canWrite;
        cb->close = (void (*)(struct _CFStream *, void *))cbV0->close;
        cb->copyProperty = (CFTypeRef (*)(struct _CFStream *, CFStringRef, void *))cbV0->copyProperty;
        cb->setProperty = NULL;
        cb->requestEvents = NULL;
        cb->schedule = (void (*)(struct _CFStream *, CFRunLoopRef, CFStringRef, void *))cbV0->schedule;
        cb->unschedule = (void (*)(struct _CFStream *, CFRunLoopRef, CFStringRef, void *))cbV0->unschedule;
    } else if (callbacks->version == 1) {
        CFWriteStreamCallBacksV1 *cbV1 = (CFWriteStreamCallBacksV1 *)callbacks;
        cb->version = 1;
        newStream->info = cbV1->create ? cbV1->create((CFWriteStreamRef)newStream, info) : info;
        cb->create = (void *(*)(struct _CFStream *, void *))cbV1->create;
        cb->finalize = (void(*)(struct _CFStream *, void *))cbV1->finalize;
        cb->copyDescription = (CFStringRef(*)(struct _CFStream *, void *))cbV1->copyDescription;
        cb->open = (Boolean(*)(struct _CFStream *, CFErrorRef *, Boolean *, void *))cbV1->open;
        cb->openCompleted = (Boolean (*)(struct _CFStream *, CFErrorRef *, void *))cbV1->openCompleted;
        cb->read = NULL;
        cb->getBuffer = NULL;
        cb->canRead = NULL;
        cb->write = (CFIndex(*)(CFWriteStreamRef stream, const UInt8 *buffer, CFIndex bufferLength, CFErrorRef *error, void *info))cbV1->write;
        cb->canWrite = (Boolean(*)(CFWriteStreamRef stream, CFErrorRef *error, void *info))cbV1->canWrite;
        cb->close = (void (*)(struct _CFStream *, void *))cbV1->close;        
        cb->copyProperty = (CFTypeRef (*)(struct _CFStream *, CFStringRef, void *))cbV1->copyProperty;
        cb->setProperty = (Boolean (*)(struct _CFStream *, CFStringRef, CFTypeRef, void *))cbV1->setProperty;
        cb->requestEvents = (void(*)(struct _CFStream *, CFOptionFlags, void *))cbV1->requestEvents;
        cb->schedule = (void (*)(struct _CFStream *, CFRunLoopRef, CFStringRef, void *))cbV1->schedule;
        cb->unschedule = (void (*)(struct _CFStream *, CFRunLoopRef, CFStringRef, void *))cbV1->unschedule;
    } else {
        cb->version = callbacks->version;
        newStream->info = callbacks->create ? callbacks->create((CFWriteStreamRef)newStream, info) : info;
        cb->create = (void *(*)(struct _CFStream *, void *))callbacks->create;
        cb->finalize = (void(*)(struct _CFStream *, void *))callbacks->finalize;
        cb->copyDescription = (CFStringRef(*)(struct _CFStream *, void *))callbacks->copyDescription;
        cb->open = (Boolean(*)(struct _CFStream *, CFErrorRef *, Boolean *, void *))callbacks->open;
        cb->openCompleted = (Boolean (*)(struct _CFStream *, CFErrorRef *, void *))callbacks->openCompleted;
        cb->read = NULL;
        cb->getBuffer = NULL;
        cb->canRead = NULL;
        cb->write = callbacks->write;
        cb->canWrite = callbacks->canWrite;
        cb->close = (void (*)(struct _CFStream *, void *))callbacks->close;        
        cb->copyProperty = (CFTypeRef (*)(struct _CFStream *, CFStringRef, void *))callbacks->copyProperty;
        cb->setProperty = (Boolean (*)(struct _CFStream *, CFStringRef, CFTypeRef, void *))callbacks->setProperty;
        cb->requestEvents = (void(*)(struct _CFStream *, CFOptionFlags, void *))callbacks->requestEvents;
        cb->schedule = (void (*)(struct _CFStream *, CFRunLoopRef, CFStringRef, void *))callbacks->schedule;
        cb->unschedule = (void (*)(struct _CFStream *, CFRunLoopRef, CFStringRef, void *))callbacks->unschedule;
    }
    newStream->callBacks = cb;
    return (CFWriteStreamRef)newStream;
}

static void _signalEventSync(struct _CFStream* stream, CFOptionFlags whatToSignal)
{
    CFOptionFlags eventMask;

    __CFBitSet(stream->flags, CALLING_CLIENT);

    void* info = NULL;
    void (*release) (void*) = NULL;

    if (stream->client->cbContext.retain == NULL)
	info = stream->client->cbContext.info;
    else {
	info = stream->client->cbContext.retain(stream->client->cbContext.info);
	release = stream->client->cbContext.release;
    }

    for (eventMask = 1; eventMask <= whatToSignal; eventMask = eventMask << 1) {
	if ((eventMask & whatToSignal) && (stream->client->when & eventMask)) {
	    stream->client->cb(stream, eventMask, info);
	    
	    /* What happens if the callback sets the client to NULL?  We're in a loop here... Hmm. */
	    /* After writing that comment, I see: <rdar://problem/6793636> CFReadStreamSetClient(..., NULL) unsafely releases info pointer immediately */
        /* Of note, when the stream callbacks are set to to NULL, we're re-initalized so as not to receive more events, so we 
         * should break pout of this loop */
	}
    }

    if (release)
	(*release) (info);

    __CFBitClear(stream->flags, CALLING_CLIENT);
}

static void _cfstream_solo_signalEventSync(void* info)
{
    CFTypeID typeID = CFGetTypeID((CFTypeRef) info);
    
    if (typeID != CFReadStreamGetTypeID() && typeID != CFWriteStreamGetTypeID()) {
	CFLog(__kCFLogAssertion, CFSTR("Expected an read or write stream for %p"), info);
#if defined(DEBUG)
	abort();
#endif
    } else {
	struct _CFStream* stream = (struct _CFStream*) info;
	CFOptionFlags whatToSignal = stream->client->whatToSignal;
	stream->client->whatToSignal = 0;
	
	/* Since the array version holds a retain, we do it here as well, as opposed to taking a second retain in the client callback */
	CFRetain(stream);
	_signalEventSync(stream, whatToSignal);
	CFRelease(stream);
    }
}

static void _cfstream_shared_signalEventSync(void* info)
{
    CFTypeID typeID = CFGetTypeID((CFTypeRef) info);
    
    if (typeID != CFArrayGetTypeID()) {
	CFLog(__kCFLogAssertion, CFSTR("Expected an array for %p"), info);
#if defined(DEBUG)
	abort();
#endif
    } else {
	CFMutableArrayRef list = (CFMutableArrayRef) info;
	CFIndex c, i;
	CFOptionFlags whatToSignal = 0;
	struct _CFStream* stream = NULL;
	
	__CFSpinLock(&sSourceLock);
	
	/* Looks like, we grab the first stream that wants an event... */
	/* Note that I grab an extra retain when I pull out the stream here... */
	c = CFArrayGetCount(list);
	for (i = 0; i < c; i++) {
	    struct _CFStream* s = (struct _CFStream*)CFArrayGetValueAtIndex(list, i);
	    
	    if (s->client->whatToSignal) {
		stream = s;
		CFRetain(stream);
		whatToSignal = stream->client->whatToSignal;
		s->client->whatToSignal = 0;
		break;
	    }
	}
	
	/* And then we also signal any other streams in this array so that we get them next go? */
	for (; i < c;  i++) {
	    struct _CFStream* s = (struct _CFStream*)CFArrayGetValueAtIndex(list, i);
	    if (s->client->whatToSignal) {
			CFRunLoopSourceRef source = _CFStreamCopySource(s);
			if (source) {
				CFRunLoopSourceSignal(source);
				CFRelease(source);
			}
            break;
	    }
	}
	
	__CFSpinUnlock(&sSourceLock);
	
	/* We're sitting here now, possibly with a stream that needs to be processed by the common routine */
	if (stream) {
	    _signalEventSync(stream, whatToSignal);
	    
	    /* Lose our extra retain */
	    CFRelease(stream);
	}
    }
}

// Largely cribbed from CFSocket.c; find a run loop where our source is scheduled and wake it up.  We skip the runloop cycling, so we
// are likely to signal the same run loop over and over again.  Don't know if we should worry about that.
static void _wakeUpRunLoop(struct _CFStream *stream) {
    CFRunLoopRef rl = NULL;
    SInt32 idx, cnt;
    CFArrayRef rlArray;
    if (!stream->client || !stream->client->runLoopsAndModes) return;
    rlArray = stream->client->runLoopsAndModes;
    cnt = CFArrayGetCount(rlArray);
    if (cnt == 0) return;
    if (cnt == 2) {
        rl = (CFRunLoopRef)CFArrayGetValueAtIndex(rlArray, 0);
    } else {
        rl = (CFRunLoopRef)CFArrayGetValueAtIndex(rlArray, 0);
        for (idx = 2; NULL != rl && idx < cnt; idx+=2) {
            CFRunLoopRef value = (CFRunLoopRef)CFArrayGetValueAtIndex(rlArray, idx);
            if (value != rl) rl = NULL;
        }
        if (NULL == rl) {	/* more than one different rl, so we must pick one */
            for (idx = 0; idx < cnt; idx+=2) {
                CFRunLoopRef value = (CFRunLoopRef)CFArrayGetValueAtIndex(rlArray, idx);
                CFStringRef currentMode = CFRunLoopCopyCurrentMode(value);
                if (NULL != currentMode && CFEqual(currentMode, CFArrayGetValueAtIndex(rlArray, idx+1)) && CFRunLoopIsWaiting(value)) {
                    CFRelease(currentMode);
                    rl = value;
                    break;
                }
                if (NULL != currentMode) CFRelease(currentMode);
            }
            if (NULL == rl) {	/* didn't choose one above, so choose first */
                rl = (CFRunLoopRef)CFArrayGetValueAtIndex(rlArray, 0);
            }
        }
    }
    if (NULL != rl && CFRunLoopIsWaiting(rl)) CFRunLoopWakeUp(rl);
}

__private_extern__ void _CFStreamSignalEvent(struct _CFStream *stream, CFStreamEventType event, CFErrorRef error, Boolean synchronousAllowed) {
    // Update our internal status; we must use the primitive __CFStreamGetStatus(), because CFStreamGetStatus() calls us, and we can end up in an infinite loop.
    CFStreamStatus status = __CFStreamGetStatus(stream);

    // Sanity check the event
    if (status == kCFStreamStatusNotOpen) {
        // No events allowed; this is almost certainly a bug in the stream's implementation
        CFLog(__kCFLogAssertion, CFSTR("Stream %p is sending an event before being opened"), stream);
        event = 0;
    } else if (status == kCFStreamStatusClosed || status == kCFStreamStatusError) {
        // no further events are allowed
        event = 0;
    } else if (status == kCFStreamStatusAtEnd) {
        // Only error events are allowed
        event &= kCFStreamEventErrorOccurred;
    } else if (status != kCFStreamStatusOpening) {
        // cannot send open completed; that happened already
        event &= ~kCFStreamEventOpenCompleted;
    }
    
    // Change status if appropriate
    if (event & kCFStreamEventOpenCompleted && status == kCFStreamStatusOpening) {
        _CFStreamSetStatusCode(stream, kCFStreamStatusOpen);
    }
    if (event & kCFStreamEventEndEncountered && status < kCFStreamStatusAtEnd) {
        _CFStreamSetStatusCode(stream, kCFStreamStatusAtEnd);
    }
    if (event & kCFStreamEventErrorOccurred) {
        if (_CFStreamGetCallBackPtr(stream)->version < 2) {
            _CFStreamSetStreamError(stream, (CFStreamError *)error);
        } else {
            CFAssert(error, __kCFLogAssertion, "CFStream: kCFStreamEventErrorOccurred signalled, but error is NULL!");
            CFRetain(error);
            if (stream->error) CFRelease(stream->error);
            stream->error = error;
        }
        _CFStreamSetStatusCode(stream, kCFStreamStatusError);
    }

    // Now signal any pertinent event 
    if (stream->client && (stream->client->when & event) != 0) {
        CFRunLoopSourceRef source = _CFStreamCopySource(stream);
        
        if (source) {
            Boolean signalNow = FALSE;
        
            stream->client->whatToSignal |= event;
        
            if (synchronousAllowed && !__CFBitIsSet(stream->flags, CALLING_CLIENT)) {
            
                CFRunLoopRef rl = CFRunLoopGetCurrent();
                CFStringRef mode = CFRunLoopCopyCurrentMode(rl);
            
                if (mode) {
                    if (CFRunLoopContainsSource(rl, source, mode))
                        signalNow = TRUE;
                }
                if (mode) 
                    CFRelease(mode);
            }

            if (signalNow) {
                // Can call out safely right now
                _cfstream_solo_signalEventSync(stream);
            } else {
                // Schedule for later delivery
                if (source) {
                    CFRunLoopSourceSignal(source);
                }
                _wakeUpRunLoop(stream);
            }
		
			CFRelease(source);
        }
    }
}

__private_extern__ CFStreamStatus _CFStreamGetStatus(struct _CFStream *stream) {
  CFStreamStatus status = __CFStreamGetStatus(stream);
  // Status code just represents the value when last we checked; if we were in the middle of doing work at that time, we need to find out if the work has completed, now.  If we find out about a status change, we need to inform the client as well, so we call _CFStreamSignalEvent.  This will take care of updating our internal status correctly, too.
  __CFBitSet(stream->flags, CALLING_CLIENT);
  if (status == kCFStreamStatusOpening) {
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    if (cb->openCompleted) {
      Boolean isComplete;
      if (cb->version < 2) {
	CFStreamError err = {0, 0};
	isComplete = ((_CFStreamCBOpenCompletedV1)(cb->openCompleted))(stream, &err, _CFStreamGetInfoPointer(stream));
	if (err.error != 0) _CFStreamSetStreamError(stream, &err);
      } else {
	isComplete = cb->openCompleted(stream, &(stream->error), _CFStreamGetInfoPointer(stream));
      }
      if (isComplete) {
	if (!stream->error) {
	  status = kCFStreamStatusOpen;
	} else {
	  status = kCFStreamStatusError;
	}
	_CFStreamSetStatusCode(stream, status);
	if (status == kCFStreamStatusOpen) {
	  _CFStreamScheduleEvent(stream, kCFStreamEventOpenCompleted);
	} else {
	  _CFStreamScheduleEvent(stream, kCFStreamEventErrorOccurred);
	}
      }
    }
  }
  __CFBitClear(stream->flags, CALLING_CLIENT);
  return status;
}

CF_EXPORT CFStreamStatus CFReadStreamGetStatus(CFReadStreamRef stream) {
    CF_OBJC_FUNCDISPATCH0(__kCFReadStreamTypeID, CFStreamStatus, stream, "streamStatus");
    return _CFStreamGetStatus((struct _CFStream *)stream);
}

CF_EXPORT CFStreamStatus CFWriteStreamGetStatus(CFWriteStreamRef stream) {
    CF_OBJC_FUNCDISPATCH0(__kCFWriteStreamTypeID, CFStreamStatus, stream, "streamStatus");
    return _CFStreamGetStatus((struct _CFStream *)stream);
}

static CFStreamError _CFStreamGetStreamError(struct _CFStream *stream) {
    CFStreamError result;
    if (!stream->error) {
        result.error = 0;
        result.domain = 0;
    } else if (_CFStreamGetCallBackPtr(stream)->version < 2) {
        CFStreamError *streamError = (CFStreamError *)(stream->error);
        result.error = streamError->error;
        result.domain = streamError->domain;
    } else {
        result = _CFStreamErrorFromError(stream->error);
    }
    return result;
}

CF_EXPORT CFStreamError CFReadStreamGetError(CFReadStreamRef stream) {
    CF_OBJC_FUNCDISPATCH0(__kCFReadStreamTypeID, CFStreamError, stream, "_cfStreamError");
    return _CFStreamGetStreamError((struct _CFStream *)stream);
}

CF_EXPORT CFStreamError CFWriteStreamGetError(CFWriteStreamRef stream) {
    CF_OBJC_FUNCDISPATCH0(__kCFWriteStreamTypeID, CFStreamError, stream, "_cfStreamError");
    return _CFStreamGetStreamError((struct _CFStream *)stream);
}

static CFErrorRef _CFStreamCopyError(struct _CFStream *stream) {
    if (!stream->error) {
        return NULL;
    } else if (_CFStreamGetCallBackPtr(stream)->version < 2) {
        return _CFErrorFromStreamError(CFGetAllocator(stream), (CFStreamError *)(stream->error));
    } else {
        CFRetain(stream->error);
        return stream->error;
    }
}

CF_EXPORT CFErrorRef CFReadStreamCopyError(CFReadStreamRef stream) {
    CF_OBJC_FUNCDISPATCH0(__kCFReadStreamTypeID, CFErrorRef, stream, "streamError");
    return _CFStreamCopyError((struct _CFStream *)stream);
}

CF_EXPORT CFErrorRef CFWriteStreamCopyError(CFWriteStreamRef stream) {
    return _CFStreamCopyError((struct _CFStream *)stream);
    CF_OBJC_FUNCDISPATCH0(__kCFWriteStreamTypeID, CFErrorRef, stream, "streamError");
}

__private_extern__ Boolean _CFStreamOpen(struct _CFStream *stream) {
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    Boolean success, openComplete;
    if (_CFStreamGetStatus(stream) != kCFStreamStatusNotOpen) {
        return FALSE;
    }
    __CFBitSet(stream->flags, CALLING_CLIENT);
    _CFStreamSetStatusCode(stream, kCFStreamStatusOpening);
    if (cb->open) {
        if (cb->version < 2) {
            CFStreamError err = {0, 0};
            success = ((_CFStreamCBOpenV1)(cb->open))(stream, &err, &openComplete, _CFStreamGetInfoPointer(stream));
            if (err.error != 0) _CFStreamSetStreamError(stream, &err);
        } else {
            success = cb->open(stream, &(stream->error), &openComplete, _CFStreamGetInfoPointer(stream));
        }
    } else {
        success = TRUE;
        openComplete = TRUE;
    }
    if (openComplete) {
        if (success) {
            // 2957690 - Guard against the possibility that the stream has already signalled itself in to a later state (like AtEnd)
            if (__CFStreamGetStatus(stream) == kCFStreamStatusOpening) {
                _CFStreamSetStatusCode(stream, kCFStreamStatusOpen);
            }
            _CFStreamScheduleEvent(stream, kCFStreamEventOpenCompleted);
        } else {
#if DEPLOYMENT_TARGET_WINDOWS
            _CFStreamClose(stream);
#endif
            _CFStreamSetStatusCode(stream, kCFStreamStatusError);
            _CFStreamScheduleEvent(stream, kCFStreamEventErrorOccurred);
        }
    }
    __CFBitClear(stream->flags, CALLING_CLIENT);
    return success;
}

CF_EXPORT Boolean CFReadStreamOpen(CFReadStreamRef stream) {
    if(CF_IS_OBJC(__kCFReadStreamTypeID, stream)) {
        CF_OBJC_VOIDCALL0(stream, "open");
        return TRUE;
    }
    return _CFStreamOpen((struct _CFStream *)stream);
}

CF_EXPORT Boolean CFWriteStreamOpen(CFWriteStreamRef stream) {
    if(CF_IS_OBJC(__kCFWriteStreamTypeID, stream)) {
        CF_OBJC_VOIDCALL0(stream, "open");
        return TRUE;
    }
    return _CFStreamOpen((struct _CFStream *)stream);
}

CF_EXPORT void CFReadStreamClose(CFReadStreamRef stream) {
    CF_OBJC_FUNCDISPATCH0(__kCFReadStreamTypeID, void, stream, "close");
    _CFStreamClose((struct _CFStream *)stream);
}

CF_EXPORT void CFWriteStreamClose(CFWriteStreamRef stream) {
    CF_OBJC_FUNCDISPATCH0(__kCFWriteStreamTypeID, void, stream, "close");
    _CFStreamClose((struct _CFStream *)stream);
}

CF_EXPORT Boolean CFReadStreamHasBytesAvailable(CFReadStreamRef readStream) {
    CF_OBJC_FUNCDISPATCH0(__kCFReadStreamTypeID, Boolean, readStream, "hasBytesAvailable");
    struct _CFStream *stream = (struct _CFStream *)readStream;
    CFStreamStatus status = _CFStreamGetStatus(stream);
    const struct _CFStreamCallBacks *cb;
    if (status != kCFStreamStatusOpen && status != kCFStreamStatusReading) {
        return FALSE;
    } 
    cb  = _CFStreamGetCallBackPtr(stream);
    if (cb->canRead == NULL) {
        return TRUE;  // No way to know without trying....
    } else {
        Boolean result;
        __CFBitSet(stream->flags, CALLING_CLIENT);
        if (cb->version < 2) {
            result = ((_CFStreamCBCanReadV1)(cb->canRead))((CFReadStreamRef)stream, _CFStreamGetInfoPointer(stream));
        } else {
            result = cb->canRead((CFReadStreamRef)stream, &(stream->error), _CFStreamGetInfoPointer(stream));
            if (stream->error) {
                _CFStreamSetStatusCode(stream, kCFStreamStatusError);
                _CFStreamScheduleEvent(stream, kCFStreamEventErrorOccurred);
            }
        }
        __CFBitClear(stream->flags, CALLING_CLIENT);
        return result;
    }
}

static void waitForOpen(struct _CFStream *stream);
CFIndex CFReadStreamRead(CFReadStreamRef readStream, UInt8 *buffer, CFIndex bufferLength) {
    CF_OBJC_FUNCDISPATCH2(__kCFReadStreamTypeID, CFIndex, readStream, "read:maxLength:", buffer, bufferLength);
    struct _CFStream *stream = (struct _CFStream *)readStream;
    CFStreamStatus status = _CFStreamGetStatus(stream);
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    if (status == kCFStreamStatusOpening) {
        __CFBitSet(stream->flags, CALLING_CLIENT);
        waitForOpen(stream);
        __CFBitClear(stream->flags, CALLING_CLIENT);
        status = _CFStreamGetStatus(stream);
    }
        
    if (status != kCFStreamStatusOpen && status != kCFStreamStatusReading && status != kCFStreamStatusAtEnd) {
        return -1;
    } else  if (status == kCFStreamStatusAtEnd) {
        return 0;
    } else {
        Boolean atEOF;
        CFIndex bytesRead;
        __CFBitSet(stream->flags, CALLING_CLIENT);
        if (stream->client) {
            stream->client->whatToSignal &= ~kCFStreamEventHasBytesAvailable;
        }
        _CFStreamSetStatusCode(stream, kCFStreamStatusReading);
        if (cb->version < 2) {
            CFStreamError err = {0, 0};
            bytesRead = ((_CFStreamCBReadV1)(cb->read))((CFReadStreamRef)stream, buffer, bufferLength, &err, &atEOF, _CFStreamGetInfoPointer(stream));
            if (err.error != 0) _CFStreamSetStreamError(stream, &err);
        } else {
            bytesRead = cb->read((CFReadStreamRef)stream, buffer, bufferLength, &(stream->error), &atEOF, _CFStreamGetInfoPointer(stream));
        }
        if (stream->error) {
            bytesRead = -1;
            _CFStreamSetStatusCode(stream, kCFStreamStatusError);
            _CFStreamScheduleEvent(stream, kCFStreamEventErrorOccurred);
        } else if (atEOF) {
            _CFStreamSetStatusCode(stream, kCFStreamStatusAtEnd);
            _CFStreamScheduleEvent(stream, kCFStreamEventEndEncountered);
        } else {
            _CFStreamSetStatusCode(stream, kCFStreamStatusOpen);
        }
        __CFBitClear(stream->flags, CALLING_CLIENT);
        return bytesRead;
    }
}

CF_EXPORT const UInt8 *CFReadStreamGetBuffer(CFReadStreamRef readStream, CFIndex maxBytesToRead, CFIndex *numBytesRead) {
    if (CF_IS_OBJC(__kCFReadStreamTypeID, readStream)) {
        uint8_t *bufPtr = NULL;
        Boolean gotBytes;
        CF_OBJC_CALL2(Boolean, gotBytes, readStream, "getBuffer:length:", &bufPtr, numBytesRead); 
        if(gotBytes) {
            return (const UInt8 *)bufPtr;
        } else {
            return NULL;
        }
    }
    struct _CFStream *stream = (struct _CFStream *)readStream;
    CFStreamStatus status = _CFStreamGetStatus(stream);
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    const UInt8 *buffer;
    if (status == kCFStreamStatusOpening) {
        __CFBitSet(stream->flags, CALLING_CLIENT);
        waitForOpen(stream);
        __CFBitClear(stream->flags, CALLING_CLIENT);
        status = _CFStreamGetStatus(stream);
    }
    if (status != kCFStreamStatusOpen && status != kCFStreamStatusReading && status != kCFStreamStatusAtEnd) {
        *numBytesRead = -1;
        buffer = NULL;
    } else  if (status == kCFStreamStatusAtEnd || cb->getBuffer == NULL) {
        *numBytesRead = 0;
        buffer = NULL;
    } else {
        Boolean atEOF;
        Boolean hadBytes = stream->client && (stream->client->whatToSignal & kCFStreamEventHasBytesAvailable);
        __CFBitSet(stream->flags, CALLING_CLIENT);
        if (hadBytes) {
            stream->client->whatToSignal &= ~kCFStreamEventHasBytesAvailable;
        }
        _CFStreamSetStatusCode(stream, kCFStreamStatusReading);
        if (cb->version < 2) {
            CFStreamError err = {0, 0};
            buffer = ((_CFStreamCBGetBufferV1)(cb->getBuffer))((CFReadStreamRef)stream, maxBytesToRead, numBytesRead, &err, &atEOF, _CFStreamGetInfoPointer(stream));
            if (err.error != 0) _CFStreamSetStreamError(stream, &err);
        } else {
            buffer = cb->getBuffer((CFReadStreamRef)stream, maxBytesToRead, numBytesRead, &(stream->error), &atEOF, _CFStreamGetInfoPointer(stream));
        }
        if (stream->error) {
            *numBytesRead = -1;
            _CFStreamSetStatusCode(stream, kCFStreamStatusError);
            buffer = NULL;
            _CFStreamScheduleEvent(stream, kCFStreamEventErrorOccurred);
        } else if (atEOF) {
            _CFStreamSetStatusCode(stream, kCFStreamStatusAtEnd);
            _CFStreamScheduleEvent(stream, kCFStreamEventEndEncountered);
        } else {
            if (!buffer && hadBytes) {
                stream->client->whatToSignal |= kCFStreamEventHasBytesAvailable;
            }
            _CFStreamSetStatusCode(stream, kCFStreamStatusOpen);
        }
        __CFBitClear(stream->flags, CALLING_CLIENT);
    }
    return buffer;
}

CF_EXPORT Boolean CFWriteStreamCanAcceptBytes(CFWriteStreamRef writeStream) {
    CF_OBJC_FUNCDISPATCH0(__kCFWriteStreamTypeID, Boolean, writeStream, "hasSpaceAvailable");
    struct _CFStream *stream = (struct _CFStream *)writeStream;
    CFStreamStatus status = _CFStreamGetStatus(stream);
    const struct _CFStreamCallBacks *cb;
    if (status != kCFStreamStatusOpen && status != kCFStreamStatusWriting) {
        return FALSE;
    } 
    cb  = _CFStreamGetCallBackPtr(stream);
    if (cb->canWrite == NULL) {
        return TRUE;  // No way to know without trying....
    } else {
        Boolean result;
        __CFBitSet(stream->flags, CALLING_CLIENT);
        if (cb->version < 2) {
            result = ((_CFStreamCBCanWriteV1)(cb->canWrite))((CFWriteStreamRef)stream, _CFStreamGetInfoPointer(stream));
        } else {
            result = cb->canWrite((CFWriteStreamRef)stream, &(stream->error), _CFStreamGetInfoPointer(stream));
            if (stream->error) {
                _CFStreamSetStatusCode(stream, kCFStreamStatusError);
                _CFStreamScheduleEvent(stream, kCFStreamEventErrorOccurred);
            }
        }
        __CFBitClear(stream->flags, CALLING_CLIENT);
        return result;
    }
}

CF_EXPORT CFIndex CFWriteStreamWrite(CFWriteStreamRef writeStream, const UInt8 *buffer, CFIndex bufferLength) {
    CF_OBJC_FUNCDISPATCH2(__kCFWriteStreamTypeID, CFIndex, writeStream, "write:maxLength:", buffer, bufferLength);
    struct _CFStream *stream = (struct _CFStream *)writeStream;
    CFStreamStatus status = _CFStreamGetStatus(stream);
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    if (status == kCFStreamStatusOpening) {
        __CFBitSet(stream->flags, CALLING_CLIENT);
        waitForOpen(stream);
        __CFBitClear(stream->flags, CALLING_CLIENT);
        status = _CFStreamGetStatus(stream);
    }
    if (status != kCFStreamStatusOpen && status != kCFStreamStatusWriting) {
        return -1;
    } else {
        CFIndex result;
        __CFBitSet(stream->flags, CALLING_CLIENT);
        _CFStreamSetStatusCode(stream, kCFStreamStatusWriting);
        if (stream->client) {
            stream->client->whatToSignal &= ~kCFStreamEventCanAcceptBytes;
        }
        if (cb->version < 2) {
            CFStreamError err = {0, 0};
            result = ((_CFStreamCBWriteV1)(cb->write))((CFWriteStreamRef)stream, buffer, bufferLength, &err, _CFStreamGetInfoPointer(stream));
            if (err.error) _CFStreamSetStreamError(stream, &err);
        } else {
            result = cb->write((CFWriteStreamRef)stream, buffer, bufferLength, &(stream->error), _CFStreamGetInfoPointer(stream));
        }
        if (stream->error) {
            _CFStreamSetStatusCode(stream, kCFStreamStatusError);
            _CFStreamScheduleEvent(stream, kCFStreamEventErrorOccurred);
        } else if (result == 0) {
            _CFStreamSetStatusCode(stream, kCFStreamStatusAtEnd);
            _CFStreamScheduleEvent(stream, kCFStreamEventEndEncountered);
        } else {
            _CFStreamSetStatusCode(stream, kCFStreamStatusOpen);
        }
        __CFBitClear(stream->flags, CALLING_CLIENT);
        return result;
    }
}

__private_extern__ CFTypeRef _CFStreamCopyProperty(struct _CFStream *stream, CFStringRef propertyName) {
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    if (cb->copyProperty == NULL) {
        return NULL;
    } else {
        CFTypeRef result;
        __CFBitSet(stream->flags, CALLING_CLIENT);
        result = cb->copyProperty(stream, propertyName, _CFStreamGetInfoPointer(stream));
        __CFBitClear(stream->flags, CALLING_CLIENT);
        return result;
    }
}

CF_EXPORT CFTypeRef CFReadStreamCopyProperty(CFReadStreamRef stream, CFStringRef propertyName) {
    CF_OBJC_FUNCDISPATCH1(__kCFReadStreamTypeID, CFTypeRef, stream, "propertyForKey:", propertyName);
    return _CFStreamCopyProperty((struct _CFStream *)stream, propertyName);
}

CF_EXPORT CFTypeRef CFWriteStreamCopyProperty(CFWriteStreamRef stream, CFStringRef propertyName) {
    CF_OBJC_FUNCDISPATCH1(__kCFWriteStreamTypeID, CFTypeRef, stream, "propertyForKey:", propertyName);
    return _CFStreamCopyProperty((struct _CFStream *)stream, propertyName);
}

__private_extern__ Boolean _CFStreamSetProperty(struct _CFStream *stream, CFStringRef prop, CFTypeRef val) {
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    if (cb->setProperty == NULL) {
        return FALSE;
    } else {
        Boolean result;
        __CFBitSet(stream->flags, CALLING_CLIENT);
        result = cb->setProperty(stream, prop, val, _CFStreamGetInfoPointer(stream));
        __CFBitClear(stream->flags, CALLING_CLIENT);
        return result;
    }
}

CF_EXPORT
Boolean CFReadStreamSetProperty(CFReadStreamRef stream, CFStringRef propertyName, CFTypeRef propertyValue) {
    CF_OBJC_FUNCDISPATCH2(__kCFReadStreamTypeID, Boolean, stream, "setProperty:forKey:", propertyValue, propertyName);
    return _CFStreamSetProperty((struct _CFStream *)stream, propertyName, propertyValue);
}

CF_EXPORT
Boolean CFWriteStreamSetProperty(CFWriteStreamRef stream, CFStringRef propertyName, CFTypeRef propertyValue) {
    CF_OBJC_FUNCDISPATCH2(__kCFWriteStreamTypeID, Boolean, stream, "setProperty:forKey:", propertyValue, propertyName);
    return _CFStreamSetProperty((struct _CFStream *)stream, propertyName, propertyValue);
}

static void _initializeClient(struct _CFStream *stream) {
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    if (!cb->schedule) return; // Do we wish to allow this?
    stream->client = (struct _CFStreamClient *)CFAllocatorAllocate(CFGetAllocator(stream), sizeof(struct _CFStreamClient), 0);
    memset(stream->client, 0, sizeof(struct _CFStreamClient));
}

/* If we add a setClient callback to the concrete stream callbacks, we must set/clear CALLING_CLIENT around it */
__private_extern__ Boolean _CFStreamSetClient(struct _CFStream *stream, CFOptionFlags streamEvents, void (*clientCB)(struct _CFStream *, CFStreamEventType, void *), CFStreamClientContext *clientCallBackContext) {

    Boolean removingClient = (streamEvents == kCFStreamEventNone || clientCB == NULL || clientCallBackContext == NULL);

    if (removingClient) {
        clientCB = NULL;
        streamEvents = kCFStreamEventNone;
        clientCallBackContext = NULL;
    }
    if (!stream->client) {
        if (removingClient) {
            // We have no client now, and we've been asked to add none???
            return TRUE;
        }
        _initializeClient(stream);
        if (!stream->client) {
            // Asynch not supported
            return FALSE;
        }
    }
    if (stream->client->cb && stream->client->cbContext.release) {
        stream->client->cbContext.release(stream->client->cbContext.info);
    }
    stream->client->cb = clientCB;
    if (clientCallBackContext) {
        stream->client->cbContext.version = clientCallBackContext->version;
        stream->client->cbContext.retain = clientCallBackContext->retain;
        stream->client->cbContext.release = clientCallBackContext->release;
        stream->client->cbContext.copyDescription = clientCallBackContext->copyDescription;
        stream->client->cbContext.info = (clientCallBackContext->retain && clientCallBackContext->info) ? clientCallBackContext->retain(clientCallBackContext->info) : clientCallBackContext->info;
    } else {
        stream->client->cbContext.retain = NULL;
        stream->client->cbContext.release = NULL;
        stream->client->cbContext.copyDescription = NULL;
        stream->client->cbContext.info = NULL;
    }
    if (stream->client->when != streamEvents) {
        const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
        stream->client->when = streamEvents;
        if (cb->requestEvents) {
            cb->requestEvents(stream, streamEvents, _CFStreamGetInfoPointer(stream));
        }
    }
    return TRUE;
}

CF_EXPORT Boolean CFReadStreamSetClient(CFReadStreamRef readStream, CFOptionFlags streamEvents, CFReadStreamClientCallBack clientCB, CFStreamClientContext *clientContext) {
    CF_OBJC_FUNCDISPATCH3(__kCFReadStreamTypeID, Boolean, readStream, "_setCFClientFlags:callback:context:", streamEvents, clientCB, clientContext);
    streamEvents &= ~kCFStreamEventCanAcceptBytes;
    return _CFStreamSetClient((struct _CFStream *)readStream, streamEvents, (void (*)(struct _CFStream *, CFStreamEventType, void *))clientCB, clientContext);
}

CF_EXPORT Boolean CFWriteStreamSetClient(CFWriteStreamRef writeStream, CFOptionFlags streamEvents, CFWriteStreamClientCallBack clientCB, CFStreamClientContext *clientContext) {
    CF_OBJC_FUNCDISPATCH3(__kCFWriteStreamTypeID, Boolean, writeStream, "_setCFClientFlags:callback:context:", streamEvents, clientCB, clientContext);
    streamEvents &= ~kCFStreamEventHasBytesAvailable;
    return _CFStreamSetClient((struct _CFStream *)writeStream, streamEvents, (void (*)(struct _CFStream *, CFStreamEventType, void *))clientCB, clientContext);
}

CF_INLINE void *_CFStreamGetClient(struct _CFStream *stream) {
    if (stream->client) return stream->client->cbContext.info;
    else return NULL;
}

CF_EXPORT void *_CFReadStreamGetClient(CFReadStreamRef readStream) {
    return _CFStreamGetClient((struct _CFStream *)readStream);
}

CF_EXPORT void *_CFWriteStreamGetClient(CFWriteStreamRef writeStream) {
    return _CFStreamGetClient((struct _CFStream *)writeStream);
}


__private_extern__ void _CFStreamScheduleWithRunLoop(struct _CFStream *stream, CFRunLoopRef runLoop, CFStringRef runLoopMode) {
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    
    if (! stream->client) {
        _initializeClient(stream);
        if (!stream->client) return; // we don't support asynch.
    }
    
    if (! stream->client->rlSource) {
	/* No source, so we join the shared source group */
        CFTypeRef a[] = { runLoop, runLoopMode };
        
        CFArrayRef runLoopAndSourceKey = CFArrayCreate(kCFAllocatorSystemDefault, a, sizeof(a) / sizeof(a[0]), &kCFTypeArrayCallBacks);

        __CFSpinLock(&sSourceLock);
        
        if (!sSharedSources)
            sSharedSources = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        
        CFMutableArrayRef listOfStreamsSharingASource = (CFMutableArrayRef)CFDictionaryGetValue(sSharedSources, runLoopAndSourceKey);
        if (listOfStreamsSharingASource) {
			struct _CFStream* aStream = (struct _CFStream*) CFArrayGetValueAtIndex(listOfStreamsSharingASource, 0);
			CFRunLoopSourceRef source = _CFStreamCopySource(aStream);
			if (source) {
				_CFStreamSetSource(stream, source, FALSE);
				CFRelease(source);
			}
            CFRetain(listOfStreamsSharingASource);
        }
        else {
            CFRunLoopSourceContext ctxt = {
                0,
                NULL,
                CFRetain,
                CFRelease,
                (CFStringRef(*)(const void *))CFCopyDescription,
                NULL,
                NULL,
                NULL,
                NULL,
                (void(*)(void *))_cfstream_shared_signalEventSync
            };

            listOfStreamsSharingASource = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
            CFDictionaryAddValue(sSharedSources, runLoopAndSourceKey, listOfStreamsSharingASource);

            ctxt.info = listOfStreamsSharingASource;
            
			CFRunLoopSourceRef source = CFRunLoopSourceCreate(kCFAllocatorSystemDefault, 0, &ctxt);
			_CFStreamSetSource(stream, source, FALSE);
            CFRunLoopAddSource(runLoop, source, runLoopMode);
			CFRelease(source);
        }
        
        CFArrayAppendValue(listOfStreamsSharingASource, stream);
        CFDictionaryAddValue(sSharedSources, stream, runLoopAndSourceKey);
        
        CFRelease(runLoopAndSourceKey);
        CFRelease(listOfStreamsSharingASource);
        
        __CFBitSet(stream->flags, SHARED_SOURCE);

        __CFSpinUnlock(&sSourceLock);
    }
    else if (__CFBitIsSet(stream->flags, SHARED_SOURCE)) {
        /* We were sharing, but now we'll get our own source */
	
        CFArrayRef runLoopAndSourceKey;
        CFMutableArrayRef listOfStreamsSharingASource;
        CFIndex count, i;
        
        CFAllocatorRef alloc = CFGetAllocator(stream);
        CFRunLoopSourceContext ctxt = {
            0,
            (void *)stream,
            NULL,														// Do not use CFRetain/CFRelease callbacks here; that will cause a retain loop 
            NULL,														// Do not use CFRetain/CFRelease callbacks here; that will cause a retain loop
            (CFStringRef(*)(const void *))CFCopyDescription,
            NULL,
            NULL,
            NULL,
            NULL,
            (void(*)(void *))_cfstream_solo_signalEventSync
        };

        __CFSpinLock(&sSourceLock);
        
        runLoopAndSourceKey = (CFArrayRef)CFRetain((CFTypeRef)CFDictionaryGetValue(sSharedSources, stream));
        listOfStreamsSharingASource = (CFMutableArrayRef)CFDictionaryGetValue(sSharedSources, runLoopAndSourceKey);
        
        count = CFArrayGetCount(listOfStreamsSharingASource);
        i = CFArrayGetFirstIndexOfValue(listOfStreamsSharingASource, CFRangeMake(0, count), stream);
        if (i != kCFNotFound) {
            CFArrayRemoveValueAtIndex(listOfStreamsSharingASource, i);
            count--;
        }
        
        if (count == 0) {
			CFRunLoopSourceRef source = _CFStreamCopySource(stream);
			if (source) {
				CFRunLoopRemoveSource((CFRunLoopRef)CFArrayGetValueAtIndex(runLoopAndSourceKey, 0), source, (CFStringRef)CFArrayGetValueAtIndex(runLoopAndSourceKey, 1));
				CFRelease(source);
			}
            CFDictionaryRemoveValue(sSharedSources, runLoopAndSourceKey);
        }
        
        CFDictionaryRemoveValue(sSharedSources, stream);
        
		_CFStreamSetSource(stream, NULL, count == 0);
		
        __CFBitClear(stream->flags, SHARED_SOURCE);

        __CFSpinUnlock(&sSourceLock);
        
		CFRunLoopSourceRef source = CFRunLoopSourceCreate(alloc, 0, &ctxt);
		_CFStreamSetSource(stream, source, FALSE);
        CFRunLoopAddSource((CFRunLoopRef)CFArrayGetValueAtIndex(runLoopAndSourceKey, 0), source, (CFStringRef)CFArrayGetValueAtIndex(runLoopAndSourceKey, 1));
        CFRelease(runLoopAndSourceKey);
        
        CFRunLoopAddSource(runLoop, source, runLoopMode);
		
		CFRelease(source);
    } else {
	/* We're not sharing, so just add the source to the rl & mode */
		CFRunLoopSourceRef source = _CFStreamCopySource(stream);
		if (source) {
			CFRunLoopAddSource(runLoop, source, runLoopMode);
			CFRelease(source);
		}
    }
    
    if (!stream->client->runLoopsAndModes) {
        stream->client->runLoopsAndModes = CFArrayCreateMutable(CFGetAllocator(stream), 0, &kCFTypeArrayCallBacks);
    }
    CFArrayAppendValue(stream->client->runLoopsAndModes, runLoop);
    CFArrayAppendValue(stream->client->runLoopsAndModes, runLoopMode);
    
    if (cb->schedule) {
        __CFBitSet(stream->flags, CALLING_CLIENT);
        cb->schedule(stream, runLoop, runLoopMode, _CFStreamGetInfoPointer(stream));
        __CFBitClear(stream->flags, CALLING_CLIENT);
    }
    
    /*
     * If we've got events pending, we need to wake up and signal
     */
    if (stream->client && stream->client->whatToSignal != 0) {
		CFRunLoopSourceRef source = _CFStreamCopySource(stream);
		if (source) {
			CFRunLoopSourceSignal(source);
			CFRelease(source);
            _wakeUpRunLoop(stream);
        }
    }
}

CF_EXPORT void CFReadStreamScheduleWithRunLoop(CFReadStreamRef stream, CFRunLoopRef runLoop, CFStringRef runLoopMode) {
    CF_OBJC_FUNCDISPATCH2(__kCFReadStreamTypeID, void, stream, "_scheduleInCFRunLoop:forMode:", runLoop, runLoopMode);
    _CFStreamScheduleWithRunLoop((struct _CFStream *)stream, runLoop, runLoopMode);
}

CF_EXPORT void CFWriteStreamScheduleWithRunLoop(CFWriteStreamRef stream, CFRunLoopRef runLoop, CFStringRef runLoopMode) {
    CF_OBJC_FUNCDISPATCH2(__kCFWriteStreamTypeID, void, stream, "_scheduleInCFRunLoop:forMode:", runLoop, runLoopMode);
    _CFStreamScheduleWithRunLoop((struct _CFStream *)stream, runLoop, runLoopMode);
}


__private_extern__ void _CFStreamUnscheduleFromRunLoop(struct _CFStream *stream, CFRunLoopRef runLoop, CFStringRef runLoopMode) {
    const struct _CFStreamCallBacks *cb = _CFStreamGetCallBackPtr(stream);
    if (!stream->client) return;
    if (!stream->client->rlSource) return;
    
    if (!__CFBitIsSet(stream->flags, SHARED_SOURCE)) {
		CFRunLoopSourceRef source = _CFStreamCopySource(stream);
		if (source) {
			CFRunLoopRemoveSource(runLoop, source, runLoopMode);
			CFRelease(source);
		}
    } else {
        CFArrayRef runLoopAndSourceKey;
        CFMutableArrayRef list;
        CFIndex count, i;

        __CFSpinLock(&sSourceLock);
        
        runLoopAndSourceKey = (CFArrayRef)CFDictionaryGetValue(sSharedSources, stream);
        list = (CFMutableArrayRef)CFDictionaryGetValue(sSharedSources, runLoopAndSourceKey);
        
        count = CFArrayGetCount(list);
        i = CFArrayGetFirstIndexOfValue(list, CFRangeMake(0, count), stream);
        if (i != kCFNotFound) {
            CFArrayRemoveValueAtIndex(list, i);
            count--;
        }
        
        if (count == 0) {
			CFRunLoopSourceRef source = _CFStreamCopySource(stream);
			if (source) {
				CFRunLoopRemoveSource(runLoop, source, runLoopMode);
				CFRelease(source);
			}
            CFDictionaryRemoveValue(sSharedSources, runLoopAndSourceKey);
        }
        
        CFDictionaryRemoveValue(sSharedSources, stream);
        
        _CFStreamSetSource(stream, NULL, count == 0);

        __CFBitClear(stream->flags, SHARED_SOURCE);

        __CFSpinUnlock(&sSourceLock);
    }
    
    _CFStreamRemoveRunLoopAndModeFromArray(stream->client->runLoopsAndModes, runLoop, runLoopMode);

    if (cb->unschedule) {
        cb->unschedule(stream, runLoop, runLoopMode, _CFStreamGetInfoPointer(stream));
    }
}

CF_EXPORT void CFReadStreamUnscheduleFromRunLoop(CFReadStreamRef stream, CFRunLoopRef runLoop, CFStringRef runLoopMode) {
    CF_OBJC_FUNCDISPATCH2(__kCFReadStreamTypeID, void, stream, "_unscheduleFromCFRunLoop:forMode:", runLoop, runLoopMode);
    _CFStreamUnscheduleFromRunLoop((struct _CFStream *)stream, runLoop, runLoopMode);
}

void CFWriteStreamUnscheduleFromRunLoop(CFWriteStreamRef stream, CFRunLoopRef runLoop, CFStringRef runLoopMode) {
    CF_OBJC_FUNCDISPATCH2(__kCFWriteStreamTypeID, void, stream, "_unscheduleFromCFRunLoop:forMode:", runLoop, runLoopMode);
    _CFStreamUnscheduleFromRunLoop((struct _CFStream *)stream, runLoop, runLoopMode);
}

static void waitForOpen(struct _CFStream *stream) {
    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    CFStringRef privateMode = CFSTR("_kCFStreamBlockingOpenMode");
    _CFStreamScheduleWithRunLoop(stream, runLoop, privateMode);
    // We cannot call _CFStreamGetStatus, because that tries to set/clear CALLING_CLIENT, which should be set around this entire call (we're within a call from the client).  This should be o.k., because we're running the run loop, so our status code should be being updated in a timely fashion....
    while (__CFStreamGetStatus(stream) == kCFStreamStatusOpening) {
        CFRunLoopRunInMode(privateMode, 1e+20, TRUE);
    }
    _CFStreamUnscheduleFromRunLoop(stream, runLoop, privateMode);
}

CF_INLINE CFArrayRef _CFStreamGetRunLoopsAndModes(struct _CFStream *stream) {
    if (stream->client) return stream->client->runLoopsAndModes;
    else return NULL;
}

CF_EXPORT CFArrayRef _CFReadStreamGetRunLoopsAndModes(CFReadStreamRef readStream) {
    return _CFStreamGetRunLoopsAndModes((struct _CFStream *)readStream);
}

CF_EXPORT CFArrayRef _CFWriteStreamGetRunLoopsAndModes(CFWriteStreamRef writeStream) {
    return _CFStreamGetRunLoopsAndModes((struct _CFStream *)writeStream);
}

CF_EXPORT void CFReadStreamSignalEvent(CFReadStreamRef stream, CFStreamEventType event, const void *error) {
    _CFStreamSignalEvent((struct _CFStream *)stream, event, (CFErrorRef)error, TRUE);
}

CF_EXPORT void CFWriteStreamSignalEvent(CFWriteStreamRef stream, CFStreamEventType event, const void *error) {
    _CFStreamSignalEvent((struct _CFStream *)stream, event, (CFErrorRef)error, TRUE);
}

CF_EXPORT void _CFReadStreamSignalEventDelayed(CFReadStreamRef stream, CFStreamEventType event, const void *error) {
    _CFStreamSignalEvent((struct _CFStream *)stream, event, (CFErrorRef)error, FALSE);
}

CF_EXPORT void _CFReadStreamClearEvent(CFReadStreamRef readStream, CFStreamEventType event) {
    struct _CFStream *stream = (struct _CFStream *)readStream;
    if (stream->client) {
        stream->client->whatToSignal &= ~event;
    }
}

CF_EXPORT void _CFWriteStreamSignalEventDelayed(CFWriteStreamRef stream, CFStreamEventType event, const void *error) {
    _CFStreamSignalEvent((struct _CFStream *)stream, event, (CFErrorRef)error, FALSE);
}

CF_EXPORT void *CFReadStreamGetInfoPointer(CFReadStreamRef stream) {
    return _CFStreamGetInfoPointer((struct _CFStream *)stream);
}

CF_EXPORT void *CFWriteStreamGetInfoPointer(CFWriteStreamRef stream) {
    return _CFStreamGetInfoPointer((struct _CFStream *)stream);
}

/* CF_EXPORT */
void _CFStreamSourceScheduleWithRunLoop(CFRunLoopSourceRef source, CFMutableArrayRef runLoopsAndModes, CFRunLoopRef runLoop, CFStringRef runLoopMode)
{
    CFIndex count;
    CFRange range;
    
    count = CFArrayGetCount(runLoopsAndModes);
    range = CFRangeMake(0, count);
    
    while (range.length) {
        
        CFIndex i = CFArrayGetFirstIndexOfValue(runLoopsAndModes, range, runLoop);
        
        if (i == kCFNotFound)
            break;
            
        if (CFEqual(CFArrayGetValueAtIndex(runLoopsAndModes, i + 1), runLoopMode))
            return;
        
        range.location = i + 2;
        range.length = count - range.location;
    }

	// Add the new values.
    CFArrayAppendValue(runLoopsAndModes, runLoop);
    CFArrayAppendValue(runLoopsAndModes, runLoopMode);
    
	// Schedule the source on the new loop and mode.
    if (source)
        CFRunLoopAddSource(runLoop, source, runLoopMode);
}


/* CF_EXPORT */
void _CFStreamSourceUnscheduleFromRunLoop(CFRunLoopSourceRef source, CFMutableArrayRef runLoopsAndModes, CFRunLoopRef runLoop, CFStringRef runLoopMode)
{
    CFIndex count;
	CFRange range;
    
    count = CFArrayGetCount(runLoopsAndModes);
    range = CFRangeMake(0, count);
	
	while (range.length) {
    
		CFIndex i = CFArrayGetFirstIndexOfValue(runLoopsAndModes, range, runLoop);
		
		// If not found, it's not scheduled on it.
		if (i == kCFNotFound)
			return;
			
		// Make sure it is scheduled in this mode.
		if (CFEqual(CFArrayGetValueAtIndex(runLoopsAndModes, i + 1), runLoopMode)) {

			// Remove mode and runloop from the list.
            CFArrayReplaceValues(runLoopsAndModes, CFRangeMake(i, 2), NULL, 0);
            
            // Remove it from the runloop.
            if (source)
                CFRunLoopRemoveSource(runLoop, source, runLoopMode);
			
			return;
		}
		
        range.location = i + 2;
        range.length = count - range.location;
	}
}


/* CF_EXPORT */
void _CFStreamSourceScheduleWithAllRunLoops(CFRunLoopSourceRef source, CFArrayRef runLoopsAndModes)
{
    CFIndex i, count = CFArrayGetCount(runLoopsAndModes);

    if (!source)
        return;

    for (i = 0; i < count; i += 2) {

        // Make sure it's scheduled on all the right loops and modes.
        // Go through the array adding the source to all loops and modes.
        CFRunLoopAddSource((CFRunLoopRef)CFArrayGetValueAtIndex(runLoopsAndModes, i),
                            source,
                            (CFStringRef)CFArrayGetValueAtIndex(runLoopsAndModes, i + 1));
	}
}


/* CF_EXPORT */
void _CFStreamSourceUncheduleFromAllRunLoops(CFRunLoopSourceRef source, CFArrayRef runLoopsAndModes)
{
    CFIndex i, count = CFArrayGetCount(runLoopsAndModes);

    if (!source)
        return;

    for (i = 0; i < count; i += 2) {

        // Go through the array removing the source from all loops and modes.
        CFRunLoopRemoveSource((CFRunLoopRef)CFArrayGetValueAtIndex(runLoopsAndModes, i),
                              source,
                              (CFStringRef)CFArrayGetValueAtIndex(runLoopsAndModes, i + 1));
	}
}

Boolean _CFStreamRemoveRunLoopAndModeFromArray(CFMutableArrayRef runLoopsAndModes, CFRunLoopRef rl, CFStringRef mode) {
    CFIndex idx, cnt;
    Boolean found = FALSE;
    
    if (!runLoopsAndModes) return FALSE;

    cnt = CFArrayGetCount(runLoopsAndModes);
    for (idx = 0; idx + 1 < cnt; idx += 2) {
        if (CFEqual(CFArrayGetValueAtIndex(runLoopsAndModes, idx), rl) && CFEqual(CFArrayGetValueAtIndex(runLoopsAndModes, idx + 1), mode)) {
            CFArrayRemoveValueAtIndex(runLoopsAndModes, idx);
            CFArrayRemoveValueAtIndex(runLoopsAndModes, idx);
            found = TRUE;
            break;
        }
    }
    return found;
}

// Used by NSStream to properly allocate the bridged objects
CF_EXPORT CFIndex _CFStreamInstanceSize(void) {
    return sizeof(struct _CFStream);
}

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
#elif DEPLOYMENT_TARGET_WINDOWS
void __CFStreamCleanup(void) {
    __CFSpinLock(&sSourceLock);
    if (sSharedSources) {
        CFIndex count = CFDictionaryGetCount(sSharedSources);
        if (count == 0) {
            // Only release if empty.  If it's still holding streams (which would be a client
            // bug leak), freeing this dict would free the streams, which then need to access the
            // dict to remove themselves, which leads to a deadlock.
            CFRelease(sSharedSources);
            sSharedSources = NULL;
        } else {
            const void ** keys = (const void **)malloc(sizeof(const void *) * count);
#if defined(DEBUG)
            int i;
#endif
            CFDictionaryGetKeysAndValues(sSharedSources, keys, NULL);
             fprintf(stderr, "*** CFNetwork is shutting down, but %ld streams are still scheduled.\n", count);
#if defined(DEBUG)
            for (i = 0; i < count;i ++) {
                if ((CFGetTypeID(keys[i]) == __kCFReadStreamTypeID) || (CFGetTypeID(keys[i]) == __kCFWriteStreamTypeID)) {
                    CFShow(keys[i]);
                }
            }
#endif
        }
    }
    __CFSpinUnlock(&sSourceLock);
}
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif

