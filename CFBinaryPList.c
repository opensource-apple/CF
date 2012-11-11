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
/*	CFBinaryPList.c
	Copyright 2000-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/


#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFDate.h>
#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFSet.h>
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFByteOrder.h>
#include <CoreFoundation/CFRuntime.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include "CFInternal.h"

typedef struct {
    int64_t high;
    uint64_t low;
} CFSInt128Struct;

enum {
    kCFNumberSInt128Type = 17
};

extern CFNumberType _CFNumberGetType2(CFNumberRef number);

enum {
	CF_NO_ERROR = 0,
	CF_OVERFLOW_ERROR = (1 << 0),
};

CF_INLINE uint32_t __check_uint32_add_unsigned_unsigned(uint32_t x, uint32_t y, int32_t* err) {
   if((UINT_MAX - y) < x)
        *err = *err | CF_OVERFLOW_ERROR;
   return x + y;
};

CF_INLINE uint64_t __check_uint64_add_unsigned_unsigned(uint64_t x, uint64_t y, int32_t* err) {
   if((ULLONG_MAX - y) < x)
        *err = *err | CF_OVERFLOW_ERROR;
   return x + y;
};

CF_INLINE uint32_t __check_uint32_mul_unsigned_unsigned(uint32_t x, uint32_t y, int32_t* err) {
   uint64_t tmp = (uint64_t) x * (uint64_t) y;
   /* If any of the upper 32 bits touched, overflow */
   if(tmp & 0xffffffff00000000ULL)
        *err = *err | CF_OVERFLOW_ERROR;
   return (uint32_t) tmp;
};

CF_INLINE uint64_t __check_uint64_mul_unsigned_unsigned(uint64_t x, uint64_t y, int32_t* err) {
  if(x == 0) return 0;
  if(ULLONG_MAX/x < y)
     *err = *err | CF_OVERFLOW_ERROR;
  return x * y;
};

#if __LP64__
#define check_ptr_add(p, a, err)	(const uint8_t *)__check_uint64_add_unsigned_unsigned((uintptr_t)p, (uintptr_t)a, err)
#define check_size_t_mul(b, a, err)	(size_t)__check_uint64_mul_unsigned_unsigned((size_t)b, (size_t)a, err)
#else
#define check_ptr_add(p, a, err)	(const uint8_t *)__check_uint32_add_unsigned_unsigned((uintptr_t)p, (uintptr_t)a, err)
#define check_size_t_mul(b, a, err)	(size_t)__check_uint32_mul_unsigned_unsigned((size_t)b, (size_t)a, err)
#endif


CF_INLINE CFTypeID __CFGenericTypeID_genericobj_inline(const void *cf) {
    CFTypeID typeID = (*(uint32_t *)(((CFRuntimeBase *)cf)->_cfinfo) >> 8) & 0xFFFF;
    return CF_IS_OBJC(typeID, cf) ? CFGetTypeID(cf) : typeID;
}

struct __CFKeyedArchiverUID {
    CFRuntimeBase _base;
    uint32_t _value;
};

static CFStringRef __CFKeyedArchiverUIDCopyDescription(CFTypeRef cf) {
    CFKeyedArchiverUIDRef uid = (CFKeyedArchiverUIDRef)cf;
    return CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFKeyedArchiverUID %p [%p]>{value = %u}"), cf, CFGetAllocator(cf), uid->_value);
}

static CFStringRef __CFKeyedArchiverUIDCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    CFKeyedArchiverUIDRef uid = (CFKeyedArchiverUIDRef)cf;
    return CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("@%u@"), uid->_value);
}

static CFTypeID __kCFKeyedArchiverUIDTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFKeyedArchiverUIDClass = {
    0,
    "CFKeyedArchiverUID",
    NULL,	// init
    NULL,	// copy
    NULL,	// finalize
    NULL,	// equal -- pointer equality only
    NULL,	// hash -- pointer hashing only
    __CFKeyedArchiverUIDCopyFormattingDescription,
    __CFKeyedArchiverUIDCopyDescription
};

__private_extern__ void __CFKeyedArchiverUIDInitialize(void) {
    __kCFKeyedArchiverUIDTypeID = _CFRuntimeRegisterClass(&__CFKeyedArchiverUIDClass);
}

CFTypeID _CFKeyedArchiverUIDGetTypeID(void) {
    return __kCFKeyedArchiverUIDTypeID;
}

CFKeyedArchiverUIDRef _CFKeyedArchiverUIDCreate(CFAllocatorRef allocator, uint32_t value) {
    CFKeyedArchiverUIDRef uid;
    uid = (CFKeyedArchiverUIDRef)_CFRuntimeCreateInstance(allocator, __kCFKeyedArchiverUIDTypeID, sizeof(struct __CFKeyedArchiverUID) - sizeof(CFRuntimeBase), NULL);
    if (NULL == uid) {
	return NULL;
    }
    ((struct __CFKeyedArchiverUID *)uid)->_value = value;
    return uid;
}


uint32_t _CFKeyedArchiverUIDGetValue(CFKeyedArchiverUIDRef uid) {
    return uid->_value;
}


typedef struct {
    CFTypeRef stream;
    CFTypeRef error;
    uint64_t written;
    int32_t used;
    bool streamIsData;
    uint8_t buffer[8192 - 32];
} __CFBinaryPlistWriteBuffer;

static void writeBytes(__CFBinaryPlistWriteBuffer *buf, const UInt8 *bytes, CFIndex length) {
    if (0 == length) return;
    if (buf->error) return;
    if (buf->streamIsData) {
        CFDataAppendBytes((CFMutableDataRef)buf->stream, bytes, length);
        buf->written += length;
    } else {
        CFAssert(false, __kCFLogAssertion, "Streams are not supported on this platform");
    }
}

static void bufferWrite(__CFBinaryPlistWriteBuffer *buf, const uint8_t *buffer, CFIndex count) {
    if (0 == count) return;
    if ((CFIndex)sizeof(buf->buffer) <= count) {
	writeBytes(buf, buf->buffer, buf->used);
	buf->used = 0;
	writeBytes(buf, buffer, count);
	return;
    }
    CFIndex copyLen = __CFMin(count, (CFIndex)sizeof(buf->buffer) - buf->used);
    memmove(buf->buffer + buf->used, buffer, copyLen);
    buf->used += copyLen;
    if (sizeof(buf->buffer) == buf->used) {
	writeBytes(buf, buf->buffer, sizeof(buf->buffer));
	memmove(buf->buffer, buffer + copyLen, count - copyLen);
	buf->used = count - copyLen;
    }
}

static void bufferFlush(__CFBinaryPlistWriteBuffer *buf) {
    writeBytes(buf, buf->buffer, buf->used);
    buf->used = 0;
}

/*
HEADER
	magic number ("bplist")
	file format version

OBJECT TABLE
	variable-sized objects

	Object Formats (marker byte followed by additional info in some cases)
	null	0000 0000
	bool	0000 1000			// false
	bool	0000 1001			// true
	fill	0000 1111			// fill byte
	int	0001 nnnn	...		// # of bytes is 2^nnnn, big-endian bytes
	real	0010 nnnn	...		// # of bytes is 2^nnnn, big-endian bytes
	date	0011 0011	...		// 8 byte float follows, big-endian bytes
	data	0100 nnnn	[int]	...	// nnnn is number of bytes unless 1111 then int count follows, followed by bytes
	string	0101 nnnn	[int]	...	// ASCII string, nnnn is # of chars, else 1111 then int count, then bytes
	string	0110 nnnn	[int]	...	// Unicode string, nnnn is # of chars, else 1111 then int count, then big-endian 2-byte uint16_t
		0111 xxxx			// unused
	uid	1000 nnnn	...		// nnnn+1 is # of bytes
		1001 xxxx			// unused
	array	1010 nnnn	[int]	objref*	// nnnn is count, unless '1111', then int count follows
		1011 xxxx			// unused
	set	1100 nnnn	[int]	objref* // nnnn is count, unless '1111', then int count follows
	dict	1101 nnnn	[int]	keyref* objref*	// nnnn is count, unless '1111', then int count follows
		1110 xxxx			// unused
		1111 xxxx			// unused

OFFSET TABLE
	list of ints, byte size of which is given in trailer
	-- these are the byte offsets into the file
	-- number of these is in the trailer

TRAILER
	byte size of offset ints in offset table
	byte size of object refs in arrays and dicts
	number of offsets in offset table (also is number of objects)
	element # in offset table which is top level object
	offset table offset

*/


static CFTypeID stringtype = -1, datatype = -1, numbertype = -1, datetype = -1;
static CFTypeID booltype = -1, nulltype = -1, dicttype = -1, arraytype = -1, settype = -1;

static void _appendInt(__CFBinaryPlistWriteBuffer *buf, uint64_t bigint) {
    uint8_t marker;
    uint8_t *bytes;
    CFIndex nbytes;
    if (bigint <= (uint64_t)0xff) {
	nbytes = 1;
	marker = kCFBinaryPlistMarkerInt | 0;
    } else if (bigint <= (uint64_t)0xffff) {
	nbytes = 2;
	marker = kCFBinaryPlistMarkerInt | 1;
    } else if (bigint <= (uint64_t)0xffffffff) {
	nbytes = 4;
	marker = kCFBinaryPlistMarkerInt | 2;
    } else {
	nbytes = 8;
	marker = kCFBinaryPlistMarkerInt | 3;
    }
    bigint = CFSwapInt64HostToBig(bigint);
    bytes = (uint8_t *)&bigint + sizeof(bigint) - nbytes;
    bufferWrite(buf, &marker, 1);
    bufferWrite(buf, bytes, nbytes);
}

static void _appendUID(__CFBinaryPlistWriteBuffer *buf, CFKeyedArchiverUIDRef uid) {
    uint8_t marker;
    uint8_t *bytes;
    CFIndex nbytes;
    uint64_t bigint = _CFKeyedArchiverUIDGetValue(uid);
    if (bigint <= (uint64_t)0xff) {
	nbytes = 1;
    } else if (bigint <= (uint64_t)0xffff) {
	nbytes = 2;
    } else if (bigint <= (uint64_t)0xffffffff) {
	nbytes = 4;
    } else {
	nbytes = 8;
    }
    marker = kCFBinaryPlistMarkerUID | (uint8_t)(nbytes - 1);
    bigint = CFSwapInt64HostToBig(bigint);
    bytes = (uint8_t *)&bigint + sizeof(bigint) - nbytes;
    bufferWrite(buf, &marker, 1);
    bufferWrite(buf, bytes, nbytes);
}

static Boolean __plistNumberEqual(CFTypeRef cf1, CFTypeRef cf2) {
    // As long as this equals function is more restrictive than the
    // existing one, for any given type, the hash function need not
    // also be provided for the uniquing set.
    if (CFNumberIsFloatType((CFNumberRef)cf1) != CFNumberIsFloatType((CFNumberRef)cf2)) return false;
    return CFEqual(cf1, cf2);
}

static CFHashCode __plistDataHash(CFTypeRef cf) {
    CFDataRef data = (CFDataRef)cf;
    return CFHashBytes((UInt8 *)CFDataGetBytePtr(data), __CFMin(CFDataGetLength(data), 1280));
}

static void _flattenPlist(CFPropertyListRef plist, CFMutableArrayRef objlist, CFMutableDictionaryRef objtable, CFMutableSetRef uniquingsets[]) {
    CFPropertyListRef unique;
    uint32_t refnum;
    CFTypeID type = __CFGenericTypeID_genericobj_inline(plist);
    CFIndex idx;
    CFPropertyListRef *list, buffer[256];

    // Do not unique dictionaries or arrays, because: they
    // are slow to compare, and have poor hash codes.
    // Uniquing bools is unnecessary.
    int which = -1;
    if (stringtype == type) {
	which = 0;
    } else if (numbertype == type) {
	which = 1;
    } else if (datetype == type) {
	which = 2;
    } else if (datatype == type) {
	which = 3;
    }
    if (1 && -1 != which) {
	CFMutableSetRef uniquingset = uniquingsets[which];
	CFIndex before = CFSetGetCount(uniquingset);
	CFSetAddValue(uniquingset, plist);
	CFIndex after = CFSetGetCount(uniquingset);
	if (after == before) {	// already in set
	    unique = CFSetGetValue(uniquingset, plist);
	    if (unique != plist) {
		refnum = (uint32_t)(uintptr_t)CFDictionaryGetValue(objtable, unique);
		CFDictionaryAddValue(objtable, plist, (const void *)(uintptr_t)refnum);
	    }
	    return;
	}
    }
    refnum = CFArrayGetCount(objlist);
    CFArrayAppendValue(objlist, plist);
    CFDictionaryAddValue(objtable, plist, (const void *)(uintptr_t)refnum);
    if (dicttype == type) {
	CFIndex count = CFDictionaryGetCount((CFDictionaryRef)plist);
	list = (count <= 128) ? buffer : (CFPropertyListRef *)CFAllocatorAllocate(kCFAllocatorSystemDefault, 2 * count * sizeof(CFTypeRef), 0);
        CFDictionaryGetKeysAndValues((CFDictionaryRef)plist, list, list + count);
        for (idx = 0; idx < 2 * count; idx++) {
            _flattenPlist(list[idx], objlist, objtable, uniquingsets);
        }
        if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
    } else if (arraytype == type) {
	CFIndex count = CFArrayGetCount((CFArrayRef)plist);
	list = (count <= 256) ? buffer : (CFPropertyListRef *)CFAllocatorAllocate(kCFAllocatorSystemDefault, count * sizeof(CFTypeRef), 0);
        CFArrayGetValues((CFArrayRef)plist, CFRangeMake(0, count), list);
        for (idx = 0; idx < count; idx++) {
            _flattenPlist(list[idx], objlist, objtable, uniquingsets);
        }
        if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
    }
}

// stream must be a CFMutableDataRef
CFIndex __CFBinaryPlistWriteToStream(CFPropertyListRef plist, CFTypeRef stream) {
    CFMutableDictionaryRef objtable;
    CFMutableArrayRef objlist;
    CFBinaryPlistTrailer trailer;
    uint64_t *offsets, length_so_far;
    uint64_t mask, refnum;
    int64_t idx, idx2, cnt;
    __CFBinaryPlistWriteBuffer *buf;

    if ((CFTypeID)-1 == stringtype) {
	stringtype = CFStringGetTypeID();
    }
    if ((CFTypeID)-1 == datatype) {
	datatype = CFDataGetTypeID();
    }
    if ((CFTypeID)-1 == numbertype) {
	numbertype = CFNumberGetTypeID();
    }
    if ((CFTypeID)-1 == booltype) {
	booltype = CFBooleanGetTypeID();
    }
    if ((CFTypeID)-1 == datetype) {
	datetype = CFDateGetTypeID();
    }
    if ((CFTypeID)-1 == dicttype) {
	dicttype = CFDictionaryGetTypeID();
    }
    if ((CFTypeID)-1 == arraytype) {
	arraytype = CFArrayGetTypeID();
    }
    if ((CFTypeID)-1 == settype) {
	settype = CFSetGetTypeID();
    }
    if ((CFTypeID)-1 == nulltype) {
	nulltype = CFNullGetTypeID();
    }

    objtable = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, NULL);
    _CFDictionarySetCapacity(objtable, 640);
    objlist = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
    _CFArraySetCapacity(objlist, 640);
    CFSetCallBacks cb = kCFTypeSetCallBacks;
    cb.retain = NULL;
    cb.release = NULL;
    CFMutableSetRef uniquingsets[4];
    uniquingsets[0] = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &cb);
    _CFSetSetCapacity(uniquingsets[0], 1000);
    cb.equal = __plistNumberEqual;
    uniquingsets[1] = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &cb);
    _CFSetSetCapacity(uniquingsets[1], 500);
    cb.equal = kCFTypeSetCallBacks.equal;
    uniquingsets[2] = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &cb);
    _CFSetSetCapacity(uniquingsets[2], 500);
    cb.hash = __plistDataHash;
    uniquingsets[3] = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &cb);
    _CFSetSetCapacity(uniquingsets[3], 500);

    _flattenPlist(plist, objlist, objtable, uniquingsets);

    CFRelease(uniquingsets[0]);
    CFRelease(uniquingsets[1]);
    CFRelease(uniquingsets[2]);
    CFRelease(uniquingsets[3]);

    cnt = CFArrayGetCount(objlist);
    offsets = (uint64_t *)CFAllocatorAllocate(kCFAllocatorSystemDefault, (CFIndex)(cnt * sizeof(*offsets)), 0);

    buf = (__CFBinaryPlistWriteBuffer *)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(__CFBinaryPlistWriteBuffer), 0);
    buf->stream = stream;
    buf->error = NULL;
    buf->streamIsData = (CFGetTypeID(stream) == CFDataGetTypeID());
    buf->written = 0;
    buf->used = 0;
    bufferWrite(buf, (uint8_t *)"bplist00", 8);	// header

    memset(&trailer, 0, sizeof(trailer));
    trailer._numObjects = CFSwapInt64HostToBig(cnt);
    trailer._topObject = 0;	// true for this implementation
    mask = ~(uint64_t)0;
    while (cnt & mask) {
	trailer._objectRefSize++;
	mask = mask << 8;
    }

    for (idx = 0; idx < cnt; idx++) {
	CFPropertyListRef obj = CFArrayGetValueAtIndex(objlist, (CFIndex)idx);
	CFTypeID type = __CFGenericTypeID_genericobj_inline(obj);
	offsets[idx] = buf->written + buf->used;
	if (stringtype == type) {
	    CFIndex ret, count = CFStringGetLength((CFStringRef)obj);
	    CFIndex needed;
	    uint8_t *bytes, buffer[1024];
	    bytes = (count <= 1024) ? buffer : (uint8_t *)CFAllocatorAllocate(kCFAllocatorSystemDefault, count, 0);
	    // presumption, believed to be true, is that ASCII encoding may need
	    // less bytes, but will not need greater, than the # of unichars
	    ret = CFStringGetBytes((CFStringRef)obj, CFRangeMake(0, count), kCFStringEncodingASCII, 0, false, bytes, count, &needed);
	    if (ret == count) {
		uint8_t marker = (uint8_t)(kCFBinaryPlistMarkerASCIIString | (needed < 15 ? needed : 0xf));
		bufferWrite(buf, &marker, 1);
		if (15 <= needed) {
		    _appendInt(buf, (uint64_t)needed);
		}
		bufferWrite(buf, bytes, needed);
	    } else {
		UniChar *chars;
		uint8_t marker = (uint8_t)(kCFBinaryPlistMarkerUnicode16String | (count < 15 ? count : 0xf));
		bufferWrite(buf, &marker, 1);
		if (15 <= count) {
		    _appendInt(buf, (uint64_t)count);
		}
		chars = (UniChar *)CFAllocatorAllocate(kCFAllocatorSystemDefault, count * sizeof(UniChar), 0);
		CFStringGetCharacters((CFStringRef)obj, CFRangeMake(0, count), chars);
		for (idx2 = 0; idx2 < count; idx2++) {
		    chars[idx2] = CFSwapInt16HostToBig(chars[idx2]);
		}
		bufferWrite(buf, (uint8_t *)chars, count * sizeof(UniChar));
		CFAllocatorDeallocate(kCFAllocatorSystemDefault, chars);
	    }
	    if (bytes != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, bytes);
	} else if (numbertype == type) {
	    uint8_t marker;
	    uint64_t bigint;
	    uint8_t *bytes;
	    CFIndex nbytes;
	    if (CFNumberIsFloatType((CFNumberRef)obj)) {
		CFSwappedFloat64 swapped64;
		CFSwappedFloat32 swapped32;
		if (CFNumberGetByteSize((CFNumberRef)obj) <= (CFIndex)sizeof(float)) {
		    float v;
		    CFNumberGetValue((CFNumberRef)obj, kCFNumberFloat32Type, &v);
		    swapped32 = CFConvertFloat32HostToSwapped(v);
		    bytes = (uint8_t *)&swapped32;
		    nbytes = sizeof(float);
		    marker = kCFBinaryPlistMarkerReal | 2;
		} else {
		    double v;
		    CFNumberGetValue((CFNumberRef)obj, kCFNumberFloat64Type, &v);
		    swapped64 = CFConvertFloat64HostToSwapped(v);
		    bytes = (uint8_t *)&swapped64;
		    nbytes = sizeof(double);
		    marker = kCFBinaryPlistMarkerReal | 3;
		}
		bufferWrite(buf, &marker, 1);
		bufferWrite(buf, bytes, nbytes);
	    } else {
		CFNumberType type = _CFNumberGetType2((CFNumberRef)obj);
		if (kCFNumberSInt128Type == type) {
		    CFSInt128Struct s;
		    CFNumberGetValue((CFNumberRef)obj, kCFNumberSInt128Type, &s);
		    struct {
			int64_t high;
			uint64_t low;
		    } storage;
		    storage.high = CFSwapInt64HostToBig(s.high);
		    storage.low = CFSwapInt64HostToBig(s.low);
		    uint8_t *bytes = (uint8_t *)&storage;
		    uint8_t marker = kCFBinaryPlistMarkerInt | 4;
		    CFIndex nbytes = 16;
		    bufferWrite(buf, &marker, 1);
		    bufferWrite(buf, bytes, nbytes);
		} else {
		    CFNumberGetValue((CFNumberRef)obj, kCFNumberSInt64Type, &bigint);
		    _appendInt(buf, bigint);
		}
	    }
	} else if (_CFKeyedArchiverUIDGetTypeID() == type) {
	    _appendUID(buf, (CFKeyedArchiverUIDRef)obj);
	} else if (booltype == type) {
	    uint8_t marker = CFBooleanGetValue((CFBooleanRef)obj) ? kCFBinaryPlistMarkerTrue : kCFBinaryPlistMarkerFalse;
	    bufferWrite(buf, &marker, 1);
	} else if (datatype == type) {
	    CFIndex count = CFDataGetLength((CFDataRef)obj);
	    uint8_t marker = (uint8_t)(kCFBinaryPlistMarkerData | (count < 15 ? count : 0xf));
	    bufferWrite(buf, &marker, 1);
	    if (15 <= count) {
		_appendInt(buf, (uint64_t)count);
	    }
	    bufferWrite(buf, CFDataGetBytePtr((CFDataRef)obj), count);
	} else if (datetype == type) {
	    CFSwappedFloat64 swapped;
	    uint8_t marker = kCFBinaryPlistMarkerDate;
	    bufferWrite(buf, &marker, 1);
	    swapped = CFConvertFloat64HostToSwapped(CFDateGetAbsoluteTime((CFDateRef)obj));
	    bufferWrite(buf, (uint8_t *)&swapped, sizeof(swapped));
	} else if (dicttype == type) {
	    CFIndex count = CFDictionaryGetCount((CFDictionaryRef)obj);
	    CFPropertyListRef *list, buffer[512];
	    uint8_t marker = (uint8_t)(kCFBinaryPlistMarkerDict | (count < 15 ? count : 0xf));
	    bufferWrite(buf, &marker, 1);
	    if (15 <= count) {
		_appendInt(buf, (uint64_t)count);
	    }
	    list = (count <= 256) ? buffer : (CFPropertyListRef *)CFAllocatorAllocate(kCFAllocatorSystemDefault, 2 * count * sizeof(CFTypeRef), 0);
	    CFDictionaryGetKeysAndValues((CFDictionaryRef)obj, list, list + count);
	    for (idx2 = 0; idx2 < 2 * count; idx2++) {
		CFPropertyListRef value = list[idx2];
		uint32_t swapped = 0;
		uint8_t *source = (uint8_t *)&swapped;
                refnum = (uint32_t)(uintptr_t)CFDictionaryGetValue(objtable, value);
                swapped = CFSwapInt32HostToBig((uint32_t)refnum);
		bufferWrite(buf, source + sizeof(swapped) - trailer._objectRefSize, trailer._objectRefSize);
	    }
	    if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
	} else if (arraytype == type) {
	    CFIndex count = CFArrayGetCount((CFArrayRef)obj);
	    CFPropertyListRef *list, buffer[256];
	    uint8_t marker = (uint8_t)(kCFBinaryPlistMarkerArray | (count < 15 ? count : 0xf));
	    bufferWrite(buf, &marker, 1);
	    if (15 <= count) {
		_appendInt(buf, (uint64_t)count);
	    }
	    list = (count <= 256) ? buffer : (CFPropertyListRef *)CFAllocatorAllocate(kCFAllocatorSystemDefault, count * sizeof(CFTypeRef), 0);
	    CFArrayGetValues((CFArrayRef)obj, CFRangeMake(0, count), list);
	    for (idx2 = 0; idx2 < count; idx2++) {
		CFPropertyListRef value = list[idx2];
		uint32_t swapped = 0;
		uint8_t *source = (uint8_t *)&swapped;
                refnum = (uint32_t)(uintptr_t)CFDictionaryGetValue(objtable, value);
                swapped = CFSwapInt32HostToBig((uint32_t)refnum);
		bufferWrite(buf, source + sizeof(swapped) - trailer._objectRefSize, trailer._objectRefSize);
	    }
	    if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
	} else {
	    CFRelease(objtable);
	    CFRelease(objlist);
	    if (buf->error) CFRelease(buf->error);
	    CFAllocatorDeallocate(kCFAllocatorSystemDefault, buf);
            CFAllocatorDeallocate(kCFAllocatorSystemDefault, offsets);
	    return 0;
	}
    }
    CFRelease(objtable);
    CFRelease(objlist);

    length_so_far = buf->written + buf->used;
    trailer._offsetTableOffset = CFSwapInt64HostToBig(length_so_far);
    trailer._offsetIntSize = 0;
    mask = ~(uint64_t)0;
    while (length_so_far & mask) {
	trailer._offsetIntSize++;
	mask = mask << 8;
    }

    for (idx = 0; idx < cnt; idx++) {
	uint64_t swapped = CFSwapInt64HostToBig(offsets[idx]);
	uint8_t *source = (uint8_t *)&swapped;
	bufferWrite(buf, source + sizeof(*offsets) - trailer._offsetIntSize, trailer._offsetIntSize);
    }
    length_so_far += cnt * trailer._offsetIntSize;

    bufferWrite(buf, (uint8_t *)&trailer, sizeof(trailer));
    bufferFlush(buf);
    length_so_far += sizeof(trailer);
    if (buf->error) {
	CFRelease(buf->error);
	return 0;
    }
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, buf);
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, offsets);
    return (CFIndex)length_so_far;
}


#define FAIL_FALSE	do { return false; } while (0)
#define FAIL_MAXOFFSET	do { return UINT64_MAX; } while (0)

bool __CFBinaryPlistGetTopLevelInfo(const uint8_t *databytes, uint64_t datalen, uint8_t *marker, uint64_t *offset, CFBinaryPlistTrailer *trailer) {
    CFBinaryPlistTrailer trail;

    if ((CFTypeID)-1 == stringtype) {
	stringtype = CFStringGetTypeID();
    }
    if ((CFTypeID)-1 == datatype) {
	datatype = CFDataGetTypeID();
    }
    if ((CFTypeID)-1 == numbertype) {
	numbertype = CFNumberGetTypeID();
    }
    if ((CFTypeID)-1 == booltype) {
	booltype = CFBooleanGetTypeID();
    }
    if ((CFTypeID)-1 == datetype) {
	datetype = CFDateGetTypeID();
    }
    if ((CFTypeID)-1 == dicttype) {
	dicttype = CFDictionaryGetTypeID();
    }
    if ((CFTypeID)-1 == arraytype) {
	arraytype = CFArrayGetTypeID();
    }
    if ((CFTypeID)-1 == settype) {
	settype = CFSetGetTypeID();
    }
    if ((CFTypeID)-1 == nulltype) {
	nulltype = CFNullGetTypeID();
    }

    if (!databytes || datalen < sizeof(trail) + 8 + 1) FAIL_FALSE;
    if (0 != memcmp("bplist00", databytes, 8) && 0 != memcmp("bplist01", databytes, 8)) return false;
    memmove(&trail, databytes + datalen - sizeof(trail), sizeof(trail));
    if (trail._unused[0] != 0 || trail._unused[1] != 0 || trail._unused[2] != 0 || trail._unused[3] != 0 || trail._unused[4] != 0 || trail._unused[5] != 0) FAIL_FALSE;
    trail._numObjects = CFSwapInt64BigToHost(trail._numObjects);
    trail._topObject = CFSwapInt64BigToHost(trail._topObject);
    trail._offsetTableOffset = CFSwapInt64BigToHost(trail._offsetTableOffset);
    if (LONG_MAX < trail._numObjects) FAIL_FALSE;
    if (LONG_MAX < trail._offsetTableOffset) FAIL_FALSE;
    if (trail._numObjects < 1) FAIL_FALSE;
    if (trail._numObjects <= trail._topObject) FAIL_FALSE;
    if (trail._offsetTableOffset < 9) FAIL_FALSE;
    if (datalen - sizeof(trail) <= trail._offsetTableOffset) FAIL_FALSE;
    if (trail._offsetIntSize < 1) FAIL_FALSE;
    if (trail._objectRefSize < 1) FAIL_FALSE;
    int32_t err = CF_NO_ERROR;
    uint64_t offsetIntSize = trail._offsetIntSize;
    uint64_t offsetTableSize = __check_uint64_mul_unsigned_unsigned(trail._numObjects, offsetIntSize, &err);
    if (CF_NO_ERROR!= err) FAIL_FALSE;
    if (offsetTableSize < 1) FAIL_FALSE;
    uint64_t objectDataSize = trail._offsetTableOffset - 8;
    uint64_t tmpSum = __check_uint64_add_unsigned_unsigned(8, objectDataSize, &err);
    tmpSum = __check_uint64_add_unsigned_unsigned(tmpSum, offsetTableSize, &err);
    tmpSum = __check_uint64_add_unsigned_unsigned(tmpSum, sizeof(trail), &err);
    if (CF_NO_ERROR != err) FAIL_FALSE;
    if (datalen != tmpSum) FAIL_FALSE;
    if (trail._objectRefSize < 8 && (1ULL << (8 * trail._objectRefSize)) <= trail._numObjects) FAIL_FALSE;
    if (trail._offsetIntSize < 8 && (1ULL << (8 * trail._offsetIntSize)) <= trail._offsetTableOffset) FAIL_FALSE;
    const uint8_t *objectsFirstByte;
    objectsFirstByte = check_ptr_add(databytes, 8, &err);
    if (CF_NO_ERROR != err) FAIL_FALSE;
    const uint8_t *offsetsFirstByte = check_ptr_add(databytes, trail._offsetTableOffset, &err);
    if (CF_NO_ERROR != err) FAIL_FALSE;
    const uint8_t *offsetsLastByte;
    offsetsLastByte = check_ptr_add(offsetsFirstByte, offsetTableSize - 1, &err);
    if (CF_NO_ERROR != err) FAIL_FALSE;

    const uint8_t *bytesptr = databytes + trail._offsetTableOffset;
    uint64_t maxOffset = trail._offsetTableOffset - 1;
    for (CFIndex idx = 0; idx < trail._numObjects; idx++) {
	uint64_t off = 0;
	for (CFIndex idx2 = 0; idx2 < trail._offsetIntSize; idx2++) {
	    off = (off << 8) + bytesptr[idx2];
	}
	if (maxOffset < off) FAIL_FALSE;
	bytesptr += trail._offsetIntSize;
    }

    bytesptr = databytes + trail._offsetTableOffset + trail._topObject * trail._offsetIntSize;
    uint64_t off = 0;
    for (CFIndex idx = 0; idx < trail._offsetIntSize; idx++) {
	off = (off << 8) + bytesptr[idx];
    }
    if (off < 8 || trail._offsetTableOffset <= off) FAIL_FALSE;
    if (trailer) *trailer = trail;
    if (offset) *offset = off;
    if (marker) *marker = *(databytes + off);
    return true;
}

CF_INLINE Boolean _plistIsPrimitive(CFPropertyListRef pl) {
    CFTypeID type = __CFGenericTypeID_genericobj_inline(pl);
    if (dicttype == type || arraytype == type || settype == type) FAIL_FALSE;
    return true;
}

CF_INLINE bool _readInt(const uint8_t *ptr, const uint8_t *end_byte_ptr, uint64_t *bigint, const uint8_t **newptr) {
    if (end_byte_ptr < ptr) FAIL_FALSE;
    uint8_t marker = *ptr++;
    if ((marker & 0xf0) != kCFBinaryPlistMarkerInt) FAIL_FALSE;
    uint64_t cnt = 1 << (marker & 0x0f);
    int32_t err = CF_NO_ERROR;
    const uint8_t *extent = check_ptr_add(ptr, cnt, &err) - 1;
    if (CF_NO_ERROR != err) FAIL_FALSE;
    if (end_byte_ptr < extent) FAIL_FALSE;
    // integers are not required to be in the most compact possible representation, but only the last 64 bits are significant currently
    *bigint = 0;
    for (CFIndex idx = 0; idx < cnt; idx++) {
	*bigint = (*bigint << 8) + *ptr++;
    }
    if (newptr) *newptr = ptr;
    return true;
}

// bytesptr points at a ref
CF_INLINE uint64_t _getOffsetOfRefAt(const uint8_t *databytes, const uint8_t *bytesptr, const CFBinaryPlistTrailer *trailer) {
    // *trailer contents are trusted, even for overflows -- was checked when the trailer was parsed;
    // this pointer arithmetic and the multiplication was also already done once and checked,
    // and the offsetTable was already validated.
    const uint8_t *objectsFirstByte = databytes + 8;
    const uint8_t *offsetsFirstByte = databytes + trailer->_offsetTableOffset;
    if (bytesptr < objectsFirstByte || offsetsFirstByte - trailer->_objectRefSize < bytesptr) FAIL_MAXOFFSET;

    uint64_t ref = 0;
    for (CFIndex idx = 0; idx < trailer->_objectRefSize; idx++) {
	ref = (ref << 8) + bytesptr[idx];
    }
    if (trailer->_numObjects <= ref) FAIL_MAXOFFSET;

    bytesptr = databytes + trailer->_offsetTableOffset + ref * trailer->_offsetIntSize;
    uint64_t off = 0;
    for (CFIndex idx = 0; idx < trailer->_offsetIntSize; idx++) {
	off = (off << 8) + bytesptr[idx];
    }
    return off;
}

static bool __CFBinaryPlistCreateObject2(const uint8_t *databytes, uint64_t datalen, uint64_t startOffset, const CFBinaryPlistTrailer *trailer, CFAllocatorRef allocator, CFOptionFlags mutabilityOption, CFMutableDictionaryRef objects, CFMutableSetRef set, CFIndex curDepth, CFPropertyListRef *plist);

bool __CFBinaryPlistGetOffsetForValueFromArray2(const uint8_t *databytes, uint64_t datalen, uint64_t startOffset, const CFBinaryPlistTrailer *trailer, CFIndex idx, uint64_t *offset, CFMutableDictionaryRef objects) {
    uint64_t objectsRangeStart = 8, objectsRangeEnd = trailer->_offsetTableOffset - 1;
    if (startOffset < objectsRangeStart || objectsRangeEnd < startOffset) FAIL_FALSE;
    const uint8_t *ptr = databytes + startOffset;
    uint8_t marker = *ptr;
    if ((marker & 0xf0) != kCFBinaryPlistMarkerArray) FAIL_FALSE;
    int32_t err = CF_NO_ERROR;
    ptr = check_ptr_add(ptr, 1, &err);
    if (CF_NO_ERROR != err) FAIL_FALSE;
    uint64_t cnt = (marker & 0x0f);
    if (0xf == cnt) {
	uint64_t bigint;
	if (!_readInt(ptr, databytes + objectsRangeEnd, &bigint, &ptr)) FAIL_FALSE;
	if (LONG_MAX < bigint) FAIL_FALSE;
	cnt = bigint;
    }
    if (cnt <= idx) FAIL_FALSE;
    size_t byte_cnt = check_size_t_mul(cnt, trailer->_objectRefSize, &err);
    if (CF_NO_ERROR != err) FAIL_FALSE;
    const uint8_t *extent = check_ptr_add(ptr, byte_cnt, &err) - 1;
    if (CF_NO_ERROR != err) FAIL_FALSE;
    if (databytes + objectsRangeEnd < extent) FAIL_FALSE;
    uint64_t off = _getOffsetOfRefAt(databytes, ptr + idx * trailer->_objectRefSize, trailer);
    if (offset) *offset = off;
    return true;
}

bool __CFBinaryPlistGetOffsetForValueFromDictionary2(const uint8_t *databytes, uint64_t datalen, uint64_t startOffset, const CFBinaryPlistTrailer *trailer, CFTypeRef key, uint64_t *koffset, uint64_t *voffset, CFMutableDictionaryRef objects) {
    if (!key || !_plistIsPrimitive(key)) FAIL_FALSE;
    uint64_t objectsRangeStart = 8, objectsRangeEnd = trailer->_offsetTableOffset - 1;
    if (startOffset < objectsRangeStart || objectsRangeEnd < startOffset) FAIL_FALSE;
    const uint8_t *ptr = databytes + startOffset;
    uint8_t marker = *ptr;
    if ((marker & 0xf0) != kCFBinaryPlistMarkerDict) FAIL_FALSE;
    int32_t err = CF_NO_ERROR;
    ptr = check_ptr_add(ptr, 1, &err);
    if (CF_NO_ERROR != err) FAIL_FALSE;
    uint64_t cnt = (marker & 0x0f);
    if (0xf == cnt) {
	uint64_t bigint = 0;
	if (!_readInt(ptr, databytes + objectsRangeEnd, &bigint, &ptr)) FAIL_FALSE;
	if (LONG_MAX < bigint) FAIL_FALSE;
	cnt = bigint;
    }
    cnt = check_size_t_mul(cnt, 2, &err);
    if (CF_NO_ERROR != err) FAIL_FALSE;
    size_t byte_cnt = check_size_t_mul(cnt, trailer->_objectRefSize, &err);
    if (CF_NO_ERROR != err) FAIL_FALSE;
    const uint8_t *extent = check_ptr_add(ptr, byte_cnt, &err) - 1;
    if (CF_NO_ERROR != err) FAIL_FALSE;
    if (databytes + objectsRangeEnd < extent) FAIL_FALSE;
    CFIndex stringKeyLen = -1;
    UniChar ubuffer[16];
    if (__CFGenericTypeID_genericobj_inline(key) == stringtype) {
	stringKeyLen = CFStringGetLength((CFStringRef)key);
	if (stringKeyLen < 0xf) {
	    CFStringGetCharacters((CFStringRef)key, CFRangeMake(0, stringKeyLen), ubuffer);
	}
    }
    cnt = cnt / 2;
    for (CFIndex idx = 0; idx < cnt; idx++) {
	uint64_t off = _getOffsetOfRefAt(databytes, ptr, trailer);
	uint8_t marker = *(databytes + off);
	CFIndex len = marker & 0x0f;
	// if it is a short ascii string in the data, and the key is a string
	if ((marker & 0xf0) == kCFBinaryPlistMarkerASCIIString && len < 0xf && stringKeyLen != -1) {
	    if (len != stringKeyLen) goto miss;
	    err = CF_NO_ERROR;
	    const uint8_t *ptr2 = databytes + off;
	    extent = check_ptr_add(ptr2, len, &err);
	    if (CF_NO_ERROR != err) FAIL_FALSE;
	    if (databytes + trailer->_offsetTableOffset <= extent) FAIL_FALSE;
            for (CFIndex idx2 = 0; idx2 < stringKeyLen; idx2++) {
                if ((UniChar)ptr2[idx2 + 1] != ubuffer[idx2]) goto miss;
            }
	    if (koffset) *koffset = off;
	    if (voffset) {
		off = _getOffsetOfRefAt(databytes, ptr + cnt * trailer->_objectRefSize, trailer);
		*voffset = off;
	    }
	    return true;
	    miss:;
	} else {
	    CFPropertyListRef pl = NULL;
	    if (!__CFBinaryPlistCreateObject2(databytes, datalen, off, trailer, kCFAllocatorSystemDefault, kCFPropertyListImmutable, objects, NULL, 0, &pl) || !_plistIsPrimitive(pl)) {
		if (pl) CFRelease(pl);
		FAIL_FALSE;
	    }
	    if (CFEqual(key, pl)) {
		CFRelease(pl);
		if (koffset) *koffset = off;
		if (voffset) {
		    off = _getOffsetOfRefAt(databytes, ptr + cnt * trailer->_objectRefSize, trailer);
		    *voffset = off;
		}
		return true;
	    }
	    CFRelease(pl);
	}
	ptr += trailer->_objectRefSize;
    }
    return false;
}

extern CFArrayRef _CFArrayCreate_ex(CFAllocatorRef allocator, Boolean isMutable, const void **values, CFIndex numValues);
extern CFSetRef _CFSetCreate_ex(CFAllocatorRef allocator, Boolean isMutable, const void **values, CFIndex numValues);
extern CFDictionaryRef _CFDictionaryCreate_ex(CFAllocatorRef allocator, Boolean isMutable, const void **keys, const void **values, CFIndex numValues);

static bool __CFBinaryPlistCreateObject2(const uint8_t *databytes, uint64_t datalen, uint64_t startOffset, const CFBinaryPlistTrailer *trailer, CFAllocatorRef allocator, CFOptionFlags mutabilityOption, CFMutableDictionaryRef objects, CFMutableSetRef set, CFIndex curDepth, CFPropertyListRef *plist) {

    if (objects) {
	*plist = CFDictionaryGetValue(objects, (const void *)(uintptr_t)startOffset);
	if (*plist) {
	    CFRetain(*plist);
	    return true;
	}
    }

    // at any one invocation of this function, set should contain the offsets in the "path" down to this object
    if (set && CFSetContainsValue(set, (const void *)(uintptr_t)startOffset)) return false;

    // databytes is trusted to be at least datalen bytes long
    // *trailer contents are trusted, even for overflows -- was checked when the trailer was parsed
    uint64_t objectsRangeStart = 8, objectsRangeEnd = trailer->_offsetTableOffset - 1;
    if (startOffset < objectsRangeStart || objectsRangeEnd < startOffset) FAIL_FALSE;

    uint64_t off;
    CFPropertyListRef *list, buffer[256];
    CFAllocatorRef listAllocator;

    uint8_t marker = *(databytes + startOffset);
    switch (marker & 0xf0) {
    case kCFBinaryPlistMarkerNull:
	switch (marker) {
	case kCFBinaryPlistMarkerNull:
	    *plist = kCFNull;
	    return true;
	case kCFBinaryPlistMarkerFalse:
	    *plist = CFRetain(kCFBooleanFalse);
	    return true;
	case kCFBinaryPlistMarkerTrue:
	    *plist = CFRetain(kCFBooleanTrue);
	    return true;
	}
	FAIL_FALSE;
    case kCFBinaryPlistMarkerInt:
    {
	const uint8_t *ptr = (databytes + startOffset);
	int32_t err = CF_NO_ERROR;
	ptr = check_ptr_add(ptr, 1, &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	uint64_t cnt = 1 << (marker & 0x0f);
	const uint8_t *extent = check_ptr_add(ptr, cnt, &err) - 1;
	if (CF_NO_ERROR != err) FAIL_FALSE;
	if (databytes + objectsRangeEnd < extent) FAIL_FALSE;
	if (16 < cnt) FAIL_FALSE;
	// in format version '00', 1, 2, and 4-byte integers have to be interpreted as unsigned,
	// whereas 8-byte integers are signed (and 16-byte when available)
	// negative 1, 2, 4-byte integers are always emitted as 8 bytes in format '00'
	uint64_t bigint = 0;
	// integers are not required to be in the most compact possible representation, but only the last 64 bits are significant currently
	for (CFIndex idx = 0; idx < cnt; idx++) {
	    bigint = (bigint << 8) + *ptr++;
	}
	if (8 < cnt) {
	    CFSInt128Struct val;
	    val.high = 0;
	    val.low = bigint;
	    *plist = CFNumberCreate(allocator, kCFNumberSInt128Type, &val);
	} else {
	    *plist = CFNumberCreate(allocator, kCFNumberSInt64Type, &bigint);
	}
	// these are always immutable
	if (objects && *plist) {
	    CFDictionarySetValue(objects, (const void *)(uintptr_t)startOffset, *plist);
	}
	return (*plist) ? true : false;
    }
    case kCFBinaryPlistMarkerReal:
	switch (marker & 0x0f) {
	case 2: {
	    const uint8_t *ptr = (databytes + startOffset);
	    int32_t err = CF_NO_ERROR;
	    ptr = check_ptr_add(ptr, 1, &err);
	    if (CF_NO_ERROR != err) FAIL_FALSE;
	    const uint8_t *extent = check_ptr_add(ptr, 4, &err) - 1;
	    if (CF_NO_ERROR != err) FAIL_FALSE;
	    if (databytes + objectsRangeEnd < extent) FAIL_FALSE;
	    CFSwappedFloat32 swapped32;
	    memmove(&swapped32, ptr, 4);
	    float f = CFConvertFloat32SwappedToHost(swapped32);
	    *plist = CFNumberCreate(allocator, kCFNumberFloat32Type, &f);
	    // these are always immutable
	    if (objects && *plist) {
		CFDictionarySetValue(objects, (const void *)(uintptr_t)startOffset, *plist);
	    }
	    return (*plist) ? true : false;
	}
	case 3: {
	    const uint8_t *ptr = (databytes + startOffset);
	    int32_t err = CF_NO_ERROR;
	    ptr = check_ptr_add(ptr, 1, &err);
	    if (CF_NO_ERROR != err) FAIL_FALSE;
	    const uint8_t *extent = check_ptr_add(ptr, 8, &err) - 1;
	    if (CF_NO_ERROR != err) FAIL_FALSE;
	    if (databytes + objectsRangeEnd < extent) FAIL_FALSE;
	    CFSwappedFloat64 swapped64;
	    memmove(&swapped64, ptr, 8);
	    double d = CFConvertFloat64SwappedToHost(swapped64);
	    *plist = CFNumberCreate(allocator, kCFNumberFloat64Type, &d);
	    // these are always immutable
	    if (objects && *plist) {
		CFDictionarySetValue(objects, (const void *)(uintptr_t)startOffset, *plist);
	    }
	    return (*plist) ? true : false;
	}
	}
	FAIL_FALSE;
    case kCFBinaryPlistMarkerDate & 0xf0:
	switch (marker) {
	case kCFBinaryPlistMarkerDate: {
	    const uint8_t *ptr = (databytes + startOffset);
	    int32_t err = CF_NO_ERROR;
	    ptr = check_ptr_add(ptr, 1, &err);
	    if (CF_NO_ERROR != err) FAIL_FALSE;
	    const uint8_t *extent = check_ptr_add(ptr, 8, &err) - 1;
	    if (CF_NO_ERROR != err) FAIL_FALSE;
	    if (databytes + objectsRangeEnd < extent) FAIL_FALSE;
	    CFSwappedFloat64 swapped64;
	    memmove(&swapped64, ptr, 8);
	    double d = CFConvertFloat64SwappedToHost(swapped64);
	    *plist = CFDateCreate(allocator, d);
	    // these are always immutable
	    if (objects && *plist) {
		CFDictionarySetValue(objects, (const void *)(uintptr_t)startOffset, *plist);
	    }
	    return (*plist) ? true : false;
	}
	}
	FAIL_FALSE;
    case kCFBinaryPlistMarkerData: {
	const uint8_t *ptr = databytes + startOffset;
	int32_t err = CF_NO_ERROR;
	ptr = check_ptr_add(ptr, 1, &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	CFIndex cnt = marker & 0x0f;
	if (0xf == cnt) {
	    uint64_t bigint = 0;
	    if (!_readInt(ptr, databytes + objectsRangeEnd, &bigint, &ptr)) FAIL_FALSE;
	    if (LONG_MAX < bigint) FAIL_FALSE;
	    cnt = (CFIndex)bigint;
	}
	const uint8_t *extent = check_ptr_add(ptr, cnt, &err) - 1;
	if (CF_NO_ERROR != err) FAIL_FALSE;
	if (databytes + objectsRangeEnd < extent) FAIL_FALSE;
	if (mutabilityOption == kCFPropertyListMutableContainersAndLeaves) {
	    *plist = CFDataCreateMutable(allocator, 0);
	    if (*plist) CFDataAppendBytes((CFMutableDataRef)*plist, ptr, cnt);
	} else {
	    *plist = CFDataCreate(allocator, ptr, cnt);
	}
        if (objects && *plist && (mutabilityOption != kCFPropertyListMutableContainersAndLeaves)) {
	    CFDictionarySetValue(objects, (const void *)(uintptr_t)startOffset, *plist);
	}
	return (*plist) ? true : false;
	}
    case kCFBinaryPlistMarkerASCIIString: {
	const uint8_t *ptr = databytes + startOffset;
	int32_t err = CF_NO_ERROR;
	ptr = check_ptr_add(ptr, 1, &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	CFIndex cnt = marker & 0x0f;
	if (0xf == cnt) {
            uint64_t bigint = 0;
	    if (!_readInt(ptr, databytes + objectsRangeEnd, &bigint, &ptr)) FAIL_FALSE;
	    if (LONG_MAX < bigint) FAIL_FALSE;
	    cnt = (CFIndex)bigint;
	}
	const uint8_t *extent = check_ptr_add(ptr, cnt, &err) - 1;
	if (CF_NO_ERROR != err) FAIL_FALSE;
	if (databytes + objectsRangeEnd < extent) FAIL_FALSE;
	if (mutabilityOption == kCFPropertyListMutableContainersAndLeaves) {
	    CFStringRef str = CFStringCreateWithBytes(allocator, ptr, cnt, kCFStringEncodingASCII, false);
	    *plist = str ? CFStringCreateMutableCopy(allocator, 0, str) : NULL;
	    if (str) CFRelease(str);
	} else {
	    *plist = CFStringCreateWithBytes(allocator, ptr, cnt, kCFStringEncodingASCII, false);
	}
        if (objects && *plist && (mutabilityOption != kCFPropertyListMutableContainersAndLeaves)) {
	    CFDictionarySetValue(objects, (const void *)(uintptr_t)startOffset, *plist);
	}
	return (*plist) ? true : false;
	}
    case kCFBinaryPlistMarkerUnicode16String: {
	const uint8_t *ptr = databytes + startOffset;
	int32_t err = CF_NO_ERROR;
	ptr = check_ptr_add(ptr, 1, &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	CFIndex cnt = marker & 0x0f;
	if (0xf == cnt) {
            uint64_t bigint = 0;
	    if (!_readInt(ptr, databytes + objectsRangeEnd, &bigint, &ptr)) FAIL_FALSE;
	    if (LONG_MAX < bigint) FAIL_FALSE;
	    cnt = (CFIndex)bigint;
	}
	const uint8_t *extent = check_ptr_add(ptr, cnt, &err) - 1;
	extent = check_ptr_add(extent, cnt, &err);	// 2 bytes per character
	if (CF_NO_ERROR != err) FAIL_FALSE;
	if (databytes + objectsRangeEnd < extent) FAIL_FALSE;
	size_t byte_cnt = check_size_t_mul(cnt, sizeof(UniChar), &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	UniChar *chars = (UniChar *)CFAllocatorAllocate(allocator, byte_cnt, 0);
	if (!chars) FAIL_FALSE;
	memmove(chars, ptr, byte_cnt);
	for (CFIndex idx = 0; idx < cnt; idx++) {
	    chars[idx] = CFSwapInt16BigToHost(chars[idx]);
	}
	if (mutabilityOption == kCFPropertyListMutableContainersAndLeaves) {
	    CFStringRef str = CFStringCreateWithCharactersNoCopy(allocator, chars, cnt, allocator);
	    *plist = str ? CFStringCreateMutableCopy(allocator, 0, str) : NULL;
	    if (str) CFRelease(str);
	} else {
	    *plist = CFStringCreateWithCharactersNoCopy(allocator, chars, cnt, allocator);
	}
        if (objects && *plist && (mutabilityOption != kCFPropertyListMutableContainersAndLeaves)) {
	    CFDictionarySetValue(objects, (const void *)(uintptr_t)startOffset, *plist);
	}
	return (*plist) ? true : false;
	}
    case kCFBinaryPlistMarkerUID: {
	const uint8_t *ptr = databytes + startOffset;
	int32_t err = CF_NO_ERROR;
	ptr = check_ptr_add(ptr, 1, &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	CFIndex cnt = (marker & 0x0f) + 1;
	const uint8_t *extent = check_ptr_add(ptr, cnt, &err) - 1;
	if (CF_NO_ERROR != err) FAIL_FALSE;
	if (databytes + objectsRangeEnd < extent) FAIL_FALSE;
	// uids are not required to be in the most compact possible representation, but only the last 64 bits are significant currently
	uint64_t bigint = 0;
	for (CFIndex idx = 0; idx < cnt; idx++) {
	    bigint = (bigint << 8) + *ptr++;
	}
	if (UINT32_MAX < bigint) FAIL_FALSE;
	*plist = _CFKeyedArchiverUIDCreate(allocator, (uint32_t)bigint);
	// these are always immutable
	if (objects && *plist) {
	    CFDictionarySetValue(objects, (const void *)(uintptr_t)startOffset, *plist);
	}
	return (*plist) ? true : false;
	}
    case kCFBinaryPlistMarkerArray:
    case kCFBinaryPlistMarkerSet: {
	const uint8_t *ptr = databytes + startOffset;
	int32_t err = CF_NO_ERROR;
	ptr = check_ptr_add(ptr, 1, &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	CFIndex cnt = marker & 0x0f;
	if (0xf == cnt) {
	    uint64_t bigint = 0;
	    if (!_readInt(ptr, databytes + objectsRangeEnd, &bigint, &ptr)) FAIL_FALSE;
	    if (LONG_MAX < bigint) FAIL_FALSE;
	    cnt = (CFIndex)bigint;
	}
	size_t byte_cnt = check_size_t_mul(cnt, trailer->_objectRefSize, &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	const uint8_t *extent = check_ptr_add(ptr, byte_cnt, &err) - 1;
	if (CF_NO_ERROR != err) FAIL_FALSE;
	if (databytes + objectsRangeEnd < extent) FAIL_FALSE;
	byte_cnt = check_size_t_mul(cnt, sizeof(CFPropertyListRef), &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	list = (cnt <= 256) ? buffer : (CFPropertyListRef *)CFAllocatorAllocate(kCFAllocatorSystemDefault, byte_cnt, __kCFAllocatorGCScannedMemory);
	listAllocator = (list == buffer ? kCFAllocatorNull : kCFAllocatorSystemDefault);
	if (!list) FAIL_FALSE;
	Boolean madeSet = false;
	if (!set && 15 < curDepth) {
	    set = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
	    madeSet = set ? true : false;
	}
	if (set) CFSetAddValue(set, (const void *)(uintptr_t)startOffset);
	for (CFIndex idx = 0; idx < cnt; idx++) {
	    CFPropertyListRef pl;
	    off = _getOffsetOfRefAt(databytes, ptr, trailer);
	    if (!__CFBinaryPlistCreateObject2(databytes, datalen, off, trailer, allocator, mutabilityOption, objects, set, curDepth + 1, &pl)) {
		if (!CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
		    while (idx--) {
			CFRelease(list[idx]);
		    }
		}
		if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
		FAIL_FALSE;
	    }
	    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
		CF_WRITE_BARRIER_BASE_ASSIGN(listAllocator, list, list[idx], CFMakeCollectable(pl));
	    } else {
		list[idx] = pl;
	    }
	    ptr += trailer->_objectRefSize;
	}
	if (set) CFSetRemoveValue(set, (const void *)(uintptr_t)startOffset);
	if (madeSet) {
	    CFRelease(set);
	    set = NULL;
	}
	if ((marker & 0xf0) == kCFBinaryPlistMarkerArray) {
	    *plist = _CFArrayCreate_ex(allocator, (mutabilityOption != kCFPropertyListImmutable), list, cnt);
	} else {
	    *plist = _CFSetCreate_ex(allocator, (mutabilityOption != kCFPropertyListImmutable), list, cnt);
	}
	if (objects && *plist && (mutabilityOption == kCFPropertyListImmutable)) {
	    CFDictionarySetValue(objects, (const void *)(uintptr_t)startOffset, *plist);
	}
	if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
	return (*plist) ? true : false;
	}
    case kCFBinaryPlistMarkerDict: {
	const uint8_t *ptr = databytes + startOffset;
	int32_t err = CF_NO_ERROR;
	ptr = check_ptr_add(ptr, 1, &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	CFIndex cnt = marker & 0x0f;
	if (0xf == cnt) {
	    uint64_t bigint = 0;
	    if (!_readInt(ptr, databytes + objectsRangeEnd, &bigint, &ptr)) FAIL_FALSE;
	    if (LONG_MAX < bigint) FAIL_FALSE;
	    cnt = (CFIndex)bigint;
	}
	cnt = check_size_t_mul(cnt, 2, &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	size_t byte_cnt = check_size_t_mul(cnt, trailer->_objectRefSize, &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	const uint8_t *extent = check_ptr_add(ptr, byte_cnt, &err) - 1;
	if (CF_NO_ERROR != err) FAIL_FALSE;
	if (databytes + objectsRangeEnd < extent) FAIL_FALSE;
	byte_cnt = check_size_t_mul(cnt, sizeof(CFPropertyListRef), &err);
	if (CF_NO_ERROR != err) FAIL_FALSE;
	list = (cnt <= 256) ? buffer : (CFPropertyListRef *)CFAllocatorAllocate(kCFAllocatorSystemDefault, byte_cnt, __kCFAllocatorGCScannedMemory);
	listAllocator = (list == buffer ? kCFAllocatorNull : kCFAllocatorSystemDefault);
	if (!list) FAIL_FALSE;
	Boolean madeSet = false;
	if (!set && 15 < curDepth) {
	    set = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
	    madeSet = set ? true : false;
	}
	if (set) CFSetAddValue(set, (const void *)(uintptr_t)startOffset);
	for (CFIndex idx = 0; idx < cnt; idx++) {
	    CFPropertyListRef pl = NULL;
	    off = _getOffsetOfRefAt(databytes, ptr, trailer);
	    if (!__CFBinaryPlistCreateObject2(databytes, datalen, off, trailer, allocator, mutabilityOption, objects, set, curDepth + 1, &pl) || (idx < cnt / 2 && !_plistIsPrimitive(pl))) {
		if (!CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
		    if (pl) CFRelease(pl);
		    while (idx--) {
			CFRelease(list[idx]);
		    }
		}
		if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
		FAIL_FALSE;
	    }
	    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
		CF_WRITE_BARRIER_BASE_ASSIGN(listAllocator, list, list[idx], CFMakeCollectable(pl));
	    } else {
		list[idx] = pl;
	    }
	    ptr += trailer->_objectRefSize;
	}
	if (set) CFSetRemoveValue(set, (const void *)(uintptr_t)startOffset);
	if (madeSet) {
	    CFRelease(set);
	    set = NULL;
	}
	*plist = _CFDictionaryCreate_ex(allocator, (mutabilityOption != kCFPropertyListImmutable), list, list + cnt / 2, cnt / 2);
	if (objects && *plist && (mutabilityOption == kCFPropertyListImmutable)) {
	    CFDictionarySetValue(objects, (const void *)(uintptr_t)startOffset, *plist);
	}
	if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
	return (*plist) ? true : false;
	}
    }
    FAIL_FALSE;
}

bool __CFBinaryPlistCreateObject(const uint8_t *databytes, uint64_t datalen, uint64_t startOffset, const CFBinaryPlistTrailer *trailer, CFAllocatorRef allocator, CFOptionFlags mutabilityOption, CFMutableDictionaryRef objects, CFPropertyListRef *plist) {
	// for compatibility with Foundation's use, need to leave this here
    return __CFBinaryPlistCreateObject2(databytes, datalen, startOffset, trailer, allocator, mutabilityOption, objects, NULL, 0, plist);
}

__private_extern__ bool __CFTryParseBinaryPlist(CFAllocatorRef allocator, CFDataRef data, CFOptionFlags option, CFPropertyListRef *plist, CFStringRef *errorString) {
    uint8_t marker;    
    CFBinaryPlistTrailer trailer;
    uint64_t offset;
    const uint8_t *databytes = CFDataGetBytePtr(data);
    uint64_t datalen = CFDataGetLength(data);

    if (8 <= datalen && __CFBinaryPlistGetTopLevelInfo(databytes, datalen, &marker, &offset, &trailer)) {
	// FALSE: We know for binary plist parsing that the result objects will be retained
	// by their containing collections as the parsing proceeds, so we do not need
	// to use retaining callbacks for the objects map in this case. WHY: the file might
	// be malformed and contain hash-equal keys for the same dictionary (for example)
	// and the later key will cause the previous one to be released when we set the second
	// in the dictionary.
	CFMutableDictionaryRef objects = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, &kCFTypeDictionaryValueCallBacks);
	_CFDictionarySetCapacity(objects, 4000);
	CFPropertyListRef pl = NULL;
        if (__CFBinaryPlistCreateObject2(databytes, datalen, offset, &trailer, allocator, option, objects, NULL, 0, &pl)) {
	    if (plist) *plist = pl;
        } else {
	    if (plist) *plist = NULL;
            if (errorString) *errorString = (CFStringRef)CFRetain(CFSTR("binary data is corrupt"));
	}
	CFRelease(objects);
        return true;
    }
    return false;
}

