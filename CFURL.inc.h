/*
 * Copyright (c) 2013 Apple Inc. All rights reserved.
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

/*	CFURL.inc.h
	Copyright (c) 2012-2013, Apple Inc. All rights reserved.
	Responsibility: Jim Luther
*/


/*
 
 What's this file for?
 
 CFURL's URL string parser needs to be able to parse either an array of char or an array of UniChar.
 
 The code in CFURL.c used to use this macro "#define STRING_CHAR(x) (useCString ? cstring[(x)] : ustring[(x)])" to determine which array to get a character from for every character looked at in the URL string. That macro added one or more compare and branch instructins to the parser's execution for *every* character in the URL string. Those extra compares and branches added up to 10% of the time (for long URL strings) it takes to create a URL object.
 
 To ensure the exact same parser code is run over a char or a UniChar string, the source code was move to this .h file and is included multiple times by CFURL.c as needed. "STRING_CHAR(x)" was replaced by "characterArray[x]", and characterArray is defined as either an "const char *" or a "const UniChar *" for the two sets of function headers that are either parsing an array of char or an array of UniChar.
 
 Any changes made to the parser are made in this file so that both char and the UniChar strings are parsed exactly the same way.
 
 */

/*
    static void _parseComponentsCString(CFAllocatorRef alloc, CFURLRef baseURL, UInt32 *theFlags, CFRange **range, CFIndex cfStringLength, const char *characterArray)
    static void _parseComponentsUString(CFAllocatorRef alloc, CFURLRef baseURL, UInt32 *theFlags, CFRange **range, CFIndex cfStringLength, const UniChar *characterArray)
 */
#ifdef CFURL_INCLUDE_PARSE_COMPONENTS // defined when we want this block of code included
{
    CFRange ranges[9];
    /* index gives the URL part involved; to calculate the correct range index, use the number of the bit of the equivalent flag (i.e. the host flag is HAS_HOST, which is 0x8.  so the range index for the host is 3.)  Note that this is true in this function ONLY, since the ranges stored in (*range) are actually packed, skipping those URL components that don't exist.  This is why the indices are hard-coded in this function. */
    
    CFIndex idx, base_idx = 0;
    CFIndex string_length;
    UInt32 flags = *theFlags;
    Boolean isCompliant;
    uint8_t numRanges = 0;
    
    string_length = cfStringLength;
    
    // Algorithm is as described in RFC 1808
    // 1: parse the fragment; remainder after left-most "#" is fragment
    for (idx = base_idx; idx < string_length; idx++) {
        if ('#' == characterArray[idx]) {
            flags |= HAS_FRAGMENT;
            ranges[8].location = idx + 1;
            ranges[8].length = string_length - (idx + 1);
            numRanges ++;
            string_length = idx;	// remove fragment from parse string
            break;
        }
    }
    // 2: parse the scheme
    for (idx = base_idx; idx < string_length; idx++) {
        UniChar ch = characterArray[idx];
        if (':' == ch) {
            flags |= HAS_SCHEME;
            ranges[0].location = base_idx;
            ranges[0].length = idx;
            numRanges ++;
            base_idx = idx + 1;
            // optimization for ftp urls
            if (idx == 3 && characterArray[0] == 'f' && characterArray[1] == 't' && characterArray[2] == 'p') {
                _setSchemeTypeInFlags(&flags, kHasFtpScheme);
            }
            else if (idx == 4) {
                // optimization for http urls
                if (characterArray[0] == 'h' && characterArray[1] == 't' && characterArray[2] == 't' && characterArray[3] == 'p') {
                    _setSchemeTypeInFlags(&flags, kHasHttpScheme);
                }
                // optimization for file urls
                if (characterArray[0] == 'f' && characterArray[1] == 'i' && characterArray[2] == 'l' && characterArray[3] == 'e') {
                    _setSchemeTypeInFlags(&flags, kHasFileScheme);
                }
                // optimization for data urls
                if (characterArray[0] == 'd' && characterArray[1] == 'a' && characterArray[2] == 't' && characterArray[3] == 'a') {
                    _setSchemeTypeInFlags(&flags, kHasDataScheme);
                }
            }
            // optimization for https urls
            else if (idx == 5 && characterArray[0] == 'h' && characterArray[1] == 't' && characterArray[2] == 't' && characterArray[3] == 'p' && characterArray[3] == 's') {
                _setSchemeTypeInFlags(&flags, kHasHttpsScheme);
            }
            break;
        } else if (!scheme_valid(ch)) {
            break;	// invalid scheme character -- no scheme
        }
    }
    
    // Make sure we have an RFC-1808 compliant URL - that's either something without a scheme, or scheme:/(stuff) or scheme://(stuff)
    // Strictly speaking, RFC 1808 & 2396 bar "scheme:" (with nothing following the colon); however, common usage
    // expects this to be treated identically to "scheme://" - REW, 12/08/03
    if (!(flags & HAS_SCHEME)) {
        isCompliant = true;
    } else if (base_idx == string_length) {
        isCompliant = false;
    } else if (characterArray[base_idx] != '/') {
        isCompliant = false;
    } else {
        isCompliant = true;
    }
    
    if (!isCompliant) {
        // Clear the fragment flag if it's been set
        if (flags & HAS_FRAGMENT) {
            flags &= (~HAS_FRAGMENT);
            string_length = cfStringLength;
        }
        (*theFlags) = flags;
        (*range) = (CFRange *)CFAllocatorAllocate(alloc, sizeof(CFRange), 0);
        (*range)->location = ranges[0].location;
        (*range)->length = ranges[0].length;
        
        return;
    }
    // URL is 1808-compliant
    flags |= IS_DECOMPOSABLE;
    
    // 3: parse the network location and login
    if (2 <= (string_length - base_idx) && '/' == characterArray[base_idx] && '/' == characterArray[base_idx+1]) {
        CFIndex base = 2 + base_idx, extent;
        for (idx = base; idx < string_length; idx++) {
            if ('/' == characterArray[idx] || '?' == characterArray[idx]) {
                break;
            }
        }
        extent = idx;
        
        // net_loc parts extend from base to extent (but not including), which might be to end of string
        // net location is "<user>:<password>@<host>:<port>"
        if (extent != base) {
            for (idx = base; idx < extent; idx++) {
                if ('@' == characterArray[idx]) {   // there is a user
                    CFIndex idx2;
                    flags |= HAS_USER;
                    numRanges ++;
                    ranges[1].location = base;  // base of the user
                    for (idx2 = base; idx2 < idx; idx2++) {
                        if (':' == characterArray[idx2]) {	// found a password separator
                            flags |= HAS_PASSWORD;
                            numRanges ++;
                            ranges[2].location = idx2+1; // base of the password
                            ranges[2].length = idx-(idx2+1);  // password extent
                            ranges[1].length = idx2 - base; // user extent
                            break;
                        }
                    }
                    if (!(flags & HAS_PASSWORD)) {
                        // user extends to the '@'
                        ranges[1].length = idx - base; // user extent
                    }
                    base = idx + 1;
                    break;
                }
            }
            flags |= HAS_HOST;
            numRanges ++;
            ranges[3].location = base; // base of host
            
            // base has been advanced past the user and password if they existed
            for (idx = base; idx < extent; idx++) {
                // IPV6 support (RFC 2732) DCJ June/10/2002
                if ('[' == characterArray[idx]) {	// starting IPV6 explicit address
                    //	Find the ']' terminator of the IPv6 address, leave idx pointing to ']' or end
                    for ( ; idx < extent; ++ idx ) {
                        if ( ']' == characterArray[idx]) {
                            flags |= IS_IPV6_ENCODED;
                            break;
                        }
                    }
                }
                // there is a port if we see a colon.  Only the last one is the port, though.
                else if ( ':' == characterArray[idx]) {
                    flags |= HAS_PORT;
                    numRanges ++;
                    ranges[4].location = idx+1; // base of port
                    ranges[4].length = extent - (idx+1); // port extent
                    ranges[3].length = idx - base; // host extent
                    break;
                }
            }
            if (!(flags & HAS_PORT)) {
                ranges[3].length = extent - base;  // host extent
            }
        }
        base_idx = extent;
    }
    
    // 4: parse the query; remainder after left-most "?" is query
    for (idx = base_idx; idx < string_length; idx++) {
        if ('?' == characterArray[idx]) {
            flags |= HAS_QUERY;
            numRanges ++;
            ranges[7].location = idx + 1;
            ranges[7].length = string_length - (idx+1);
            string_length = idx;	// remove query from parse string
            break;
        }
    }
    
    // 5: parse the parameters; remainder after left-most ";" is parameters
    for (idx = base_idx; idx < string_length; idx++) {
        if (';' == characterArray[idx]) {
            flags |= HAS_PARAMETERS;
            numRanges ++;
            ranges[6].location = idx + 1;
            ranges[6].length = string_length - (idx+1);
            string_length = idx;	// remove parameters from parse string
            break;
        }
    }
    
    // 6: parse the path; it's whatever's left between string_length & base_idx
    if (string_length - base_idx != 0 || (flags & NET_LOCATION_MASK))
    {
        // If we have a net location, we are 1808-compliant, and an empty path substring implies a path of "/"
        UniChar ch;
        Boolean isDir;
        CFRange pathRg;
        flags |= HAS_PATH;
        numRanges ++;
        pathRg.location = base_idx;
        pathRg.length = string_length - base_idx;
        ranges[5] = pathRg;
        
        if (pathRg.length > 0) {
            Boolean sawPercent = FALSE;
            for (idx = pathRg.location; idx < string_length; idx++) {
                if ('%' == characterArray[idx]) {
                    sawPercent = TRUE;
                    break;
                }
            }
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
	    if (pathRg.length > 6 && characterArray[pathRg.location] == '/' && characterArray[pathRg.location + 1] == '.' && characterArray[pathRg.location + 2] == 'f' && characterArray[pathRg.location + 3] == 'i' && characterArray[pathRg.location + 4] == 'l' && characterArray[pathRg.location + 5] == 'e' && characterArray[pathRg.location + 6] == '/') {
		flags |= PATH_HAS_FILE_ID;
	    } else if (!sawPercent) {
                flags |= POSIX_AND_URL_PATHS_MATCH;
            }
#elif DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_WINDOWS
            if (!sawPercent) {
                flags |= POSIX_AND_URL_PATHS_MATCH;
            }
#endif
            
            ch = characterArray[pathRg.location + pathRg.length - 1];
            if (ch == '/') {
                isDir = true;
            } else if (ch == '.') {
                if (pathRg.length == 1) {
                    isDir = true;
                } else {
                    ch = characterArray[pathRg.location + pathRg.length - 2];
                    if (ch == '/') {
                        isDir = true;
                    } else if (ch != '.') {
                        isDir = false;
                    } else if (pathRg.length == 2) {
                        isDir = true;
                    } else {
                        isDir = (characterArray[pathRg.location + pathRg.length - 3] == '/');
                    }
                }
            } else {
                isDir = false;
            }
        } else {
            isDir = (baseURL != NULL) ? CFURLHasDirectoryPath(baseURL) : false;
        }
        if (isDir) {
            flags |= IS_DIRECTORY;
        }
    }
    
    (*theFlags) = flags;
    (*range) = (CFRange *)CFAllocatorAllocate(alloc, sizeof(CFRange)*numRanges, 0);
    numRanges = 0;
    for (idx = 0, flags = 1; flags != (1<<9); flags = (flags<<1), idx ++) {
        if ((*theFlags) & flags) {
            (*range)[numRanges] = ranges[idx];
            numRanges ++;
        }
    }
}
#endif  // CFURL_INCLUDE_PARSE_COMPONENTS

/*
    static Boolean scanCharactersCString(CFAllocatorRef alloc, CFMutableStringRef *escapedString, UInt32 *flags, const char *characterArray, Boolean useCString, CFIndex base, CFIndex end, CFIndex *mark, UInt32 componentFlag, CFStringEncoding encoding)
    static Boolean scanCharactersUString(CFAllocatorRef alloc, CFMutableStringRef *escapedString, UInt32 *flags, const UniChar *characterArray, Boolean useCString, CFIndex base, CFIndex end, CFIndex *mark, UInt32 componentFlag, CFStringEncoding encoding)
 */
#ifdef CFURL_INCLUDE_SCAN_CHARACTERS  // defined when we want this block of code included
{
    CFIndex idx;
    Boolean sawIllegalChar = false;
    for (idx = base; idx < end; idx ++) {
        Boolean shouldEscape;
        UniChar ch = characterArray[idx];
        if (isURLLegalCharacter(ch)) {
            if ((componentFlag == HAS_USER || componentFlag == HAS_PASSWORD) && (ch == '/' || ch == '?' || ch == '@')) {
                shouldEscape = true;
            } else {
                shouldEscape = false;
            }
        } else if (ch == '%' && idx + 2 < end && isHexDigit(characterArray[idx + 1]) && isHexDigit(characterArray[idx+2])) {
            shouldEscape = false;
        } else if (componentFlag == HAS_HOST && ((idx == base && ch == '[') || (idx == end-1 && ch == ']'))) {
            shouldEscape = false;
        } else {
            shouldEscape = true;
        }
        if (shouldEscape) {
            sawIllegalChar = true;
            if (componentFlag && flags) {
                *flags |= componentFlag;
            }
            if (!*escapedString) {
                *escapedString = CFStringCreateMutable(alloc, 0);
            }
            if (useCString) {
                CFStringRef tempString = CFStringCreateWithBytes(alloc, (uint8_t *)&(characterArray[*mark]), idx - *mark, kCFStringEncodingISOLatin1, false);
                CFStringAppend(*escapedString, tempString);
                CFRelease(tempString);
            } else {
                CFStringAppendCharacters(*escapedString, (const UniChar *)&(characterArray[*mark]), idx - *mark);
            }
            *mark = idx + 1;
            _appendPercentEscapesForCharacter(ch, encoding, *escapedString); // This can never fail because anURL->_string was constructed from the encoding passed in
        }
    }
    return sawIllegalChar;
}
#endif  // CFURL_INCLUDE_SCAN_CHARACTERS
