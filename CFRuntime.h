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
/*	CFRuntime.h
	Copyright (c) 1999-2009, Apple Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_CFRUNTIME__)
#define __COREFOUNDATION_CFRUNTIME__ 1

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFDictionary.h>
#include <stddef.h>

CF_EXTERN_C_BEGIN

// GC: until we link against ObjC must use indirect functions.  Overridden in CFSetupFoundationBridging
CF_EXPORT bool kCFUseCollectableAllocator;
CF_EXPORT bool (*__CFObjCIsCollectable)(void *);

// GC: primitives.
// is GC on?
#define CF_USING_COLLECTABLE_MEMORY (kCFUseCollectableAllocator)
// is GC on and is this the GC allocator?
#define CF_IS_COLLECTABLE_ALLOCATOR(allocator) (kCFUseCollectableAllocator && (NULL == (allocator) || kCFAllocatorSystemDefault == (allocator)))
// is this allocated by the collector?
#define CF_IS_COLLECTABLE(obj) (__CFObjCIsCollectable ? __CFObjCIsCollectable((void*)obj) : false)


enum {
    _kCFRuntimeNotATypeID =                0,
    _kCFRuntimeScannedObject =       (1UL << 0),
    /* _kCFRuntimeUncollectableObject = (1UL << 1),  No longer used; obsolete. */
    _kCFRuntimeResourcefulObject =   (1UL << 2)
};

typedef struct __CFRuntimeClass {	// Version 0 struct
    CFIndex version;
    const char *className;
    void (*init)(CFTypeRef cf);
    CFTypeRef (*copy)(CFAllocatorRef allocator, CFTypeRef cf);
#if MAC_OS_X_VERSION_10_2 <= MAC_OS_X_VERSION_MAX_ALLOWED
    void (*finalize)(CFTypeRef cf);
#else
    void (*dealloc)(CFTypeRef cf);
#endif
    Boolean (*equal)(CFTypeRef cf1, CFTypeRef cf2);
    CFHashCode (*hash)(CFTypeRef cf);
    CFStringRef (*copyFormattingDesc)(CFTypeRef cf, CFDictionaryRef formatOptions);	// str with retain
    CFStringRef (*copyDebugDesc)(CFTypeRef cf);	// str with retain
#if MAC_OS_X_VERSION_10_5 <= MAC_OS_X_VERSION_MAX_ALLOWED
#define CF_RECLAIM_AVAILABLE 1
    void (*reclaim)(CFTypeRef cf);
#endif
} CFRuntimeClass;

#define RADAR_5115468_FIXED 1

/* Note that CF runtime class registration and unregistration is not currently
 * thread-safe, which should not currently be a problem, as long as unregistration
 * is done only when valid to do so.
 */

CF_EXPORT CFTypeID _CFRuntimeRegisterClass(const CFRuntimeClass * const cls);
	/* Registers a new class with the CF runtime.  Pass in a
	 * pointer to a CFRuntimeClass structure.  The pointer is
	 * remembered by the CF runtime -- the structure is NOT
	 * copied.
	 *
	 * - version field must be zero currently.
	 * - className field points to a null-terminated C string
	 *   containing only ASCII (0 - 127) characters; this field
	 *   may NOT be NULL.
	 * - init field points to a function which classes can use to
	 *   apply some generic initialization to instances as they
	 *   are created; this function is called by both
	 *   _CFRuntimeCreateInstance and _CFRuntimeInitInstance; if
	 *   this field is NULL, no function is called; the instance
	 *   has been initialized enough that the polymorphic funcs
	 *   CFGetTypeID(), CFRetain(), CFRelease(), CFGetRetainCount(),
	 *   and CFGetAllocator() are valid on it when the init
	 *   function if any is called.
	 * - finalize field points to a function which destroys an
	 *   instance when the retain count has fallen to zero; if
	 *   this is NULL, finalization does nothing. Note that if
	 *   the class-specific functions which create or initialize
	 *   instances more fully decide that a half-initialized
	 *   instance must be destroyed, the finalize function for
	 *   that class has to be able to deal with half-initialized
	 *   instances.  The finalize function should NOT destroy the
	 *   memory for the instance itself; that is done by the
	 *   CF runtime after this finalize callout returns.
	 * - equal field points to an equality-testing function; this
	 *   field may be NULL, in which case only pointer/reference
	 *   equality is performed on instances of this class. 
	 *   Pointer equality is tested, and the type IDs are checked
	 *   for equality, before this function is called (so, the
	 *   two instances are not pointer-equal but are of the same
	 *   class before this function is called).
	 * NOTE: the equal function must implement an immutable
	 *   equality relation, satisfying the reflexive, symmetric,
	 *    and transitive properties, and remains the same across
	 *   time and immutable operations (that is, if equal(A,B) at
	 *   some point, then later equal(A,B) provided neither
	 *   A or B has been mutated).
	 * - hash field points to a hash-code-computing function for
	 *   instances of this class; this field may be NULL in which
	 *   case the pointer value of an instance is converted into
	 *   a hash.
	 * NOTE: the hash function and equal function must satisfy
	 *   the relationship "equal(A,B) implies hash(A) == hash(B)";
	 *   that is, if two instances are equal, their hash codes must
	 *   be equal too. (However, the converse is not true!)
	 * - copyFormattingDesc field points to a function returning a
	 *   CFStringRef with a human-readable description of the
	 *   instance; if this is NULL, the type does not have special
	 *   human-readable string-formats.
	 * - copyDebugDesc field points to a function returning a
	 *   CFStringRef with a debugging description of the instance;
	 *   if this is NULL, a simple description is generated.
	 *
	 * This function returns _kCFRuntimeNotATypeID on failure, or
	 * on success, returns the CFTypeID for the new class.  This
	 * CFTypeID is what the class uses to allocate or initialize
	 * instances of the class. It is also returned from the
	 * conventional *GetTypeID() function, which returns the
	 * class's CFTypeID so that clients can compare the
	 * CFTypeID of instances with that of a class.
	 *
	 * The function to compute a human-readable string is very
	 * optional, and is really only interesting for classes,
	 * like strings or numbers, where it makes sense to format
	 * the instance using just its contents.
	 */

CF_EXPORT const CFRuntimeClass * _CFRuntimeGetClassWithTypeID(CFTypeID typeID);
	/* Returns the pointer to the CFRuntimeClass which was
	 * assigned the specified CFTypeID.
	 */

CF_EXPORT void _CFRuntimeUnregisterClassWithTypeID(CFTypeID typeID);
	/* Unregisters the class with the given type ID.  It is
	 * undefined whether type IDs are reused or not (expect
	 * that they will be).
	 *
	 * Whether or not unregistering the class is a good idea or
	 * not is not CF's responsibility.  In particular you must
	 * be quite sure all instances are gone, and there are no
	 * valid weak refs to such in other threads.
	 */

/* All CF "instances" start with this structure.  Never refer to
 * these fields directly -- they are for CF's use and may be added
 * to or removed or change format without warning.  Binary
 * compatibility for uses of this struct is not guaranteed from
 * release to release.
 */
typedef struct __CFRuntimeBase {
    uintptr_t _cfisa;
    uint8_t _cfinfo[4];
#if __LP64__
    uint32_t _rc;
#endif
} CFRuntimeBase;

#if __BIG_ENDIAN__
#define INIT_CFRUNTIME_BASE(...) {0, {0, 0, 0, 0x80}}
#else
#define INIT_CFRUNTIME_BASE(...) {0, {0x80, 0, 0, 0}}
#endif

CF_EXPORT CFTypeRef _CFRuntimeCreateInstance(CFAllocatorRef allocator, CFTypeID typeID, CFIndex extraBytes, unsigned char *category);
	/* Creates a new CF instance of the class specified by the
	 * given CFTypeID, using the given allocator, and returns it. 
	 * If the allocator returns NULL, this function returns NULL.
	 * A CFRuntimeBase structure is initialized at the beginning
	 * of the returned instance.  extraBytes is the additional
	 * number of bytes to allocate for the instance (BEYOND that
	 * needed for the CFRuntimeBase).  If the specified CFTypeID
	 * is unknown to the CF runtime, this function returns NULL.
	 * No part of the new memory other than base header is
	 * initialized (the extra bytes are not zeroed, for example).
	 * All instances created with this function must be destroyed
	 * only through use of the CFRelease() function -- instances
	 * must not be destroyed by using CFAllocatorDeallocate()
	 * directly, even in the initialization or creation functions
	 * of a class.  Pass NULL for the category parameter.
	 */

CF_EXPORT void _CFRuntimeSetInstanceTypeID(CFTypeRef cf, CFTypeID typeID);
	/* This function changes the typeID of the given instance.
	 * If the specified CFTypeID is unknown to the CF runtime,
	 * this function does nothing.  This function CANNOT be used
	 * to initialize an instance.  It is for advanced usages such
	 * as faulting.
	 */

CF_EXPORT void _CFRuntimeInitStaticInstance(void *memory, CFTypeID typeID);
	/* This function initializes a memory block to be a constant
	 * (unreleaseable) CF object of the given typeID.
	 * If the specified CFTypeID is unknown to the CF runtime,
	 * this function does nothing.  The memory block should
	 * be a chunk of in-binary writeable static memory, and at
	 * least as large as sizeof(CFRuntimeBase) on the platform
	 * the code is being compiled for.  The init function of the
	 * CFRuntimeClass is invoked on the memory as well, if the
	 * class has one.
	 */
#define CF_HAS_INIT_STATIC_INSTANCE 1

#if 0
// ========================= EXAMPLE =========================

// Example: EXRange -- a "range" object, which keeps the starting
//       location and length of the range. ("EX" as in "EXample").

// ---- API ----

typedef const struct __EXRange * EXRangeRef;

CFTypeID EXRangeGetTypeID(void);

EXRangeRef EXRangeCreate(CFAllocatorRef allocator, uint32_t location, uint32_t length);

uint32_t EXRangeGetLocation(EXRangeRef rangeref);
uint32_t EXRangeGetLength(EXRangeRef rangeref);


// ---- implementation ----

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFString.h>

struct __EXRange {
    CFRuntimeBase _base;
    uint32_t _location;
    uint32_t _length;
};

static Boolean __EXRangeEqual(CFTypeRef cf1, CFTypeRef cf2) {
    EXRangeRef rangeref1 = (EXRangeRef)cf1;
    EXRangeRef rangeref2 = (EXRangeRef)cf2;
    if (rangeref1->_location != rangeref2->_location) return false;
    if (rangeref1->_length != rangeref2->_length) return false;
    return true;
}

static CFHashCode __EXRangeHash(CFTypeRef cf) {
    EXRangeRef rangeref = (EXRangeRef)cf;
    return (CFHashCode)(rangeref->_location + rangeref->_length);
}

static CFStringRef __EXRangeCopyFormattingDesc(CFTypeRef cf, CFDictionaryRef formatOpts) {
    EXRangeRef rangeref = (EXRangeRef)cf;
    return CFStringCreateWithFormat(CFGetAllocator(rangeref), formatOpts,
		CFSTR("[%u, %u)"),
		rangeref->_location,
		rangeref->_location + rangeref->_length);
}

static CFStringRef __EXRangeCopyDebugDesc(CFTypeRef cf) {
    EXRangeRef rangeref = (EXRangeRef)cf;
    return CFStringCreateWithFormat(CFGetAllocator(rangeref), NULL,
		CFSTR("<EXRange %p [%p]>{loc = %u, len = %u}"),
		rangeref,
		CFGetAllocator(rangeref),
		rangeref->_location,
		rangeref->_length);
}

static void __EXRangeEXRangeFinalize(CFTypeRef cf) {
    EXRangeRef rangeref = (EXRangeRef)cf;
    // nothing to finalize
}

static CFTypeID _kEXRangeID = _kCFRuntimeNotATypeID;

static CFRuntimeClass _kEXRangeClass = {0};

/* Something external to this file is assumed to call this
 * before the EXRange class is used.
 */
void __EXRangeClassInitialize(void) {
    _kEXRangeClass.version = 0;
    _kEXRangeClass.className = "EXRange";
    _kEXRangeClass.init = NULL;
    _kEXRangeClass.copy = NULL;
    _kEXRangeClass.finalize = __EXRangeEXRangeFinalize;
    _kEXRangeClass.equal = __EXRangeEqual;
    _kEXRangeClass.hash = __EXRangeHash;
    _kEXRangeClass.copyFormattingDesc = __EXRangeCopyFormattingDesc;
    _kEXRangeClass.copyDebugDesc = __EXRangeCopyDebugDesc;
    _kEXRangeID = _CFRuntimeRegisterClass((const CFRuntimeClass * const)&_kEXRangeClass);
}

CFTypeID EXRangeGetTypeID(void) {
    return _kEXRangeID;
}

EXRangeRef EXRangeCreate(CFAllocatorRef allocator, uint32_t location, uint32_t length) {
    struct __EXRange *newrange;
    uint32_t extra = sizeof(struct __EXRange) - sizeof(CFRuntimeBase);
    newrange = (struct __EXRange *)_CFRuntimeCreateInstance(allocator, _kEXRangeID, extra, NULL);
    if (NULL == newrange) {
	return NULL;
    }
    newrange->_location = location;
    newrange->_length = length;
    return (EXRangeRef)newrange;
}

uint32_t EXRangeGetLocation(EXRangeRef rangeref) {
    return rangeref->_location;
}

uint32_t EXRangeGetLength(EXRangeRef rangeref) {
    return rangeref->_length;
}

#endif

CF_EXTERN_C_END

#endif /* ! __COREFOUNDATION_CFRUNTIME__ */
