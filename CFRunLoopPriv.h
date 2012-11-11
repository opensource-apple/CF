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
/*      CFRunLoopPriv.h
        Copyright (c) 2006-2007, Apple Inc. All rights reserved.
*/

#if (DEPLOYMENT_TARGET_MACOSX || 0)

/* -------- -------- -------- -------- -------- -------- -------- -------- */

#import "CFObject.h"
#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFMachPort.h>
#include <CoreFoundation/CFSocket.h>
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFBag.h>
#include <mach/mach.h>
#include <sys/event.h>
#include <pthread.h>

@class CFRunLoopSourceSet;
typedef CFRunLoopSourceContext CFRunLoopSourceContext0;
typedef CFMachPortContext CFRunLoopMachPortContext;
typedef CFSocketContext CFRunLoopSocketContext;

/* -------- -------- -------- -------- -------- -------- -------- -------- */

@interface CFRunLoopSource : CFObject {
    uint8_t _cfruntime_[8]; // large enough for 32-bit or 64-bit
    int _kq;
    mach_port_t _portset;
    mach_port_t _port;
    CFIndex _order;
    uint8_t _invalid;
    uint8_t _firing;
    uint8_t _ownsPort;
    uint8_t _async;
}

- (int)kqueue;
- (mach_port_t)machPortSet;
- (mach_port_t)machPort;

- (void)setOrder:(CFIndex)o;
- (CFIndex)order;

- (void)setAsyncStrategy:(uint8_t)s;
- (uint8_t)asyncStrategy;

- (void)perform:(mach_msg_header_t *)msg;

- (void)invalidate;
- (Boolean)isValid;

- (void)noteAddedToSourceSet:(CFRunLoopSourceSet *)ss;
- (void)noteRemovedFromSourceSet:(CFRunLoopSourceSet *)ss;

- (CFStringRef)copyPartialDebugDescription; // subclasses override
- (CFStringRef)copyDebugDescription;

@end

@interface CFRunLoopVersion0SourceCFRef : CFRunLoopSource {
    CFRunLoopSourceContext0 _context;
}

- (Boolean)setContext:(CFRunLoopSourceContext0)c;
- (CFRunLoopSourceContext0)context;

- (void)markReady;
- (void)handle;

- (void)scheduleInRunLoop:(CFRunLoopRef)rl mode:(CFStringRef)n;
- (void)cancelFromRunLoop:(CFRunLoopRef)rl mode:(CFStringRef)n;

@end

@interface CFRunLoopVersion1SourceCFRef : CFRunLoopSource {
    CFRunLoopSourceContext1 _context;
}

- (Boolean)setContext:(CFRunLoopSourceContext1)c;
- (CFRunLoopSourceContext1)context;

- (void)markReady;
- (void)handle:(mach_msg_header_t *)msg;

@end

/* -------- -------- -------- -------- -------- -------- -------- -------- */

@interface CFRunLoopTimerSource : CFRunLoopSource {
    CFAbsoluteTime _fireAT;
    CFTimeInterval _interval;
}

- (void)setFireTime:(CFAbsoluteTime)at;
- (CFAbsoluteTime)fireTime;

- (void)setInterval:(CFTimeInterval)i;
- (CFTimeInterval)interval;

- (void)handle;

@end

@interface CFRunLoopTimerSourceCFRef : CFRunLoopTimerSource {
    void *_function;
    CFRunLoopTimerContext _context;
}

- (void)setFunction:(void *)f;
- (void *)function;

- (Boolean)setContext:(CFRunLoopTimerContext)c;
- (CFRunLoopTimerContext)context;

@end

/* -------- -------- -------- -------- -------- -------- -------- -------- */

@interface CFRunLoopMachPortSource : CFRunLoopSource {
    mach_port_t _notifyPort;
    mach_port_t _oldNotifyPort;
}

+ (id)newWithPort:(mach_port_t)p;

- (void)handle:(mach_msg_header_t *)msg;

@end

@interface CFRunLoopMachPortSourceCFRef : CFRunLoopMachPortSource {
    void *_function;
    CFRunLoopMachPortContext _context;
    void *_invalidation;
}

- (void)setFunction:(void *)f;
- (void *)function;

- (Boolean)setContext:(CFRunLoopMachPortContext)c;
- (CFRunLoopMachPortContext)context;

- (void)setInvalidationFunction:(void *)f;
- (void *)invalidationFunction;

@end

/* -------- -------- -------- -------- -------- -------- -------- -------- */

@interface CFRunLoopKEventSource : CFRunLoopSource {
    struct kevent _filter;
}

- (Boolean)setFilter:(struct kevent)kev;
- (struct kevent)filter;

- (void)handle:(struct kevent *)kev;

@end

/* -------- -------- -------- -------- -------- -------- -------- -------- */

@interface CFRunLoopSignalSource : CFRunLoopSource {
    int _signal;
}

- (Boolean)setSignal:(int)sig;
- (int)signal;

- (long)poll;

- (void)handle:(long)n;

@end

/* -------- -------- -------- -------- -------- -------- -------- -------- */

@interface CFRunLoopProcessDeathSource : CFRunLoopSource {
    int _pid;
}

- (Boolean)setProcessID:(int)pid;
- (int)processID;

- (void)handle;

@end

/* -------- -------- -------- -------- -------- -------- -------- -------- */

@interface CFRunLoopSocketSource : CFRunLoopSource {
    int _socket;
    uint8_t _ownsSocket;
    uint8_t _enabledR;
    uint8_t _enabledW;
}

+ (id)newWithSocket:(int)s;

- (int)socket;
- (CFDataRef)copyLocalAddress;
- (CFDataRef)copyRemoteAddress;

- (void)setReadEventsEnabled:(Boolean)b;
- (Boolean)readEventsEnabled;

- (void)setWriteEventsEnabled:(Boolean)b;
- (Boolean)writeEventsEnabled;

- (void)handleReadability:(CFIndex)amt endOfFile:(Boolean)b;
- (void)performRead:(struct kevent *)kev;

- (void)handleWritability:(CFIndex)amt endOfFile:(Boolean)b;
- (void)performWrite:(struct kevent *)kev;

@end

@interface CFRunLoopSocketSourceCFRef : CFRunLoopSocketSource {
    void *_function;
    CFRunLoopSocketContext _context;
    uint8_t _callbacks;
    uint8_t _flags;
    uint8_t _disabled;
}

- (void)setFunction:(void *)f;
- (void *)function;

- (Boolean)setContext:(CFRunLoopSocketContext)c;
- (CFRunLoopSocketContext)context;

- (void)setCallBackTypes:(uint8_t)f;
- (uint8_t)callBackTypes;

- (void)setFlags:(uint8_t)f;
- (uint8_t)flags;

- (void)setDisabledFlags:(uint8_t)f;
- (uint8_t)disabledFlags;

- (Boolean)handleAcceptError:(int)err;
- (Boolean)handleReadError:(int)err;

@end

/* -------- -------- -------- -------- -------- -------- -------- -------- */

@interface CFRunLoopObserver : CFRunLoopSource {
}

- (void)observeEntry:(CFRunLoopSourceSet *)ss;
- (void)observeBeforeWaiting:(CFRunLoopSourceSet *)ss;
- (void)observeAfterWaiting:(CFRunLoopSourceSet *)ss;
- (void)observeExit:(CFRunLoopSourceSet *)ss;

@end

@interface CFRunLoopObserverCFRef : CFRunLoopObserver {
    void *_function;
    CFRunLoopObserverContext _context;
    CFOptionFlags _activities;
    uint8_t _oneshot;
}

- (void)setFunction:(void *)f;
- (void *)function;

- (Boolean)setContext:(CFRunLoopObserverContext)c;
- (CFRunLoopObserverContext)context;

- (void)setActivities:(CFOptionFlags)a;
- (CFOptionFlags)activities;

- (void)setOneshot:(Boolean)b;
- (Boolean)oneshot;

@end

/* -------- -------- -------- -------- -------- -------- -------- -------- */

@interface CFRunLoopSourceSet : CFRunLoopObserver {
    CFMutableBagRef _sources;
    CFStringRef _name;
    pthread_t _thread;
    CFMutableArrayRef _observers[4];
    CFRunLoopTimerSource *_timeoutTimer;
    CFRunLoopMachPortSource *_wakeupPort;
    uint8_t _stopped;
    uint8_t _waiting;
}

+ (void)removeSourceFromAllSets:(CFRunLoopSource *)src;

- (void)setName:(CFStringRef)n;
- (CFStringRef)name;

- (void)setAffineThread:(pthread_t)t;
- (pthread_t)affineThread;

- (Boolean)containsObserver:(CFRunLoopObserver *)o;
- (void)addObserver:(CFRunLoopObserver *)o activities:(CFRunLoopActivity)a;
- (void)removeObserver:(CFRunLoopObserver *)o activities:(CFRunLoopActivity)a;

- (Boolean)containsSource:(CFRunLoopSource *)src;
- (void)addSource:(CFRunLoopSource *)src;
- (void)removeSource:(CFRunLoopSource *)src;

- (void)forEachSource:(Boolean (*)(CFRunLoopSource *, void *))f context:(void *)c;

- (void)stop;
- (void)wakeup;
- (Boolean)isWaiting;
- (Boolean)isEmpty;
- (Boolean)hasInputAvailable;

- (int32_t)serviceUntil:(CFAbsoluteTime)at handleOne:(Boolean)handleOne;

@end

/* -------- -------- -------- -------- -------- -------- -------- -------- */

@interface CFRunLoopCFRef : CFObject {
    uint8_t _cfruntime_[8]; // large enough for 32-bit or 64-bit
    pthread_t _thread;
    void *_counterpart;
    CFMutableArrayRef _sourceSets;
    CFRunLoopSourceSet *_currentSet;
    uint8_t _invalid;
}

- (void)setAffineThread:(pthread_t)t;
- (pthread_t)affineThread;

- (void)setCounterpart:(void *)c;
- (void *)counterpart;

- (void)invalidate;
- (Boolean)isValid;

- (CFArrayRef)copySourceSets;

- (void)setCurrentSourceSet:(CFRunLoopSourceSet *)ss;
- (CFRunLoopSourceSet *)currentSourceSet;

- (CFRunLoopSourceSet *)lookupSourceSetWithName:(CFStringRef)n;
- (CFRunLoopSourceSet *)lookupOrCreateSourceSetWithName:(CFStringRef)n;

- (CFStringRef)copyDebugDescription;

@end

/* -------- -------- -------- -------- -------- -------- -------- -------- */

#endif

