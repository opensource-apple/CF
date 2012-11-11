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
/*	CFCharacterSetPriv.h
	Copyright (c) 1998-2003, Apple, Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_CFCHARACTERSET_PRIV__)
#define __COREFOUNDATION_CFCHARACTERSET_PRIV__ 1

#include <CoreFoundation/CFCharacterSet.h>

#if defined(__cplusplus)
extern "C" {
#endif

#if MAC_OS_X_VERSION_10_2 <= MAC_OS_X_VERSION_MAX_ALLOWED
/*!
	@function CFCharacterSetIsSurrogateHighCharacter
	Reports whether or not the character is a high surrogate.
	@param character  The character to be checked.
	@result true, if character is a high surrogate, otherwise false.
*/
CF_INLINE Boolean CFCharacterSetIsSurrogateHighCharacter(UniChar character) {
    return ((character >= 0xD800UL) && (character <= 0xDBFFUL) ? true : false);
}

/*!
	@function CFCharacterSetIsSurrogateLowCharacter
	Reports whether or not the character is a low surrogate.
	@param character  The character to be checked.
	@result true, if character is a low surrogate, otherwise false.
*/
CF_INLINE Boolean CFCharacterSetIsSurrogateLowCharacter(UniChar character) {
    return ((character >= 0xDC00UL) && (character <= 0xDFFFUL) ? true : false);
}

/*!
	@function CFCharacterSetGetLongCharacterForSurrogatePair
	Returns the UTF-32 value corresponding to the surrogate pair passed in.
	@param surrogateHigh  The high surrogate character.  If this parameter
			is not a valid high surrogate character, the behavior is undefined.
	@param surrogateLow  The low surrogate character.  If this parameter
			is not a valid low surrogate character, the behavior is undefined.
	@result The UTF-32 value for the surrogate pair.
*/
CF_INLINE UTF32Char CFCharacterSetGetLongCharacterForSurrogatePair(UniChar surrogateHigh, UniChar surrogateLow) {
    return ((surrogateHigh - 0xD800UL) << 10) + (surrogateLow - 0xDC00UL) + 0x0010000UL;
}
#endif

/* Check to see if the character represented by the surrogate pair surrogateHigh & surrogateLow is in the chraracter set */
CF_EXPORT Boolean CFCharacterSetIsSurrogatePairMember(CFCharacterSetRef theSet, UniChar surrogateHigh, UniChar surrogateLow) ;

/* Keyed-coding support
*/
typedef enum {
    kCFCharacterSetKeyedCodingTypeBitmap = 1,
    kCFCharacterSetKeyedCodingTypeBuiltin = 2,
    kCFCharacterSetKeyedCodingTypeRange = 3,
    kCFCharacterSetKeyedCodingTypeString = 4
} CFCharacterSetKeyedCodingType;

CF_EXPORT CFCharacterSetKeyedCodingType _CFCharacterSetGetKeyedCodingType(CFCharacterSetRef cset);
CF_EXPORT CFCharacterSetPredefinedSet _CFCharacterSetGetKeyedCodingBuiltinType(CFCharacterSetRef cset);
CF_EXPORT CFRange _CFCharacterSetGetKeyedCodingRange(CFCharacterSetRef cset);
CF_EXPORT CFStringRef _CFCharacterSetCreateKeyedCodingString(CFCharacterSetRef cset);
CF_EXPORT bool _CFCharacterSetIsMutable(CFCharacterSetRef cset);
CF_EXPORT bool _CFCharacterSetIsInverted(CFCharacterSetRef cset);
CF_EXPORT void _CFCharacterSetSetIsInverted(CFCharacterSetRef cset, bool flag);

#if defined(__cplusplus)
}
#endif

#endif /* !__COREFOUNDATION_CFCHARACTERSET_PRIV__ */
