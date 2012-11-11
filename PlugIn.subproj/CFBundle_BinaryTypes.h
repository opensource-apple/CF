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
/*	CFBundle_BinaryTypes.h
	Copyright (c) 1999-2003, Apple, Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_CFBUNDLE_BINARYTYPES__)
#define __COREFOUNDATION_CFBUNDLE_BINARYTYPES__ 1

#if defined(__cplusplus)
extern "C" {
#endif

/* We support CFM on OS8 and on PPC OSX */

/* We support DYLD on OSX only */
#if defined(__MACH__)
#define BINARY_SUPPORT_DYLD 1
#endif

/* We support DLL on Windows only */
#if defined(__WIN32__)
#define BINARY_SUPPORT_DLL 1
#endif

typedef enum {
    __CFBundleUnknownBinary,
    __CFBundleCFMBinary,
    __CFBundleDYLDExecutableBinary,
    __CFBundleDYLDBundleBinary,
    __CFBundleDYLDFrameworkBinary,
    __CFBundleDLLBinary,
    __CFBundleUnreadableBinary,
    __CFBundleNoBinary
} __CFPBinaryType;

#if defined(__cplusplus)
}
#endif

#endif /* ! __COREFOUNDATION_CFBUNDLE_BINARYTYPES__ */

