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
/*	CFNumber.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Ali Ozer
*/

#include <CoreFoundation/CFNumber.h>
#include "CFInternal.h"
#include "CFUtilities.h"
#include <math.h>
#include <float.h>
#if defined(__WIN32__)
#define isnan _isnan
#define isinf !_finite
#endif

/* Various assertions
*/
#define __CFAssertIsBoolean(cf) __CFGenericValidateType(cf, __kCFBooleanTypeID)
#define __CFAssertIsNumber(cf) __CFGenericValidateType(cf, __kCFNumberTypeID)
#define __CFAssertIsValidNumberType(type) CFAssert2((type > 0 && type <= kCFNumberMaxType && __CFNumberCanonicalType[type]), __kCFLogAssertion, "%s(): bad CFNumber type %d", __PRETTY_FUNCTION__, type);
#define __CFInvalidNumberStorageType(type) CFAssert2(true, __kCFLogAssertion, "%s(): bad CFNumber storage type %d", __PRETTY_FUNCTION__, type);

/* The IEEE bit patterns... Also have:
0x7f800000		float +Inf
0x7fc00000		float NaN
0xff800000		float -Inf
*/
#define BITSFORDOUBLENAN	((uint64_t)0x7ff8000000000000ULL)
#define BITSFORDOUBLEPOSINF	((uint64_t)0x7ff0000000000000ULL)
#define BITSFORDOUBLENEGINF	((uint64_t)0xfff0000000000000ULL)

struct __CFBoolean {
    CFRuntimeBase _base;
};

static struct __CFBoolean __kCFBooleanTrue = {
    INIT_CFRUNTIME_BASE(NULL, 0, 0x0080)
};
const CFBooleanRef kCFBooleanTrue = &__kCFBooleanTrue;

static struct __CFBoolean __kCFBooleanFalse = {
    INIT_CFRUNTIME_BASE(NULL, 0, 0x0080)
};
const CFBooleanRef kCFBooleanFalse = &__kCFBooleanFalse;

static CFStringRef __CFBooleanCopyDescription(CFTypeRef cf) {
    CFBooleanRef boolean = (CFBooleanRef)cf;
    return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("<CFBoolean %p [%p]>{value = %s}"), cf, CFGetAllocator(cf), (boolean == kCFBooleanTrue) ? "true" : "false");
}

static CFStringRef __CFBooleanCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    CFBooleanRef boolean = (CFBooleanRef)cf;
    return CFRetain((boolean == kCFBooleanTrue) ? CFSTR("true") : CFSTR("false"));
}

static void __CFBooleanDeallocate(CFTypeRef cf) {
    CFAssert(false, __kCFLogAssertion, "Deallocated CFBoolean!");
}

static CFTypeID __kCFBooleanTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFBooleanClass = {
    0,
    "CFBoolean",
    NULL,      // init
    NULL,      // copy
    __CFBooleanDeallocate,
    NULL,
    NULL,
    __CFBooleanCopyFormattingDescription,
    __CFBooleanCopyDescription
};

__private_extern__ void __CFBooleanInitialize(void) {
    __kCFBooleanTypeID = _CFRuntimeRegisterClass(&__CFBooleanClass);
    _CFRuntimeSetInstanceTypeID(&__kCFBooleanTrue, __kCFBooleanTypeID);
    __kCFBooleanTrue._base._isa = __CFISAForTypeID(__kCFBooleanTypeID);
    _CFRuntimeSetInstanceTypeID(&__kCFBooleanFalse, __kCFBooleanTypeID);
    __kCFBooleanFalse._base._isa = __CFISAForTypeID(__kCFBooleanTypeID);
}

CFTypeID CFBooleanGetTypeID(void) {
    return __kCFBooleanTypeID;
}

Boolean CFBooleanGetValue(CFBooleanRef boolean) {
    CF_OBJC_FUNCDISPATCH0(__kCFBooleanTypeID, Boolean, boolean, "boolValue");
    return (boolean == kCFBooleanTrue) ? true : false;
}


/*** CFNumber ***/

typedef union {
    SInt32 valSInt32;
    int64_t valSInt64;
    Float32 valFloat32;
    Float64 valFloat64;
} __CFNumberValue;

struct __CFNumber {		/* Only as many bytes as necessary are allocated */
    CFRuntimeBase _base;
    __CFNumberValue value;
};

static struct __CFNumber __kCFNumberNaN = {
    INIT_CFRUNTIME_BASE(NULL, 0, 0x0080), {0}
};
const CFNumberRef kCFNumberNaN = &__kCFNumberNaN;

static struct __CFNumber __kCFNumberNegativeInfinity = {
    INIT_CFRUNTIME_BASE(NULL, 0, 0x0080), {0}
};
const CFNumberRef kCFNumberNegativeInfinity = &__kCFNumberNegativeInfinity;

static struct __CFNumber __kCFNumberPositiveInfinity = {
    INIT_CFRUNTIME_BASE(NULL, 0, 0x0080), {0}
};
const CFNumberRef kCFNumberPositiveInfinity = &__kCFNumberPositiveInfinity;


/* Eight bits in base:
    Bits 6..0: type (bits 4..0 is CFNumberType)
*/
enum {
    __kCFNumberIsPositiveInfinity = (0x20 | kCFNumberFloat64Type),
    __kCFNumberIsNegativeInfinity = (0x40 | kCFNumberFloat64Type),
    __kCFNumberIsNaN = (0x60 | kCFNumberFloat64Type)
};


/* ??? These tables should be changed on different architectures, depending on the actual sizes of basic C types such as int, long, float; also size of CFIndex. We can probably compute these tables at runtime.
*/

/* Canonical types are the types that the implementation knows how to deal with. There should be one for each type that is distinct; so this table basically is a type equivalence table. All functions that take a type from the outside world should call __CFNumberGetCanonicalTypeForType() before doing anything with it.
*/
static const unsigned char __CFNumberCanonicalType[kCFNumberMaxType + 1] = {
    0, kCFNumberSInt8Type, kCFNumberSInt16Type, kCFNumberSInt32Type, kCFNumberSInt64Type, kCFNumberFloat32Type, kCFNumberFloat64Type,
    kCFNumberSInt8Type, kCFNumberSInt16Type, kCFNumberSInt32Type, kCFNumberSInt32Type, kCFNumberSInt64Type, kCFNumberFloat32Type, kCFNumberFloat64Type,
    kCFNumberSInt32Type
};

/* This table determines what storage format is used for any given type.
   !!! These are the only types that can occur in the types field of a CFNumber.
   !!! If the number or kind of types returned by this array changes, also need to fix NSNumber and NSCFNumber.
*/
static const unsigned char __CFNumberStorageType[kCFNumberMaxType + 1] = {
    0, kCFNumberSInt32Type, kCFNumberSInt32Type, kCFNumberSInt32Type, kCFNumberSInt64Type, kCFNumberFloat32Type, kCFNumberFloat64Type,
    kCFNumberSInt32Type, kCFNumberSInt32Type, kCFNumberSInt32Type, kCFNumberSInt32Type, kCFNumberSInt64Type, kCFNumberFloat32Type, kCFNumberFloat64Type,
    kCFNumberSInt32Type
};

// Returns the type that is used to store the specified type
CF_INLINE CFNumberType __CFNumberGetStorageTypeForType(CFNumberType type) {
    return __CFNumberStorageType[type];
}

// Returns the canonical type used to represent the specified type
CF_INLINE CFNumberType __CFNumberGetCanonicalTypeForType(CFNumberType type) {
    return __CFNumberCanonicalType[type];
}

// Extracts and returns the type out of the CFNumber
CF_INLINE CFNumberType __CFNumberGetType(CFNumberRef num) {
    return __CFBitfieldGetValue(num->_base._info, 4, 0);
}

// Returns true if the argument type is float or double
CF_INLINE Boolean __CFNumberTypeIsFloat(CFNumberType type) {
    return (type == kCFNumberFloat64Type) || (type == kCFNumberFloat32Type) || (type == kCFNumberDoubleType) || (type == kCFNumberFloatType);
}

// Returns the number of bytes necessary to store the specified type
// Needs to handle all canonical types
CF_INLINE CFIndex __CFNumberSizeOfType(CFNumberType type) {
    switch (type) {
        case kCFNumberSInt8Type:	return sizeof(int8_t);
        case kCFNumberSInt16Type:	return sizeof(int16_t);
        case kCFNumberSInt32Type:	return sizeof(SInt32);
        case kCFNumberSInt64Type:	return sizeof(int64_t);
        case kCFNumberFloat32Type:	return sizeof(Float32);
        case kCFNumberFloat64Type:	return sizeof(Float64);
        default:			return 0;
    }
}

// Copies an external value of a given type into the appropriate slot in the union (does no type conversion)
// Needs to handle all canonical types
#define SET_VALUE(valueUnion, type, valuePtr)	\
    switch (type) {				\
        case kCFNumberSInt8Type:	(valueUnion)->valSInt32 = *(int8_t *)(valuePtr); break;	\
        case kCFNumberSInt16Type:	(valueUnion)->valSInt32 = *(int16_t *)(valuePtr); break;	\
        case kCFNumberSInt32Type:	(valueUnion)->valSInt32 = *(SInt32 *)(valuePtr); break;	\
        case kCFNumberSInt64Type:	(valueUnion)->valSInt64 = *(int64_t *)(valuePtr); break;	\
        case kCFNumberFloat32Type:	(valueUnion)->valFloat32 = *(Float32 *)(valuePtr); break;	\
        case kCFNumberFloat64Type:	(valueUnion)->valFloat64 = *(Float64 *)(valuePtr); break;	\
        default: break;	\
    }

// Casts the specified value into the specified type and copies it into the provided memory
// Needs to handle all canonical types
#define GET_VALUE(value, type, resultPtr)	\
    switch (type) {				\
        case kCFNumberSInt8Type:	*(int8_t *)(valuePtr) = (int8_t)value; break;	\
        case kCFNumberSInt16Type:	*(int16_t *)(valuePtr) = (int16_t)value; break;	\
        case kCFNumberSInt32Type:	*(SInt32 *)(valuePtr) = (SInt32)value; break;	\
        case kCFNumberSInt64Type:	*(int64_t *)(valuePtr) = (int64_t)value; break;	\
        case kCFNumberFloat32Type:	*(Float32 *)(valuePtr) = (Float32)value; break;	\
        case kCFNumberFloat64Type:	*(Float64 *)(valuePtr) = (Float64)value; break;	\
	default: break;	\
    }

// Extracts the stored type out of the union and copies it in the desired type into the provided memory
// Needs to handle all storage types
CF_INLINE void __CFNumberGetValue(const __CFNumberValue *value, CFNumberType numberType, CFNumberType typeToGet, void *valuePtr) {
    switch (numberType) {
        case kCFNumberSInt32Type:	GET_VALUE(value->valSInt32, typeToGet, resultPtr); break;	
        case kCFNumberSInt64Type:	GET_VALUE(value->valSInt64, typeToGet, resultPtr); break;	
        case kCFNumberFloat32Type:	GET_VALUE(value->valFloat32, typeToGet, resultPtr); break;
        case kCFNumberFloat64Type:	GET_VALUE(value->valFloat64, typeToGet, resultPtr); break;
	default: break;	\
    }
}

// Sees if two value union structs have the same value (will do type conversion)
static Boolean __CFNumberEqualValue(const __CFNumberValue *value1, CFNumberType type1, const __CFNumberValue *value2, CFNumberType type2) {
    if (__CFNumberTypeIsFloat(type1) || __CFNumberTypeIsFloat(type2)) {
        Float64 d1, d2;
        __CFNumberGetValue(value1, type1, kCFNumberFloat64Type, &d1);
        __CFNumberGetValue(value2, type2, kCFNumberFloat64Type, &d2);
	if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    if (isnan(d1) && isnan(d2)) return true;	// Not mathematically sound, but required
	}
        return d1 == d2;
    } else {
        int64_t i1, i2;
        __CFNumberGetValue(value1, type1, kCFNumberSInt64Type, &i1);
        __CFNumberGetValue(value2, type2, kCFNumberSInt64Type, &i2);
        return i1 == i2;
    }
}

static Boolean __CFNumberEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFNumberRef number1 = (CFNumberRef)cf1;
    CFNumberRef number2 = (CFNumberRef)cf2;
    return __CFNumberEqualValue(&(number1->value), __CFNumberGetType(number1), &(number2->value), __CFNumberGetType(number2));
}

static CFHashCode __CFNumberHash(CFTypeRef cf) {
    CFNumberRef number = (CFNumberRef)cf;
    switch (__CFNumberGetType(cf)) {
        case kCFNumberSInt32Type: return _CFHashInt(number->value.valSInt32);
        case kCFNumberSInt64Type: return _CFHashDouble((double)(number->value.valSInt64));
        case kCFNumberFloat32Type: return _CFHashDouble((double)(number->value.valFloat32));
        case kCFNumberFloat64Type: return _CFHashDouble((double)(number->value.valFloat64));
        default: 
	    __CFInvalidNumberStorageType(__CFNumberGetType(cf));
	    return 0;
    }
}

#define bufSize 100
#define emitChar(ch) \
    {if (buf - stackBuf == bufSize) {CFStringAppendCharacters(mstr, stackBuf, bufSize); buf = stackBuf;} *buf++ = ch;}
                 
static void __CFNumberEmitInt64(CFMutableStringRef mstr, int64_t value, int32_t width, UniChar pad, bool explicitPlus) {
    UniChar stackBuf[bufSize], *buf = stackBuf;
    uint64_t uvalue, factor, tmp;
    int32_t w;
    bool neg;

    neg = (value < 0) ? true : false;
    uvalue = (neg) ? -value : value;
    if (neg || explicitPlus) width--;
    width--;
    factor = 1;
    tmp = uvalue;
    while (9 < tmp) {
	width--;
	factor *= 10;
	tmp /= 10;
    }
    for (w = 0; w < width; w++) emitChar(pad);
    if (neg) {
        emitChar('-');
    } else if (explicitPlus) {
        emitChar('+');
    }
    while (0 < factor) {
	UniChar ch = '0' + (uvalue / factor);
	uvalue %= factor;
        emitChar(ch);
	factor /= 10;
    }
    if (buf > stackBuf) CFStringAppendCharacters(mstr, stackBuf, buf - stackBuf);
}

static CFStringRef __CFNumberCopyDescription(CFTypeRef cf) {
    CFNumberRef number = (CFNumberRef)cf;
    CFMutableStringRef mstr = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    CFStringAppendFormat(mstr, NULL, CFSTR("<CFNumber %p [%p]>{value = "), cf, CFGetAllocator(cf));
    switch (__CFNumberGetType(number)) {
    case kCFNumberSInt32Type:
	__CFNumberEmitInt64(mstr, number->value.valSInt32, 0, ' ', true);
	CFStringAppendFormat(mstr, NULL, CFSTR(", type = kCFNumberSInt32Type}"));
	break;
    case kCFNumberSInt64Type:
	__CFNumberEmitInt64(mstr, number->value.valSInt64, 0, ' ', true);
	CFStringAppendFormat(mstr, NULL, CFSTR(", type = kCFNumberSInt64Type}"));
	break;
    case kCFNumberFloat32Type:
	// debugging formatting is intentionally more verbose and explicit about the value of the number
	if (isnan(number->value.valFloat32)) {
	    CFStringAppend(mstr, CFSTR("nan"));
	} else if (isinf(number->value.valFloat32)) {
	    CFStringAppend(mstr, (0.0f < number->value.valFloat32) ? CFSTR("+infinity") : CFSTR("-infinity"));
	} else if (0.0f == number->value.valFloat32) {
	    CFStringAppend(mstr, (copysign(1.0, number->value.valFloat32) < 0.0) ? CFSTR("-0.0") : CFSTR("+0.0"));
	} else {
	    CFStringAppendFormat(mstr, NULL, CFSTR("%+.10f"), number->value.valFloat32);
	}
	CFStringAppend(mstr, CFSTR(", type = kCFNumberFloat32Type}"));
	break;
    case kCFNumberFloat64Type:
	// debugging formatting is intentionally more verbose and explicit about the value of the number
	if (isnan(number->value.valFloat64)) {
	    CFStringAppend(mstr, CFSTR("nan"));
	} else if (isinf(number->value.valFloat64)) {
	    CFStringAppend(mstr, (0.0 < number->value.valFloat64) ? CFSTR("+infinity") : CFSTR("-infinity"));
	} else if (0.0 == number->value.valFloat64) {
	    CFStringAppend(mstr, (copysign(1.0, number->value.valFloat64) < 0.0) ? CFSTR("-0.0") : CFSTR("+0.0"));
	} else {
	    CFStringAppendFormat(mstr, NULL, CFSTR("%+.20f"), number->value.valFloat64);
	}
	CFStringAppend(mstr, CFSTR(", type = kCFNumberFloat64Type}"));
	break;
    default:
	__CFInvalidNumberStorageType(__CFNumberGetType(number));
	CFRelease(mstr);
	return NULL;
    }
    return mstr;
}

// This function separated out from __CFNumberCopyFormattingDescription() so the plist creation can use it as well.

__private_extern__ CFStringRef __CFNumberCopyFormattingDescriptionAsFloat64(CFTypeRef cf) {
    double d;
    CFNumberGetValue(cf, kCFNumberFloat64Type, &d);
    if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
        if (isnan(d)) {
            return CFRetain(CFSTR("nan"));
        }
        if (isinf(d)) {
            return CFRetain((0.0 < d) ? CFSTR("+infinity") : CFSTR("-infinity"));
        }
        if (0.0 == d) {
            return CFRetain(CFSTR("0.0"));
        }
        // if %g is used here, need to use DBL_DIG + 2 on Mac OS X, but %f needs +1
        return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%.*g"), DBL_DIG + 2, d);
    }
    return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%lf"), d);
}

static CFStringRef __CFNumberCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    CFNumberRef number = (CFNumberRef)cf;
    CFMutableStringRef mstr;
    int64_t value;
    switch (__CFNumberGetType(number)) {
    case kCFNumberSInt32Type:
    case kCFNumberSInt64Type: 
	value = (__CFNumberGetType(number) == kCFNumberSInt32Type) ? number->value.valSInt32 : number->value.valSInt64;
	mstr = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
	__CFNumberEmitInt64(mstr, value, 0, ' ', false);
	return mstr;
    case kCFNumberFloat32Type:
	if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    if (isnan(number->value.valFloat32)) {
		return CFRetain(CFSTR("nan"));
	    }
	    if (isinf(number->value.valFloat32)) {
		return CFRetain((0.0f < number->value.valFloat32) ? CFSTR("+infinity") : CFSTR("-infinity"));
	    }
	    if (0.0f == number->value.valFloat32) {
		return CFRetain(CFSTR("0.0"));
	    }
	    // if %g is used here, need to use FLT_DIG + 2 on Mac OS X, but %f needs +1
	    return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%.*g"), FLT_DIG + 2, number->value.valFloat32);
	}
	return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%f"), number->value.valFloat32);
	break;
    case kCFNumberFloat64Type:
        return __CFNumberCopyFormattingDescriptionAsFloat64(number);
        break;
    default:
	__CFInvalidNumberStorageType(__CFNumberGetType(number));
	return NULL;
    }
}

static CFTypeID __kCFNumberTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFNumberClass = {
    0,
    "CFNumber",
    NULL,      // init
    NULL,      // copy
    NULL,
    __CFNumberEqual,
    __CFNumberHash,
    __CFNumberCopyFormattingDescription,
    __CFNumberCopyDescription
};

__private_extern__ void __CFNumberInitialize(void) {
    uint64_t dnan = BITSFORDOUBLENAN;
    uint64_t negInf = BITSFORDOUBLENEGINF;
    uint64_t posInf = BITSFORDOUBLEPOSINF;

    __kCFNumberTypeID = _CFRuntimeRegisterClass(&__CFNumberClass);

    _CFRuntimeSetInstanceTypeID(&__kCFNumberNaN, __kCFNumberTypeID);
    __kCFNumberNaN._base._isa = __CFISAForTypeID(__kCFNumberTypeID);
    __CFBitfieldSetValue(__kCFNumberNaN._base._info, 6, 0, __kCFNumberIsNaN);
    __kCFNumberNaN.value.valFloat64 = *(double *)&dnan;

    _CFRuntimeSetInstanceTypeID(& __kCFNumberNegativeInfinity, __kCFNumberTypeID);
    __kCFNumberNegativeInfinity._base._isa = __CFISAForTypeID(__kCFNumberTypeID);
    __CFBitfieldSetValue(__kCFNumberNegativeInfinity._base._info, 6, 0, __kCFNumberIsNegativeInfinity);
    __kCFNumberNegativeInfinity.value.valFloat64 = *(double *)&negInf;

    _CFRuntimeSetInstanceTypeID(& __kCFNumberPositiveInfinity, __kCFNumberTypeID);
    __kCFNumberPositiveInfinity._base._isa = __CFISAForTypeID(__kCFNumberTypeID);
    __CFBitfieldSetValue(__kCFNumberPositiveInfinity._base._info, 6, 0, __kCFNumberIsPositiveInfinity);
    __kCFNumberPositiveInfinity.value.valFloat64 = *(double *)&posInf;
}

CFTypeID CFNumberGetTypeID(void) {
    return __kCFNumberTypeID;
}

CFNumberRef CFNumberCreate(CFAllocatorRef allocator, CFNumberType type, const void *valuePtr) {
    CFNumberRef num;
    CFNumberType equivType, storageType;

    if ((type == kCFNumberFloat32Type) || (type == kCFNumberFloatType)) {
	Float32 val = *(Float32 *)(valuePtr);
	if (isnan(val)) return CFRetain(kCFNumberNaN);
	if (isinf(val)) return CFRetain((val < 0.0) ? kCFNumberNegativeInfinity : kCFNumberPositiveInfinity);
    } else if ((type == kCFNumberFloat64Type) || (type == kCFNumberDoubleType)) {
	Float64 val = *(Float64 *)(valuePtr);
	if (isnan(val)) return CFRetain(kCFNumberNaN);
	if (isinf(val)) return CFRetain((val < 0.0) ? kCFNumberNegativeInfinity : kCFNumberPositiveInfinity);
    }

    equivType = __CFNumberGetCanonicalTypeForType(type);
    storageType = __CFNumberGetStorageTypeForType(type);

    num = (CFNumberRef)_CFRuntimeCreateInstance(allocator, __kCFNumberTypeID, __CFNumberSizeOfType(storageType), NULL);
    if (NULL == num) {
	return NULL;
    }
    SET_VALUE((__CFNumberValue *)&(num->value), equivType, valuePtr);
    __CFBitfieldSetValue(((struct __CFNumber *)num)->_base._info, 6, 0, storageType);
    return num;
}

CFNumberType CFNumberGetType(CFNumberRef number) {
    CF_OBJC_FUNCDISPATCH0(__kCFNumberTypeID, CFNumberType, number, "_cfNumberType");

    __CFAssertIsNumber(number);
    return __CFNumberGetType(number);
}

CFIndex CFNumberGetByteSize(CFNumberRef number) {
    __CFAssertIsNumber(number);
    return __CFNumberSizeOfType(CFNumberGetType(number));
}

Boolean CFNumberIsFloatType(CFNumberRef number) {
    __CFAssertIsNumber(number);
    return __CFNumberTypeIsFloat(CFNumberGetType(number));
}

Boolean	CFNumberGetValue(CFNumberRef number, CFNumberType type, void *valuePtr) {
    uint8_t localMemory[sizeof(__CFNumberValue)];
    __CFNumberValue localValue;
    CFNumberType numType;
    CFNumberType storageTypeForType;

    CF_OBJC_FUNCDISPATCH2(__kCFNumberTypeID, Boolean, number, "_getValue:forType:", valuePtr, __CFNumberGetCanonicalTypeForType(type));
    
    __CFAssertIsNumber(number);
    __CFAssertIsValidNumberType(type);

    storageTypeForType = __CFNumberGetStorageTypeForType(type);
    type = __CFNumberGetCanonicalTypeForType(type);
    if (!valuePtr) valuePtr = &localMemory;

    numType = __CFNumberGetType(number);
    __CFNumberGetValue((__CFNumberValue *)&(number->value), numType, type, valuePtr);

    // If the types match, then we're fine!
    if (numType == storageTypeForType) return true;

    // Test to see if the returned value is intact...
    SET_VALUE(&localValue, type, valuePtr);
    return __CFNumberEqualValue(&localValue, storageTypeForType, &(number->value), numType);
}

CFComparisonResult CFNumberCompare(CFNumberRef number1, CFNumberRef number2, void *context) {
    CFNumberType type1, type2;

    CF_OBJC_FUNCDISPATCH1(__kCFNumberTypeID, CFComparisonResult, number1, "compare:", number2);
    CF_OBJC_FUNCDISPATCH1(__kCFNumberTypeID, CFComparisonResult, number2, "_reverseCompare:", number1);

    __CFAssertIsNumber(number1);
    __CFAssertIsNumber(number2);

    type1 = __CFNumberGetType(number1);
    type2 = __CFNumberGetType(number2);

    if (__CFNumberTypeIsFloat(type1) || __CFNumberTypeIsFloat(type2)) {
	Float64 d1, d2;
	double s1, s2;
        __CFNumberGetValue(&(number1->value), type1, kCFNumberFloat64Type, &d1);
        __CFNumberGetValue(&(number2->value), type2, kCFNumberFloat64Type, &d2);
	s1 = copysign(1.0, d1);
	s2 = copysign(1.0, d2);
	if (!_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    return (d1 > d2) ? kCFCompareGreaterThan : ((d1 < d2) ? kCFCompareLessThan : kCFCompareEqualTo);
	}
	if (isnan(d1) && isnan(d2)) return kCFCompareEqualTo;
	if (isnan(d1)) return (s2 < 0.0) ? kCFCompareGreaterThan : kCFCompareLessThan;
	if (isnan(d2)) return (s1 < 0.0) ? kCFCompareLessThan : kCFCompareGreaterThan;
	// at this point, we know we don't have any NaNs
	if (s1 < s2) return kCFCompareLessThan;
	if (s2 < s1) return kCFCompareGreaterThan;
	// at this point, we know the signs are the same; do not combine these tests
	if (d1 < d2) return kCFCompareLessThan;
	if (d2 < d1) return kCFCompareGreaterThan;
        return kCFCompareEqualTo;
    } else {
        int64_t i1, i2;
        __CFNumberGetValue(&(number1->value), type1, kCFNumberSInt64Type, &i1);
        __CFNumberGetValue(&(number2->value), type2, kCFNumberSInt64Type, &i2);
        return (i1 > i2) ? kCFCompareGreaterThan : ((i1 < i2) ? kCFCompareLessThan : kCFCompareEqualTo);
    }
}

