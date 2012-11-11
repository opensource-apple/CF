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
/*	CFBinaryPList.c
	Copyright 2000-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFBase.h>
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
#include "ForFoundationOnly.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include "CFInternal.h"


struct __CFKeyedArchiverUID {
    CFRuntimeBase _base;
    uint32_t _value;
};

static CFStringRef __CFKeyedArchiverUIDCopyDescription(CFTypeRef cf) {
    CFKeyedArchiverUIDRef uid = (CFKeyedArchiverUIDRef)cf;
    return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("<CFKeyedArchiverUID %p [%p]>{value = %u}"), cf, CFGetAllocator(cf), uid->_value);
}

static CFStringRef __CFKeyedArchiverUIDCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    CFKeyedArchiverUIDRef uid = (CFKeyedArchiverUIDRef)cf;
    return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("@%u@"), uid->_value);
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
    bool streamIsData;
    uint64_t written;
    int32_t used;
    uint8_t buffer[8192 - 16];
} __CFBinaryPlistWriteBuffer;

static void bufferWrite(__CFBinaryPlistWriteBuffer *buf, const uint8_t *buffer, CFIndex count) {
    CFIndex copyLen;
    if ((CFIndex)sizeof(buf->buffer) <= count) {
	if (buf->streamIsData) {
	    CFDataAppendBytes((CFMutableDataRef)buf->stream, buf->buffer, buf->used);
	} else {
	}
	buf->written += buf->used;
	buf->used = 0;
	if (buf->streamIsData) {
	    CFDataAppendBytes((CFMutableDataRef)buf->stream, buffer, count);
	} else {
	}
	buf->written += count;
	return;
    }
    copyLen = __CFMin(count, (CFIndex)sizeof(buf->buffer) - buf->used);
    memmove(buf->buffer + buf->used, buffer, copyLen);
    buf->used += copyLen;
    if (sizeof(buf->buffer) == buf->used) {
	if (buf->streamIsData) {
	    CFDataAppendBytes((CFMutableDataRef)buf->stream, buf->buffer, sizeof(buf->buffer));
	} else {
	}
	buf->written += sizeof(buf->buffer);
	memmove(buf->buffer, buffer + copyLen, count - copyLen);
	buf->used = count - copyLen;
    }
}

static void bufferFlush(__CFBinaryPlistWriteBuffer *buf) {
    if (buf->streamIsData) {
	CFDataAppendBytes((CFMutableDataRef)buf->stream, buf->buffer, buf->used);
    } else {
    }
    buf->written += buf->used;
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
	string	0110 nnnn	[int]	...	// Unicode string, nnnn is # of chars, else 1111 then int count, then big-endian 2-byte shorts
		0111 xxxx			// unused
	uid	1000 nnnn	...		// nnnn+1 is # of bytes
		1001 xxxx			// unused
	array	1010 nnnn	[int]	objref*	// nnnn is count, unless '1111', then int count follows
		1011 xxxx			// unused
		1100 xxxx			// unused
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

*/


static CFTypeID stringtype = -1, datatype = -1, numbertype = -1, booltype = -1;
static CFTypeID datetype = -1, dicttype = -1, arraytype = -1;

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
    marker = kCFBinaryPlistMarkerUID | (nbytes - 1);
    bigint = CFSwapInt64HostToBig(bigint);
    bytes = (uint8_t *)&bigint + sizeof(bigint) - nbytes;
    bufferWrite(buf, &marker, 1);
    bufferWrite(buf, bytes, nbytes);
}

static Boolean __plistUniquingEqual(CFTypeRef cf1, CFTypeRef cf2) {
    // As long as this equals function is more restrictive than the
    // existing one, for any given type, the hash function need not
    // also be provided for the uniquing set.
    if (CFGetTypeID(cf1) != CFGetTypeID(cf2)) return false;
    if (CFGetTypeID(cf1) == CFNumberGetTypeID()) {
	if (CFNumberIsFloatType(cf1) != CFNumberIsFloatType(cf2)) return false;
	return CFEqual(cf1, cf2);
    }
    return CFEqual(cf1, cf2);
}

static void _flattenPlist(CFPropertyListRef plist, CFMutableArrayRef objlist, CFMutableDictionaryRef objtable, CFMutableSetRef uniquingset) {
    CFPropertyListRef unique;
    uint32_t refnum;
    CFTypeID type = CFGetTypeID(plist);
    CFIndex idx, before, after;
    CFPropertyListRef *list, buffer[256];

    // Do not unique dictionaries, because: they are
    // slow to compare, and produce poor hash codes.
    // Same is true for arrays, but we still unique them;
    // they aren't as slow.
    if (dicttype != type) {
	before = CFSetGetCount(uniquingset);
	CFSetAddValue(uniquingset, plist);
	after = CFSetGetCount(uniquingset);
	if (after == before) {	// already in set
	    unique = CFSetGetValue(uniquingset, plist);
	    if (unique != plist) {
		refnum = (uint32_t)CFDictionaryGetValue(objtable, unique);
		CFDictionaryAddValue(objtable, plist, (const void *)refnum);
	    }
	    return;
	}
    }
    refnum = CFArrayGetCount(objlist);
    CFArrayAppendValue(objlist, plist);
    CFDictionaryAddValue(objtable, plist, (const void *)refnum);
    if (dicttype == type) {
	CFIndex count = CFDictionaryGetCount(plist);
	list = (count <= 128) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, 2 * count * sizeof(CFTypeRef), 0);
        CFDictionaryGetKeysAndValues(plist, list, list + count);
        for (idx = 0; idx < 2 * count; idx++) {
            _flattenPlist(list[idx], objlist, objtable, uniquingset);
        }
        if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
    } else if (arraytype == type) {
	CFIndex count = CFArrayGetCount(plist);
	list = (count <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, count * sizeof(CFTypeRef), 0);
        CFArrayGetValues(plist, CFRangeMake(0, count), list);
        for (idx = 0; idx < count; idx++) {
            _flattenPlist(list[idx], objlist, objtable, uniquingset);
        }
        if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
    }
}

// stream must be a CFMutableDataRef
CFIndex __CFBinaryPlistWriteToStream(CFPropertyListRef plist, CFTypeRef stream) {
    CFMutableDictionaryRef objtable;
    CFMutableSetRef uniquingset;
    CFMutableArrayRef objlist;
    CFBinaryPlistTrailer trailer;
    uint64_t *offsets, length_so_far;
    uint64_t mask, refnum;
    int64_t idx, idx2, cnt;
    __CFBinaryPlistWriteBuffer *buf;
    CFSetCallBacks cb = kCFTypeSetCallBacks;

    if ((CFTypeID)-1 == stringtype) {
	stringtype = CFStringGetTypeID();
	datatype = CFDataGetTypeID();
	numbertype = CFNumberGetTypeID();
	booltype = CFBooleanGetTypeID();
	datetype = CFDateGetTypeID();
	dicttype = CFDictionaryGetTypeID();
	arraytype = CFArrayGetTypeID();
    }
    objtable = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, NULL);
    _CFDictionarySetCapacity(objtable, 320);
    objlist = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
    _CFArraySetCapacity(objlist, 320);
    cb.equal = __plistUniquingEqual;
    uniquingset = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &cb);
    _CFSetSetCapacity(uniquingset, 320);

    _flattenPlist(plist, objlist, objtable, uniquingset);
    
    CFRelease(uniquingset);

    cnt = CFArrayGetCount(objlist);
    offsets = CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(*offsets), 0);

    buf = CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(__CFBinaryPlistWriteBuffer), 0);
    buf->stream = stream;
    buf->streamIsData = (CFGetTypeID(stream) == CFDataGetTypeID());
    buf->written = 0;
    buf->used = 0;
    bufferWrite(buf, "bplist00", 8);	// header

    memset(&trailer, 0, sizeof(trailer));
    trailer._numObjects = CFSwapInt64HostToBig(cnt);
    trailer._topObject = 0;	// true for this implementation
    mask = ~(uint64_t)0;
    while (cnt & mask) {
	trailer._objectRefSize++;
	mask = mask << 8;
    }

    for (idx = 0; idx < cnt; idx++) {
	CFPropertyListRef obj = CFArrayGetValueAtIndex(objlist, idx);
	CFTypeID type = CFGetTypeID(obj);
	offsets[idx] = buf->written + buf->used;
	if (stringtype == type) {
	    CFIndex ret, count = CFStringGetLength(obj);
	    CFIndex needed;
	    uint8_t *bytes, buffer[1024];
	    bytes = (count <= 1024) ? buffer : CFAllocatorAllocate(kCFAllocatorDefault, count, 0);
	    // presumption, believed to be true, is that ASCII encoding may need
	    // less bytes, but will not need greater, than the # of unichars
	    ret = CFStringGetBytes(obj, CFRangeMake(0, count), kCFStringEncodingASCII, 0, false, bytes, count, &needed);
	    if (ret == count) {
		uint8_t marker = kCFBinaryPlistMarkerASCIIString | (needed < 15 ? needed : 0xf);
		bufferWrite(buf, &marker, 1);
		if (15 <= needed) {
		    _appendInt(buf, (uint64_t)needed);
		}
		bufferWrite(buf, bytes, needed);
	    } else {
		UniChar *chars;
		uint8_t marker = kCFBinaryPlistMarkerUnicode16String | (count < 15 ? count : 0xf);
		bufferWrite(buf, &marker, 1);
		if (15 <= count) {
		    _appendInt(buf, (uint64_t)count);
		}
		chars = CFAllocatorAllocate(kCFAllocatorDefault, count * sizeof(UniChar), 0);
		CFStringGetCharacters(obj, CFRangeMake(0, count), chars);
		for (idx2 = 0; idx2 < count; idx2++) {
		    chars[idx2] = CFSwapInt16HostToBig(chars[idx2]);
		}
		bufferWrite(buf, (uint8_t *)chars, count * sizeof(UniChar));
		CFAllocatorDeallocate(kCFAllocatorDefault, chars);
	    }
	    if (bytes != buffer) CFAllocatorDeallocate(kCFAllocatorDefault, bytes);
	} else if (numbertype == type) {
	    uint8_t marker;
	    CFSwappedFloat64 swapped64;
	    CFSwappedFloat32 swapped32;
	    uint64_t bigint;
	    uint8_t *bytes;
	    CFIndex nbytes;
	    if (CFNumberIsFloatType(obj)) {
		if (CFNumberGetByteSize(obj) <= (CFIndex)sizeof(float)) {
		    float v;
		    CFNumberGetValue(obj, kCFNumberFloat32Type, &v);
		    swapped32 = CFConvertFloat32HostToSwapped(v);
		    bytes = (uint8_t *)&swapped32;
		    nbytes = sizeof(float);
		    marker = kCFBinaryPlistMarkerReal | 2;
		} else {
		    double v;
		    CFNumberGetValue(obj, kCFNumberFloat64Type, &v);
		    swapped64 = CFConvertFloat64HostToSwapped(v);
		    bytes = (uint8_t *)&swapped64;
		    nbytes = sizeof(double);
		    marker = kCFBinaryPlistMarkerReal | 3;
		}
		bufferWrite(buf, &marker, 1);
		bufferWrite(buf, bytes, nbytes);
	    } else {
		CFNumberGetValue(obj, kCFNumberSInt64Type, &bigint);
		_appendInt(buf, bigint);
	    }
	} else if (_CFKeyedArchiverUIDGetTypeID() == type) {
	    _appendUID(buf, (CFKeyedArchiverUIDRef)obj);
	} else if (booltype == type) {
	    uint8_t marker = CFBooleanGetValue(obj) ? kCFBinaryPlistMarkerTrue : kCFBinaryPlistMarkerFalse;
	    bufferWrite(buf, &marker, 1);
	} else if (datatype == type) {
	    CFIndex count = CFDataGetLength(obj);
	    uint8_t marker = kCFBinaryPlistMarkerData | (count < 15 ? count : 0xf);
	    bufferWrite(buf, &marker, 1);
	    if (15 <= count) {
		_appendInt(buf, (uint64_t)count);
	    }
	    bufferWrite(buf, CFDataGetBytePtr(obj), count);
	} else if (datetype == type) {
	    CFSwappedFloat64 swapped;
	    uint8_t marker = kCFBinaryPlistMarkerDate;
	    bufferWrite(buf, &marker, 1);
	    swapped = CFConvertFloat64HostToSwapped(CFDateGetAbsoluteTime(obj));
	    bufferWrite(buf, (uint8_t *)&swapped, sizeof(swapped));
	} else if (dicttype == type) {
	    CFIndex count = CFDictionaryGetCount(obj);
	    CFPropertyListRef *list, buffer[512];
	    uint8_t marker = kCFBinaryPlistMarkerDict | (count < 15 ? count : 0xf);
	    bufferWrite(buf, &marker, 1);
	    if (15 <= count) {
		_appendInt(buf, (uint64_t)count);
	    }
	    list = (count <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, 2 * count * sizeof(CFTypeRef), 0);
	    CFDictionaryGetKeysAndValues(obj, list, list + count);
	    for (idx2 = 0; idx2 < 2 * count; idx2++) {
		CFPropertyListRef value = list[idx2];
		uint32_t swapped = 0;
		uint8_t *source = (uint8_t *)&swapped;
                refnum = (uint32_t)CFDictionaryGetValue(objtable, value);
                swapped = CFSwapInt32HostToBig(refnum);
		bufferWrite(buf, source + sizeof(swapped) - trailer._objectRefSize, trailer._objectRefSize);
	    }
	    if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
	} else if (arraytype == type) {
	    CFIndex count = CFArrayGetCount(obj);
	    CFPropertyListRef *list, buffer[256];
	    uint8_t marker = kCFBinaryPlistMarkerArray | (count < 15 ? count : 0xf);
	    bufferWrite(buf, &marker, 1);
	    if (15 <= count) {
		_appendInt(buf, (uint64_t)count);
	    }
	    list = (count <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, count * sizeof(CFTypeRef), 0);
	    CFArrayGetValues(obj, CFRangeMake(0, count), list);
	    for (idx2 = 0; idx2 < count; idx2++) {
		CFPropertyListRef value = list[idx2];
		uint32_t swapped = 0;
		uint8_t *source = (uint8_t *)&swapped;
                refnum = (uint32_t)CFDictionaryGetValue(objtable, value);
                swapped = CFSwapInt32HostToBig(refnum);
		bufferWrite(buf, source + sizeof(swapped) - trailer._objectRefSize, trailer._objectRefSize);
	    }
	    if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
	} else {
	    CFRelease(objtable);
	    CFRelease(objlist);
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
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, buf);
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, offsets);
    return (CFIndex)length_so_far;
}

bool __CFBinaryPlistGetTopLevelInfo(CFDataRef data, uint8_t *marker, uint64_t *offset, CFBinaryPlistTrailer *trailer) {
    const uint8_t *databytes, *bytesptr;
    uint64_t datalen;
    CFBinaryPlistTrailer trail;
    uint64_t off;
    CFIndex idx;

    if ((CFTypeID)-1 == stringtype) {
	stringtype = CFStringGetTypeID();
	datatype = CFDataGetTypeID();
	numbertype = CFNumberGetTypeID();
	booltype = CFBooleanGetTypeID();
	datetype = CFDateGetTypeID();
	dicttype = CFDictionaryGetTypeID();
	arraytype = CFArrayGetTypeID();
    }
    databytes = CFDataGetBytePtr(data);
    datalen = CFDataGetLength(data);
    if (!databytes || datalen < 8 || 0 != memcmp("bplist00", databytes, 8)) return false;
    if (datalen < sizeof(trail) + 8 + 1) return false;
    memmove(&trail, databytes + datalen - sizeof(trail), sizeof(trail));
    trail._numObjects = CFSwapInt64BigToHost(trail._numObjects);
    trail._topObject = CFSwapInt64BigToHost(trail._topObject);
    if (trail._numObjects < trail._topObject) return false;
    trail._offsetTableOffset = CFSwapInt64BigToHost(trail._offsetTableOffset);
    if (datalen < trail._offsetTableOffset + trail._numObjects * trail._offsetIntSize + sizeof(trail)) return false;
    bytesptr = databytes + trail._offsetTableOffset + trail._topObject * trail._offsetIntSize;
    off = 0;
    for (idx = 0; idx < trail._offsetIntSize; idx++) {
	off = (off << 8) + bytesptr[idx];
    }
    if (trail._offsetTableOffset <= off) return false;
    if (trailer) *trailer = trail;
    if (offset) *offset = off;
    if (marker) *marker = *(databytes + off);
    return true;
}

static bool _readInt(const uint8_t *ptr, uint64_t *bigint, const uint8_t **newptr) {
    uint8_t marker;
    CFIndex idx, cnt;
    marker = *ptr++;
    if ((marker & 0xf0) != kCFBinaryPlistMarkerInt) return false;
    cnt = 1 << (marker & 0xf);
    *bigint = 0;
    for (idx = 0; idx < cnt; idx++) {
	*bigint = (*bigint << 8) + *ptr++;
    }
    if (newptr) *newptr = ptr;
    return true;
}

static uint64_t _getOffsetOfRefAt(const uint8_t *databytes, const uint8_t *bytesptr, const CFBinaryPlistTrailer *trailer) {
    uint64_t ref = 0, off = 0;
    CFIndex idx;
    for (idx = 0; idx < trailer->_objectRefSize; idx++) {
	ref = (ref << 8) + bytesptr[idx];
    }
    bytesptr = databytes + trailer->_offsetTableOffset + ref * trailer->_offsetIntSize;
    for (idx = 0; idx < trailer->_offsetIntSize; idx++) {
	off = (off << 8) + bytesptr[idx];
    }
    return off;
}

bool __CFBinaryPlistGetOffsetForValueFromArray(CFDataRef data, uint64_t startOffset, const CFBinaryPlistTrailer *trailer, CFIndex idx, uint64_t *offset) {
    const uint8_t *databytes, *bytesptr;
    uint8_t marker;
    CFIndex cnt;
    uint64_t off;
    databytes = CFDataGetBytePtr(data);
    marker = *(databytes + startOffset);
    if ((marker & 0xf0) != kCFBinaryPlistMarkerArray) return false;
    cnt = (marker & 0x0f);
    if (cnt < 15 && cnt <= idx) return false;
    bytesptr = databytes + startOffset + 1;
    if (0xf == cnt) {
	uint64_t bigint;
	if (!_readInt(bytesptr, &bigint, &bytesptr)) return false;
	if (INT_MAX < bigint) return false;
	cnt = (CFIndex)bigint;
    }
    if (cnt <= idx) return false;
    off = _getOffsetOfRefAt(databytes, bytesptr + idx * trailer->_objectRefSize, trailer);
    if ((uint64_t)CFDataGetLength(data) <= off) return false;
    if (offset) *offset = off;
    return true;
}

bool __CFBinaryPlistGetOffsetForValueFromDictionary(CFDataRef data, uint64_t startOffset, const CFBinaryPlistTrailer *trailer, CFTypeRef key, uint64_t *koffset, uint64_t *voffset) {
    const uint8_t *databytes, *refsptr, *bytesptr;
    uint64_t off;
    uint8_t marker;
    CFTypeID keytype = CFGetTypeID(key);
    CFIndex idx, keyn, cnt, cnt2;

    databytes = CFDataGetBytePtr(data);
    marker = *(databytes + startOffset);
    if ((marker & 0xf0) != kCFBinaryPlistMarkerDict) return false;
    cnt = (marker & 0x0f);
    refsptr = databytes + startOffset + 1 + 0;
    if (0xf == cnt) {
	uint64_t bigint;
	if (!_readInt(refsptr, &bigint, &refsptr)) return false;
	if (INT_MAX < bigint) return false;
	cnt = (CFIndex)bigint;
    }
    for (keyn = 0; keyn < cnt; keyn++) {
	off = _getOffsetOfRefAt(databytes, refsptr, trailer);
	if ((uint64_t)CFDataGetLength(data) <= off) return false;
	refsptr += trailer->_objectRefSize;
	bytesptr = databytes + off;
	marker = *bytesptr & 0xf0;
	cnt2 = *bytesptr & 0x0f;
	if (kCFBinaryPlistMarkerASCIIString == marker || kCFBinaryPlistMarkerUnicode16String == marker) {
	    CFStringInlineBuffer strbuf;
	    UniChar uchar;
	    if (keytype != stringtype) goto miss;
            if (0xf == cnt2 && CFStringGetLength(key) < 15) goto miss;
	    bytesptr++;
	    if (0xf == cnt2) {
		uint64_t bigint;
		if (!_readInt(bytesptr, &bigint, &bytesptr)) return false;
		if (INT_MAX < bigint) return false;
		cnt2 = (CFIndex)bigint;
	    }
	    if (cnt2 != CFStringGetLength(key)) goto miss;
	    uchar = (kCFBinaryPlistMarkerASCIIString == marker) ? (UniChar)bytesptr[0] : (UniChar)(bytesptr[0] * 256 + bytesptr[1]);
	    if (uchar != CFStringGetCharacterAtIndex(key, 0)) goto miss;
	    bytesptr += (kCFBinaryPlistMarkerASCIIString == marker) ? 1 : 2;
	    CFStringInitInlineBuffer(key, &strbuf, CFRangeMake(0, cnt2));
	    for (idx = 1; idx < cnt2; idx++) {
		uchar = (kCFBinaryPlistMarkerASCIIString == marker) ? (UniChar)bytesptr[0] : (UniChar)(bytesptr[0] * 256 + bytesptr[1]);
		if (uchar != __CFStringGetCharacterFromInlineBufferQuick(&strbuf, idx)) goto miss;
		bytesptr += (kCFBinaryPlistMarkerASCIIString == marker) ? 1 : 2;
	    }
	    if (koffset) *koffset = off;
	    off = _getOffsetOfRefAt(databytes, refsptr + (cnt - 1) * trailer->_objectRefSize, trailer);
	    if ((uint64_t)CFDataGetLength(data) <= off) return false;
	    if (voffset) *voffset = off;
	    return true;
	} else {
//#warning the other primitive types should be allowed as keys in a binary plist dictionary, I think
	    return false;
	}
	miss: ;
    }
    return false;
}

extern CFArrayRef _CFArrayCreate_ex(CFAllocatorRef allocator, bool mutable, const void **values, CFIndex numValues);

extern CFDictionaryRef _CFDictionaryCreate_ex(CFAllocatorRef allocator, bool mutable, const void **keys, const void **values, CFIndex numValues);

#if 0
static bool _getUIDFromData(const uint8_t *datap, uint64_t *vp) {
    int32_t idx, cnt;
    uint8_t marker = *datap;
    uint64_t bigint;
    if ((marker & 0xf0) != kCFBinaryPlistMarkerUID) return false;
    cnt = (marker & 0x0f) + 1;
    datap++;
    bigint = 0;
    for (idx = 0; idx < cnt; idx++) {
	bigint = (bigint << 8) + *datap++;
    }
    *vp = bigint;
    return true;
}
#endif

static bool _getFloatFromData(const uint8_t *datap, float *vp) {
    CFSwappedFloat32 swapped32;
    if (*datap != (kCFBinaryPlistMarkerReal | 2)) return false;
    datap++;
    memmove(&swapped32, datap, sizeof(swapped32));
    *vp = CFConvertFloat32SwappedToHost(swapped32);
    return true;
}

static bool _getDoubleFromData(const uint8_t *datap, double *vp) {
    CFSwappedFloat64 swapped64;
    if (*datap != (kCFBinaryPlistMarkerReal | 3)) return false;
    datap++;
    memmove(&swapped64, datap, sizeof(swapped64));
    *vp = CFConvertFloat64SwappedToHost(swapped64);
    return true;
}

bool __CFBinaryPlistCreateObject(CFDataRef data, uint64_t startOffset, const CFBinaryPlistTrailer *trailer, CFAllocatorRef allocator, CFOptionFlags mutabilityOption, CFMutableDictionaryRef objects, CFPropertyListRef *plist) {
    const uint8_t *databytes, *bytesptr;
    uint64_t off;
    uint8_t marker;
    CFIndex idx, cnt;
    uint64_t bigint;
    UniChar *chars;
    CFPropertyListRef *list, buffer[256];

    if (objects) {
	*plist = CFDictionaryGetValue(objects, (const void *)(intptr_t)startOffset);
	if (*plist) {
	    CFRetain(*plist);
	    return true;
	}
    }

    databytes = CFDataGetBytePtr(data);
    marker = *(databytes + startOffset);
    switch (marker & 0xf0) {
    case kCFBinaryPlistMarkerNull:
	switch (marker) {
	case kCFBinaryPlistMarkerNull:
	    *plist = NULL;
	    return true;
	case kCFBinaryPlistMarkerFalse:
	    *plist = CFRetain(kCFBooleanFalse);
	    return true;
	case kCFBinaryPlistMarkerTrue:
	    *plist = CFRetain(kCFBooleanTrue);
	    return true;
	}
	return false;
    case kCFBinaryPlistMarkerInt:
	if (!_readInt(databytes + startOffset, &bigint, NULL)) return false;
	*plist = CFNumberCreate(allocator, kCFNumberSInt64Type, &bigint);
	if (objects) CFDictionarySetValue(objects, (const void *)(intptr_t)startOffset, *plist);
	return (*plist) ? true : false;
    case kCFBinaryPlistMarkerReal:
	cnt = marker & 0x0f;
	if (2 == cnt) {
	    float f;
	    _getFloatFromData(databytes + startOffset, &f);
	    *plist = CFNumberCreate(allocator, kCFNumberFloat32Type, &f);
	    if (objects) CFDictionarySetValue(objects, (const void *)(intptr_t)startOffset, *plist);
	    return (*plist) ? true : false;
	} else if (3 == cnt) {
	    double d;
	    _getDoubleFromData(databytes + startOffset, &d);
	    *plist = CFNumberCreate(allocator, kCFNumberFloat64Type, &d);
	    if (objects) CFDictionarySetValue(objects, (const void *)(intptr_t)startOffset, *plist);
	    return (*plist) ? true : false;
	}
	return false;
    case kCFBinaryPlistMarkerDate & 0xf0: {
	CFSwappedFloat64 swapped64;
	double d;
	cnt = marker & 0x0f;
	if (3 != cnt) return false;
	memmove(&swapped64, databytes + startOffset + 1, sizeof(swapped64));
	d = CFConvertFloat64SwappedToHost(swapped64);
	*plist = CFDateCreate(allocator, d);
	if (objects) CFDictionarySetValue(objects, (const void *)(intptr_t)startOffset, *plist);
	return (*plist) ? true : false;
	}
    case kCFBinaryPlistMarkerData: 
	cnt = marker & 0x0f;
	bytesptr = databytes + startOffset + 1;
	if (0xf == cnt) {
	    if (!_readInt(bytesptr, &bigint, &bytesptr)) return false;
	    if (INT_MAX < bigint) return false;
	    cnt = (CFIndex)bigint;
	}
	if (mutabilityOption == kCFPropertyListMutableContainersAndLeaves) {
	    *plist = CFDataCreateMutable(allocator, 0);
	    CFDataAppendBytes((CFMutableDataRef)*plist, bytesptr, cnt);
	} else {
	    *plist = CFDataCreate(allocator, bytesptr, cnt);
	}
	if (objects) CFDictionarySetValue(objects, (const void *)(intptr_t)startOffset, *plist);
	return (*plist) ? true : false;
    case kCFBinaryPlistMarkerASCIIString:
	cnt = marker & 0x0f;
	bytesptr = databytes + startOffset + 1;
	if (0xf == cnt) {
	    if (!_readInt(bytesptr, &bigint, &bytesptr)) return false;
	    if (INT_MAX < bigint) return false;
	    cnt = (CFIndex)bigint;
	}
	if (mutabilityOption == kCFPropertyListMutableContainersAndLeaves) {
	    CFStringRef str = CFStringCreateWithBytes(allocator, bytesptr, cnt, kCFStringEncodingASCII, false);
	    *plist = CFStringCreateMutableCopy(allocator, 0, str);
	    CFRelease(str);
	} else {
	    *plist = CFStringCreateWithBytes(allocator, bytesptr, cnt, kCFStringEncodingASCII, false);
	}
	if (objects) CFDictionarySetValue(objects, (const void *)(intptr_t)startOffset, *plist);
	return (*plist) ? true : false;
    case kCFBinaryPlistMarkerUnicode16String:
	cnt = marker & 0x0f;
	bytesptr = databytes + startOffset + 1;
	if (0xf == cnt) {
	    if (!_readInt(bytesptr, &bigint, &bytesptr)) return false;
	    if (INT_MAX < bigint) return false;
	    cnt = (CFIndex)bigint;
	}
	chars = CFAllocatorAllocate(allocator, cnt * sizeof(UniChar), 0);
	memmove(chars, bytesptr, cnt * sizeof(UniChar));
	for (idx = 0; idx < cnt; idx++) {
	    chars[idx] = CFSwapInt16BigToHost(chars[idx]);
	}
	if (mutabilityOption == kCFPropertyListMutableContainersAndLeaves) {
	    CFStringRef str = CFStringCreateWithCharactersNoCopy(allocator, chars, cnt, allocator);
	    *plist = CFStringCreateMutableCopy(allocator, 0, str);
	    CFRelease(str);
	} else {
	    *plist = CFStringCreateWithCharactersNoCopy(allocator, chars, cnt, allocator);
	}
	if (objects) CFDictionarySetValue(objects, (const void *)(intptr_t)startOffset, *plist);
	return (*plist) ? true : false;
    case kCFBinaryPlistMarkerUID:
	cnt = (marker & 0x0f) + 1;
	bytesptr = databytes + startOffset + 1;
	bigint = 0;
	for (idx = 0; idx < cnt; idx++) {
	    bigint = (bigint << 8) + *bytesptr++;
	}
	if (UINT_MAX < bigint) return false;
	*plist = _CFKeyedArchiverUIDCreate(allocator, (uint32_t)bigint);
	if (objects) CFDictionarySetValue(objects, (const void *)(intptr_t)startOffset, *plist);
	return (*plist) ? true : false;
    case kCFBinaryPlistMarkerArray:
	cnt = marker & 0x0f;
	bytesptr = databytes + startOffset + 1;
	if (0xf == cnt) {
	    if (!_readInt(bytesptr, &bigint, &bytesptr)) return false;
	    if (INT_MAX < bigint) return false;
	    cnt = (CFIndex)bigint;
	}
	list = (cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(CFPropertyListRef) * cnt, 0);
	for (idx = 0; idx < cnt; idx++) {
	    CFPropertyListRef pl;
	    off = _getOffsetOfRefAt(databytes, bytesptr, trailer);
	    if ((uint64_t)CFDataGetLength(data) <= off) return false;
	    if (!__CFBinaryPlistCreateObject(data, off, trailer, allocator, mutabilityOption, objects, &pl)) {
		while (idx--) {
		    CFRelease(list[idx]);
		}
		if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
		return false;
	    }
	    list[idx] = pl;
	    bytesptr += trailer->_objectRefSize;
	}
	*plist = _CFArrayCreate_ex(allocator, (mutabilityOption != kCFPropertyListImmutable), list, cnt);
	if (objects) CFDictionarySetValue(objects, (const void *)(intptr_t)startOffset, *plist);
	if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
	return (*plist) ? true : false;
    case kCFBinaryPlistMarkerDict:
	cnt = marker & 0x0f;
	bytesptr = databytes + startOffset + 1;
	if (0xf == cnt) {
	    if (!_readInt(bytesptr, &bigint, &bytesptr)) return false;
	    if (INT_MAX < bigint) return false;
	    cnt = (CFIndex)bigint;
	}
	cnt *= 2;
	list = (cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(CFPropertyListRef) * cnt, 0);
	for (idx = 0; idx < cnt; idx++) {
	    CFPropertyListRef pl;
	    off = _getOffsetOfRefAt(databytes, bytesptr, trailer);
	    if ((uint64_t)CFDataGetLength(data) <= off) return false;
	    if (!__CFBinaryPlistCreateObject(data, off, trailer, allocator, mutabilityOption, objects, &pl)) {
		while (idx--) {
		    CFRelease(list[idx]);
		}
		if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
		return false;
	    }
	    list[idx] = pl;
	    bytesptr += trailer->_objectRefSize;
	}
	*plist = _CFDictionaryCreate_ex(allocator, (mutabilityOption != kCFPropertyListImmutable), list, list + cnt / 2, cnt / 2);
	if (objects) CFDictionarySetValue(objects, (const void *)(intptr_t)startOffset, *plist);
	if (list != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
	return (*plist) ? true : false;
    }
    return false;
}

__private_extern__ bool __CFTryParseBinaryPlist(CFAllocatorRef allocator, CFDataRef data, CFOptionFlags option, CFPropertyListRef *plist, CFStringRef *errorString) {
    uint8_t marker;    
    CFBinaryPlistTrailer trailer;
    uint64_t offset;
    CFPropertyListRef pl;

    if (8 <= CFDataGetLength(data) && __CFBinaryPlistGetTopLevelInfo(data, &marker, &offset, &trailer)) {
        if (__CFBinaryPlistCreateObject(data, offset, &trailer, allocator, option, NULL, &pl)) {
	    if (plist) *plist = pl;
        } else {
	    if (plist) *plist = NULL;
            if (errorString) *errorString = CFRetain(CFSTR("binary data is corrupt"));
	}
        return true;
    }
    return false;
}

