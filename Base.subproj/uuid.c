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
/*	uuid.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Doug Davidson
*/

#if defined(__WIN32__)
/* _CFGenerateUUID function just calls the COM library's UUID generator
 * (Aleksey Dukhnyakov)
 */
#include <windows.h>
#include <ole2.h>
#include <objbase.h>

unsigned long _CFGenerateUUID(uuid_t *uuid) {
    RPC_STATUS rStatus;

    /* call GetScode() function to get RPC_STATUS, because
     * CoCreateGuid(uuid) function return HRESULT type
     */
    rStatus = GetScode(CoCreateGuid(uuid));

    /* We accept only following results RPC_S_OK, RPC_S_UUID_LOCAL_ONLY
     */
    if ( rStatus == RPC_S_UUID_NO_ADDRESS)
        return rStatus;

    return 0;
};

#else

/*    uuid.c
 *
 *        Modifications made by William Woody to make this thing
 *    work on the Macintosh.
 */

/*
 * 
 * (c) Copyright 1989 OPEN SOFTWARE FOUNDATION, INC.
 * (c) Copyright 1989 HEWLETT-PACKARD COMPANY
 * (c) Copyright 1989 DIGITAL EQUIPMENT CORPORATION
 * To anyone who acknowledges that this file is provided "AS IS"
 * without any express or implied warranty:
 *                 permission to use, copy, modify, and distribute this
 * file for any purpose is hereby granted without fee, provided that
 * the above copyright notices and this notice appears in all source
 * code copies, and that none of the names of Open Software
 * Foundation, Inc., Hewlett-Packard Company, or Digital Equipment
 * Corporation be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission.  Neither Open Software Foundation, Inc., Hewlett-
 * Packard Company, nor Digital Equipment Corporation makes any
 * representations about the suitability of this software for any
 * purpose.
 * 
 */
/*
 */
/*
**
**  NAME:
**
**      uuid.c
**
**  FACILITY:
**
**      UUID
**
**  ABSTRACT:
**
**      UUID - routines that manipulate uuid's
**
**
*/

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFDate.h>
#include "CFInternal.h"
#include <string.h>


/* uuid_t already defined in RPCDCE.H (WIN32 header) 
 * (Aleksey Dukhnyakov) 
 */ 
#if defined(__WIN32__)  
#define UUID_T_DEFINED 1
#endif

#if !defined(UUID_T_DEFINED)
#define UUID_T_DEFINED 1

/*    uuid
 *
 *        Universal Unique ID. Note this definition will result is a 16-byte
 *    structure regardless what platform it is on.
 */

struct uuid_t {
    unsigned long        time_low;
    unsigned short        time_mid;
    unsigned short        time_hi_and_version;
    unsigned char        clock_seq_hi_and_reserved;
    unsigned char        clock_seq_low;
    unsigned char        node[6];
};

typedef struct uuid_t uuid_t;

#endif

enum {
    kUUIDInternalError = -21001,
    kUUIDInvalidString = -21002
};

extern unsigned long _CFGenerateUUID(uuid_t *uuid);

typedef struct
{
    unsigned char eaddr[6];      /* 6 bytes of ethernet hardware address */
} uuid_address_t;

typedef struct {
    unsigned long lo;
    unsigned long hi;
} uuid_time_t;

static OSErr GenRandomEthernet(uuid_address_t *addr);
static OSErr GetEthernetAddr(uuid_address_t *addr);

static OSErr ReadPrefData(void);

/*
 *    Preferences file management
 */

static uuid_address_t GSavedENetAddr = {{0, 0, 0, 0, 0, 0}};
static uuid_time_t GLastTime = {0, 0};            /* Clock state info */
static unsigned short GTimeAdjust = 0;
static unsigned short GClockSeq = 0;


/*
 * Internal structure of universal unique IDs (UUIDs).
 *
 * There are three "variants" of UUIDs that this code knows about.  The
 * variant #0 is what was defined in the 1989 HP/Apollo Network Computing
 * Architecture (NCA) specification and implemented in NCS 1.x and DECrpc
 * v1.  Variant #1 is what was defined for the joint HP/DEC specification
 * for the OSF (in DEC's "UID Architecture Functional Specification Version
 * X1.0.4") and implemented in NCS 2.0, DECrpc v2, and OSF 1.0 DCE RPC.
 * Variant #2 is defined by Microsoft.
 *
 * This code creates only variant #1 UUIDs.
 * 
 * The three UUID variants can exist on the same wire because they have
 * distinct values in the 3 MSB bits of octet 8 (see table below).  Do
 * NOT confuse the version number with these 3 bits.  (Note the distinct
 * use of the terms "version" and "variant".) Variant #0 had no version
 * field in it.  Changes to variant #1 (should any ever need to be made)
 * can be accomodated using the current form's 4 bit version field.
 * 
 * The UUID record structure MUST NOT contain padding between fields.
 * The total size = 128 bits.
 *
 * To minimize confusion about bit assignment within octets, the UUID
 * record definition is defined only in terms of fields that are integral
 * numbers of octets.
 *
 * Depending on the network data representation, the multi-octet unsigned
 * integer fields are subject to byte swapping when communicated between
 * dissimilar endian machines.  Note that all three UUID variants have
 * the same record structure; this allows this byte swapping to occur.
 * (The ways in which the contents of the fields are generated can and
 * do vary.)
 *
 * The following information applies to variant #1 UUIDs:
 *
 * The lowest addressed octet contains the global/local bit and the
 * unicast/multicast bit, and is the first octet of the address transmitted
 * on an 802.3 LAN.
 *
 * The adjusted time stamp is split into three fields, and the clockSeq
 * is split into two fields.
 *
 * |<------------------------- 32 bits -------------------------->|
 *
 * +--------------------------------------------------------------+
 * |                     low 32 bits of time                      |  0-3  .time_low
 * +-------------------------------+-------------------------------
 * |     mid 16 bits of time       |  4-5               .time_mid
 * +-------+-----------------------+
 * | vers. |   hi 12 bits of time  |  6-7               .time_hi_and_version
 * +-------+-------+---------------+
 * |Res|  clkSeqHi |  8                                 .clock_seq_hi_and_reserved
 * +---------------+
 * |   clkSeqLow   |  9                                 .clock_seq_low
 * +---------------+----------...-----+
 * |            node ID               |  8-16           .node
 * +--------------------------...-----+
 *
 * --------------------------------------------------------------------------
 *
 * The structure layout of all three UUID variants is fixed for all time.
 * I.e., the layout consists of a 32 bit int, 2 16 bit ints, and 8 8
 * bit ints.  The current form version field does NOT determine/affect
 * the layout.  This enables us to do certain operations safely on the
 * variants of UUIDs without regard to variant; this increases the utility
 * of this code even as the version number changes (i.e., this code does
 * NOT need to check the version field).
 *
 * The "Res" field in the octet #8 is the so-called "reserved" bit-field
 * and determines whether or not the uuid is a old, current or other
 * UUID as follows:
 *
 *      MS-bit  2MS-bit  3MS-bit      Variant
 *      ---------------------------------------------
 *         0       x        x       0 (NCS 1.5)
 *         1       0        x       1 (DCE 1.0 RPC)
 *         1       1        0       2 (Microsoft)
 *         1       1        1       unspecified
 *
 * --------------------------------------------------------------------------
 *
 * Internal structure of variant #0 UUIDs
 *
 * The first 6 octets are the number of 4 usec units of time that have
 * passed since 1/1/80 0000 GMT.  The next 2 octets are reserved for
 * future use.  The next octet is an address family.  The next 7 octets
 * are a host ID in the form allowed by the specified address family.
 *
 * Note that while the family field (octet 8) was originally conceived
 * of as being able to hold values in the range [0..255], only [0..13]
 * were ever used.  Thus, the 2 MSB of this field are always 0 and are
 * used to distinguish old and current UUID forms.
 *
 * +--------------------------------------------------------------+
 * |                    high 32 bits of time                      |  0-3  .time_high
 * +-------------------------------+-------------------------------
 * |     low 16 bits of time       |  4-5               .time_low
 * +-------+-----------------------+
 * |         reserved              |  6-7               .reserved
 * +---------------+---------------+
 * |    family     |   8                                .family
 * +---------------+----------...-----+
 * |            node ID               |  9-16           .node
 * +--------------------------...-----+
 *
 */

/***************************************************************************
 *
 * Local definitions
 *
 **************************************************************************/

static const long      uuid_c_version          = 1;

/*
 * local defines used in uuid bit-diddling
 */
#define HI_WORD(w)                  ((w) >> 16)
#define RAND_MASK                   0x3fff      /* same as CLOCK_SEQ_LAST */

#define TIME_MID_MASK               0x0000ffff
#define TIME_HIGH_MASK              0x0fff0000
#define TIME_HIGH_SHIFT_COUNT       16

/*
 *    The following was modified in order to prevent overlap because
 *    our clock is (theoretically) accurate to 1us (or 1s in CarbonLib)
 */


#define MAX_TIME_ADJUST             9            /* Max adjust before tick */

#define CLOCK_SEQ_LOW_MASK          0xff
#define CLOCK_SEQ_HIGH_MASK         0x3f00
#define CLOCK_SEQ_HIGH_SHIFT_COUNT  8
#define CLOCK_SEQ_FIRST             1
#define CLOCK_SEQ_LAST              0x3fff      /* same as RAND_MASK */

/*
 * Note: If CLOCK_SEQ_BIT_BANG == true, then we can avoid the modulo
 * operation.  This should save us a divide instruction and speed
 * things up.
 */

#ifndef CLOCK_SEQ_BIT_BANG
#define CLOCK_SEQ_BIT_BANG          1
#endif

#if CLOCK_SEQ_BIT_BANG
#define CLOCK_SEQ_BUMP(seq)         ((*seq) = ((*seq) + 1) & CLOCK_SEQ_LAST)
#else
#define CLOCK_SEQ_BUMP(seq)         ((*seq) = ((*seq) + 1) % (CLOCK_SEQ_LAST+1))
#endif

#define UUID_VERSION_BITS           (uuid_c_version << 12)
#define UUID_RESERVED_BITS          0x80

#define IS_OLD_UUID(uuid) (((uuid)->clock_seq_hi_and_reserved & 0xc0) != 0x80)

/****************************************************************************
 *
 * local data declarations
 *
 ****************************************************************************/

typedef struct {
    unsigned long lo;
    unsigned long hi;
} unsigned64_t;

/*
 * declarations used in UTC time calculations
 */
 
static uuid_time_t          time_now = {0, 0};     /* utc time as of last query        */
//static uuid_time_t          time_last;    /* utc time last time I looked      */
//static unsigned short       time_adjust;  /* 'adjustment' to ensure uniqness  */
//static unsigned short       clock_seq;    /* 'adjustment' for backwards clocks*/

/*
 * true_random variables
 */

static unsigned long     rand_m = 0;         /* multiplier                       */
static unsigned long     rand_ia = 0;        /* adder #1                         */
static unsigned long     rand_ib = 0;        /* adder #2                         */
static unsigned long     rand_irand = 0;     /* random value                     */

typedef enum
{
    uuid_e_less_than, uuid_e_equal_to, uuid_e_greater_than
} uuid_compval_t;




/****************************************************************************
 *
 * local function declarations
 *
 ****************************************************************************/

/*
 * I N I T
 *
 * Startup initialization routine for UUID module.
 */

static OSErr init (void);

/*
 * T R U E _ R A N D O M _ I N I T
 */

static void true_random_init (void);

/*
 * T R U E _ R A N D O M
 */
static unsigned short true_random (void);


/*
 * N E W _ C L O C K _ S E Q
 *
 * Ensure clock_seq is up-to-date
 *
 * Note: clock_seq is architected to be 14-bits (unsigned) but
 *       I've put it in here as 16-bits since there isn't a
 *       14-bit unsigned integer type (yet)
 */ 
static void new_clock_seq ( unsigned short * /*clock_seq*/);


/*
 * T I M E _ C M P
 *
 * Compares two UUID times (64-bit DEC UID UTC values)
 */
static uuid_compval_t time_cmp (
        uuid_time_t *        /*time1*/,
        uuid_time_t *        /*time2*/
    );


/************************************************************************/
/*                                                                        */
/*    New Routines                                                        */
/*                                                                        */
/************************************************************************/

/*
 * saved copy of our IEEE 802 address for quick reference
 */

static uuid_address_t saved_addr = {{0, 0, 0, 0, 0, 0}};
static int got_address = false;
static int last_addr_result = false;


/*
**++
**
**  ROUTINE NAME:       uuid_get_address
**
**  SCOPE:              PUBLIC
**
**  DESCRIPTION:
**
**  Return our IEEE 802 address.
**
**  This function is not really "public", but more like the SPI functions
**  -- available but not part of the official API.  We've done this so
**  that other subsystems (of which there are hopefully few or none)
**  that need the IEEE 802 address can use this function rather than
**  duplicating the gore it does (or more specifically, the gore that
**  "uuid__get_os_address" does).
**
**  INPUTS:             none
**
**  INPUTS/OUTPUTS:     none
**
**  OUTPUTS:
**
**      addr            IEEE 802 address
**
**      status          return status value
**
**  IMPLICIT INPUTS:    none
**
**  IMPLICIT OUTPUTS:   none
**
**  FUNCTION VALUE:     none
**
**  SIDE EFFECTS:       none
**
**--
**/

static int uuid_get_address(uuid_address_t *addr)
{
    
    /*
     * just return address we determined previously if we've
     * already got one
     */

    if (got_address) {
        memmove (addr, &saved_addr, sizeof (uuid_address_t));
        return last_addr_result;
    }

    /*
     * Otherwise, call the system specific routine.
     */

    last_addr_result = GetEthernetAddr(addr);
    
    /*
     *    Was this an error? If so, I need to generate a random
     *    sequence to use in place of an Ethernet address.
     */
    if (last_addr_result) {
        last_addr_result = GenRandomEthernet(addr);
    }
    
    got_address = true;
    if (last_addr_result == 0) {
        /* On no error copy */
        memmove (&saved_addr, addr, sizeof (uuid_address_t));
    }
    return last_addr_result;
}

static OSErr GenRandomEthernet(uuid_address_t *addr) {
    unsigned int i;
    for (i = 0; i < 6; i++) {
        addr->eaddr[i] = (unsigned char)(true_random() & 0xff);
    }
    return 0;
}

__private_extern__ CFStringRef __CFCopyRegularEthernetAddrString(void) {
    uuid_address_t addr;
    static CFStringRef string = NULL;
    static Boolean lookedUpAddr = false;
    
    if (!lookedUpAddr) {
        // dont use the cache, since a random enet addr might have been put in it
        if (GetEthernetAddr(&addr) == 0) {
            string = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%02x:%02x:%02x:%02x:%02x:%02x"), addr.eaddr[0], addr.eaddr[1], addr.eaddr[2], addr.eaddr[3], addr.eaddr[4], addr.eaddr[5]);
        }
        lookedUpAddr = true;
    }
    return (string ? CFRetain(string) : NULL);
}

__private_extern__ CFStringRef __CFCopyEthernetAddrString(void) {
    uuid_address_t addr;
    static CFStringRef string = NULL;
    static Boolean lookedUpAddr = false;
    
    if (!lookedUpAddr) {
        // dont use the cache, since a random enet addr might have been put in it
        if (GetEthernetAddr(&addr) == 0) {
            string = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%02x%02x%02x%02x%02x%02x"), addr.eaddr[0], addr.eaddr[1], addr.eaddr[2], addr.eaddr[3], addr.eaddr[4], addr.eaddr[5]);
        }
        lookedUpAddr = true;
    }
    return (string ? CFRetain(string) : NULL);
}

/*****************************************************************************
 *
 *  Macro definitions
 *
 ****************************************************************************/

/*
 * ensure we've been initialized
 */
static int uuid_init_done = false;

#define EmptyArg
#define UUID_VERIFY_INIT(Arg)          \
    if (! uuid_init_done)           \
    {                               \
        init (status);              \
        if (*status != uuid_s_ok)   \
        {                           \
            return Arg;                 \
        }                           \
    }

/*
 * Check the reserved bits to make sure the UUID is of the known structure.
 */

#define CHECK_STRUCTURE(uuid) \
( \
    (((uuid)->clock_seq_hi_and_reserved & 0x80) == 0x00) || /* var #0 */ \
    (((uuid)->clock_seq_hi_and_reserved & 0xc0) == 0x80) || /* var #1 */ \
    (((uuid)->clock_seq_hi_and_reserved & 0xe0) == 0xc0)    /* var #2 */ \
)

/*
 * The following macros invoke CHECK_STRUCTURE(), check that the return
 * value is okay and if not, they set the status variable appropriately
 * and return either a boolean false, nothing (for void procedures),
 * or a value passed to the macro.  This has been done so that checking
 * can be done more simply and values are returned where appropriate
 * to keep compilers happy.
 *
 * bCHECK_STRUCTURE - returns boolean false
 * vCHECK_STRUCTURE - returns nothing (void)
 * rCHECK_STRUCTURE - returns 'r' macro parameter
 */

#define bCHECK_STRUCTURE(uuid, status) \
{ \
    if (!CHECK_STRUCTURE (uuid)) \
    { \
        *(status) = uuid_s_bad_version; \
        return (false); \
    } \
}

#define vCHECK_STRUCTURE(uuid, status) \
{ \
    if (!CHECK_STRUCTURE (uuid)) \
    { \
        *(status) = uuid_s_bad_version; \
        return; \
    } \
}

#define rCHECK_STRUCTURE(uuid, status, result) \
{ \
    if (!CHECK_STRUCTURE (uuid)) \
    { \
        *(status) = uuid_s_bad_version; \
        return (result); \
    } \
}


/*
 *  Define constant designation difference in Unix and DTSS base times:
 *  DTSS UTC base time is October 15, 1582.
 *  Unix base time is January 1, 1970.
 */
#define uuid_c_os_base_time_diff_lo     0x13814000
#define uuid_c_os_base_time_diff_hi     0x01B21DD2

#ifndef UUID_C_100NS_PER_SEC
#define UUID_C_100NS_PER_SEC            10000000
#endif

#ifndef UUID_C_100NS_PER_USEC
#define UUID_C_100NS_PER_USEC           10
#endif





/*
 * UADD_UVLW_2_UVLW - macro to add two unsigned 64-bit long integers
 *                      (ie. add two unsigned 'very' long words)
 *
 * Important note: It is important that this macro accommodate (and it does)
 *                 invocations where one of the addends is also the sum.
 *
 * This macro was snarfed from the DTSS group and was originally:
 *
 * UTCadd - macro to add two UTC times
 *
 * add lo and high order longword separately, using sign bits of the low-order
 * longwords to determine carry.  sign bits are tested before addition in two
 * cases - where sign bits match. when the addend sign bits differ the sign of
 * the result is also tested:
 *
 *        sign            sign
 *      addend 1        addend 2        carry?
 *
 *          1               1            true
 *          1               0            true if sign of sum clear
 *          0               1            true if sign of sum clear
 *          0               0            false
 */
#define UADD_UVLW_2_UVLW(add1, add2, sum)                               \
    if (!(((add1)->lo&0x80000000UL) ^ ((add2)->lo&0x80000000UL)))           \
    {                                                                   \
        if (((add1)->lo&0x80000000UL))                                    \
        {                                                               \
            (sum)->lo = (add1)->lo + (add2)->lo ;                       \
            (sum)->hi = (add1)->hi + (add2)->hi+1 ;                     \
        }                                                               \
        else                                                            \
        {                                                               \
            (sum)->lo  = (add1)->lo + (add2)->lo ;                      \
            (sum)->hi = (add1)->hi + (add2)->hi ;                       \
        }                                                               \
    }                                                                   \
    else                                                                \
    {                                                                   \
        (sum)->lo = (add1)->lo + (add2)->lo ;                           \
        (sum)->hi = (add1)->hi + (add2)->hi ;                           \
        if (!((sum)->lo&0x80000000UL))                                    \
            (sum)->hi++ ;                                               \
    }

/*
 * UADD_UW_2_UVLW - macro to add a 16-bit unsigned integer to
 *                   a 64-bit unsigned integer
 *
 * Note: see the UADD_UVLW_2_UVLW() macro
 *
 */
#define UADD_UW_2_UVLW(add1, add2, sum)                                 \
{                                                                       \
    (sum)->hi = (add2)->hi;                                             \
    if ((add2)->lo & 0x80000000UL)                                        \
    {                                                                   \
        (sum)->lo = (*add1) + (add2)->lo;                               \
        if (!((sum)->lo & 0x80000000UL))                                  \
        {                                                               \
            (sum)->hi++;                                                \
        }                                                               \
    }                                                                   \
    else                                                                \
    {                                                                   \
        (sum)->lo = (*add1) + (add2)->lo;                               \
    }                                                                   \
}

/*
 * U U I D _ _ G E T _ O S _ T I M E
 *
 * Get OS time - contains platform-specific code.
 */

static const double utc_conversion_factor = 429.4967296; // 2^32 / 10^7

static void uuid__get_os_time (uuid_time_t * uuid_time)
{
    unsigned64_t	utc,
                        os_basetime_diff;
    CFAbsoluteTime at = CFAbsoluteTimeGetCurrent() + kCFAbsoluteTimeIntervalSince1970;
    double utc_at = at / utc_conversion_factor;
    
    /* Convert 'at' in double seconds to 100ns units in utc */
    utc.hi = (unsigned long)utc_at;
    utc_at -= (double)utc.hi;
    utc_at *= utc_conversion_factor;
    utc_at *= 10000000.0;
    utc.lo = (unsigned long)utc_at;

    /*
     * Offset between DTSS formatted times and Unix formatted times.
     */
    os_basetime_diff.lo = uuid_c_os_base_time_diff_lo;
    os_basetime_diff.hi = uuid_c_os_base_time_diff_hi;
    UADD_UVLW_2_UVLW (&utc, &os_basetime_diff, uuid_time);

}

/*
**++
**
**  ROUTINE NAME:       init
**
**  SCOPE:              INTERNAL - declared locally
**
**  DESCRIPTION:
**
**  Startup initialization routine for the UUID module.
**
**  INPUTS:             none
**
**  INPUTS/OUTPUTS:     none
**
**  OUTPUTS:
**
**      status          return status value
**
**          uuid_s_ok
**          uuid_s_coding_error
**
**  IMPLICIT INPUTS:    none
**
**  IMPLICIT OUTPUTS:   none
**
**  FUNCTION VALUE:     void
**
**  SIDE EFFECTS:       sets uuid_init_done so this won't be done again
**
**--
**/

static OSErr init()
{
    /*
     * init the random number generator
     */
    
    true_random_init();

    /*
     *    Read the preferences data from the Macintosh pref file
     */
    
    ReadPrefData();

    /*
     *    Get the time. Note that I renamed 'time_last' to
     *    GLastTime to indicate that I'm using it elsewhere as
     *    a shared library global.
     */
        
    if ((GLastTime.hi == 0) && (GLastTime.lo == 0)) {
        uuid__get_os_time (&GLastTime);
        GClockSeq = true_random();
    }
    uuid_init_done = true;
    return 0;
}

/*
** New name: GenerateUID
** 
**++
**
**  ROUTINE NAME:       uuid_create
**
**  SCOPE:              PUBLIC - declared in UUID.IDL
**
**  DESCRIPTION:
**
**  Create a new UUID. Note: we only know how to create the new
**  and improved UUIDs.
**
**  INPUTS:             none
**
**  INPUTS/OUTPUTS:     none
**
**  OUTPUTS:
**
**      uuid            A new UUID value
**
**      status          return status value
**
**          uuid_s_ok
**          uuid_s_coding_error
**
**  IMPLICIT INPUTS:    none
**
**  IMPLICIT OUTPUTS:   none
**
**  FUNCTION VALUE:     void
**
**  SIDE EFFECTS:       none
**
**--
**/

/*
PUBLIC void uuid_create
#ifdef _DCE_PROTO_
(
    uuid_t                  *uuid,
    unsigned long              *status
)
#else
(uuid, status)
uuid_t                  *uuid;
unsigned long              *status;
#endif
*/

__private_extern__ unsigned long _CFGenerateUUID(uuid_t *uuid)
{
    OSErr                    err;
    uuid_address_t            eaddr;
    int               got_no_time = false;

    if (!uuid_init_done) {
        err = init();
        if (err) return err;
    }
    /*
     * get our hardware network address
     */
     
    if (0 != (err = uuid_get_address(&eaddr))) return err;

    do
    {
        /*
         * get the current time
         */
        uuid__get_os_time (&time_now);

        /*
         * do stuff like:
         *
         *  o check that our clock hasn't gone backwards and handle it
         *    accordingly with clock_seq
         *  o check that we're not generating uuid's faster than we
         *    can accommodate with our time_adjust fudge factor
         */
        switch (time_cmp (&time_now, &GLastTime))
        {
            case uuid_e_less_than:
                new_clock_seq (&GClockSeq);
                GTimeAdjust = 0;
                break;
            case uuid_e_greater_than:
                GTimeAdjust = 0;
                break;
            case uuid_e_equal_to:
                if (GTimeAdjust == MAX_TIME_ADJUST)
                {
                    /*
                     * spin your wheels while we wait for the clock to tick
                     */
                    got_no_time = true;
                }
                else
                {
                    GTimeAdjust++;
                }
                break;
            default:
                return kUUIDInternalError;
        }
    } while (got_no_time);

    GLastTime.lo = time_now.lo;
    GLastTime.hi = time_now.hi;

    if (GTimeAdjust != 0)
    {
        UADD_UW_2_UVLW (&GTimeAdjust, &time_now, &time_now);
    }

    /*
     * now construct a uuid with the information we've gathered
     * plus a few constants
     */
    uuid->time_low = time_now.lo;
    uuid->time_mid = time_now.hi & TIME_MID_MASK;

    uuid->time_hi_and_version =
        (time_now.hi & TIME_HIGH_MASK) >> TIME_HIGH_SHIFT_COUNT;
    uuid->time_hi_and_version |= UUID_VERSION_BITS;

    uuid->clock_seq_low = GClockSeq & CLOCK_SEQ_LOW_MASK;
    uuid->clock_seq_hi_and_reserved =
        (GClockSeq & CLOCK_SEQ_HIGH_MASK) >> CLOCK_SEQ_HIGH_SHIFT_COUNT;

    uuid->clock_seq_hi_and_reserved |= UUID_RESERVED_BITS;

    memmove (uuid->node, &eaddr, sizeof (uuid_address_t));

    return 0;
}

/*****************************************************************************
 *
 *  LOCAL MATH PROCEDURES - math procedures used internally by the UUID module
 *
 ****************************************************************************/

/*
** T I M E _ C M P
**
** Compares two UUID times (64-bit UTC values)
**/

static uuid_compval_t time_cmp(uuid_time_t *time1,uuid_time_t *time2)
{
    /*
     * first check the hi parts
     */
    if (time1->hi < time2->hi) return (uuid_e_less_than);
    if (time1->hi > time2->hi) return (uuid_e_greater_than);

    /*
     * hi parts are equal, check the lo parts
     */
    if (time1->lo < time2->lo) return (uuid_e_less_than);
    if (time1->lo > time2->lo) return (uuid_e_greater_than);

    return (uuid_e_equal_to);
}



/****************************************************************************
**
**    U U I D   T R U E   R A N D O M   N U M B E R   G E N E R A T O R
**
*****************************************************************************
**
** This random number generator (RNG) was found in the ALGORITHMS Notesfile.
**
** (Note 16.7, July 7, 1989 by Robert (RDVAX::)Gries, Cambridge Research Lab,
**  Computational Quality Group)
**
** It is really a "Multiple Prime Random Number Generator" (MPRNG) and is
** completely discussed in reference #1 (see below).
**
**   References:
**   1) "The Multiple Prime Random Number Generator" by Alexander Hass
**      pp. 368 to 381 in ACM Transactions on Mathematical Software,
**      December, 1987
**   2) "The Art of Computer Programming: Seminumerical Algorithms
**      (vol 2)" by Donald E. Knuth, pp. 39 to 113.
**
** A summary of the notesfile entry follows:
**
** Gries discusses the two RNG's available for ULTRIX-C.  The default RNG
** uses a Linear Congruential Method (very popular) and the second RNG uses
** a technique known as a linear feedback shift register.
**
** The first (default) RNG suffers from bit-cycles (patterns/repetition),
** ie. it's "not that random."
**
** While the second RNG passes all the emperical tests, there are "states"
** that become "stable", albeit contrived.
**
** Gries then presents the MPRNG and says that it passes all emperical
** tests listed in reference #2.  In addition, the number of calls to the
** MPRNG before a sequence of bit position repeats appears to have a normal
** distribution.
**
** Note (mbs): I have coded the Gries's MPRNG with the same constants that
** he used in his paper.  I have no way of knowing whether they are "ideal"
** for the range of numbers we are dealing with.
**
****************************************************************************/

/*
** T R U E _ R A N D O M _ I N I T
**
** Note: we "seed" the RNG with the bits from the clock and the PID
**
**/

static void true_random_init (void)
{
    uuid_time_t         t;
    unsigned short          *seedp, seed=0;


    /*
     * optimal/recommended starting values according to the reference
     */
    static unsigned long   rand_m_init     = 971;
    static unsigned long   rand_ia_init    = 11113;
    static unsigned long   rand_ib_init    = 104322;
    static unsigned long   rand_irand_init = 4181;

    rand_m = rand_m_init;
    rand_ia = rand_ia_init;
    rand_ib = rand_ib_init;
    rand_irand = rand_irand_init;

    /*
     * Generating our 'seed' value
     *
     * We start with the current time, but, since the resolution of clocks is
     * system hardware dependent (eg. Ultrix is 10 msec.) and most likely
     * coarser than our resolution (10 usec) we 'mixup' the bits by xor'ing
     * all the bits together.  This will have the effect of involving all of
     * the bits in the determination of the seed value while remaining system
     * independent.  Then for good measure to ensure a unique seed when there
     * are multiple processes creating UUID's on a system, we add in the PID.
     */
    uuid__get_os_time(&t);
    seedp = (unsigned short *)(&t);
    seed ^= *seedp++;
    seed ^= *seedp++;
    seed ^= *seedp++;
    seed ^= *seedp++;
    rand_irand += seed;
}

/*
** T R U E _ R A N D O M
**
** Note: we return a value which is 'tuned' to our purposes.  Anyone
** using this routine should modify the return value accordingly.
**/

static unsigned short true_random (void)
{
    rand_m += 7;
    rand_ia += 1907;
    rand_ib += 73939;

    if (rand_m >= 9973) rand_m -= 9871;
    if (rand_ia >= 99991) rand_ia -= 89989;
    if (rand_ib >= 224729) rand_ib -= 96233;

    rand_irand = (rand_irand * rand_m) + rand_ia + rand_ib;

    return (HI_WORD (rand_irand) ^ (rand_irand & RAND_MASK));
}

/*****************************************************************************
 *
 *  LOCAL PROCEDURES - procedures used staticly by the UUID module
 *
 ****************************************************************************/

/*
** N E W _ C L O C K _ S E Q
**
** Ensure *clkseq is up-to-date
**
** Note: clock_seq is architected to be 14-bits (unsigned) but
**       I've put it in here as 16-bits since there isn't a
**       14-bit unsigned integer type (yet)
**/

static void new_clock_seq 
#ifdef _DCE_PROTO_
(
    unsigned short              *clkseq
)
#else
(clkseq)
unsigned short              *clkseq;
#endif
{
    /*
     * A clkseq value of 0 indicates that it hasn't been initialized.
     */
    if (*clkseq == 0)
    {
#ifdef UUID_NONVOLATILE_CLOCK
        *clkseq = uuid__read_clock();           /* read nonvolatile clock */
        if (*clkseq == 0)                       /* still not init'd ???   */
        {
            *clkseq = true_random();      /* yes, set random        */
        }
#else
        /*
         * with a volatile clock, we always init to a random number
         */
        *clkseq = true_random();
#endif
    }

    CLOCK_SEQ_BUMP (clkseq);
    if (*clkseq == 0)
    {
        *clkseq = *clkseq + 1;
    }

#ifdef UUID_NONVOLATILE_CLOCK
    uuid_write_clock (clkseq);
#endif
}



/*    ReadPrefData
 *
 *        Read the preferences data into my global variables
 */

static OSErr ReadPrefData(void)
{
    /*
     *    Zero out the saved preferences information
     */
    
    memset((void *)&GSavedENetAddr, 0, sizeof(GSavedENetAddr));
    memset((void *)&GLastTime, 0, sizeof(GLastTime));
    GTimeAdjust = 0;
    GClockSeq = 0;

    return 0;
}

#if 0
// currently unused

/*    WritePrefData
 *
 *        Write the preferences data back out to my global variables.
 *    This gets called a couple of times. First, this is called by
 *    my GetRandomEthernet routine if I generated a psudorandom MAC
 *    address. Second, this is called when the library is being
 *    terminated through the __terminate() CFM call.
 *
 *        Note this does it's best attempt at writing the data out,
 *    and relies on ReadPrefData to check for integrety of the actual
 *    saved file.
 */

static void WritePrefData(void)
{
}

#endif


#if defined(__MACH__)

#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sockio.h>
#include <sys/uio.h>
#include <sys/errno.h>

#include <netinet/in.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>

#if !defined(MAX)
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#endif

#define IFR_NEXT(ifr)   \
    ((struct ifreq *) ((char *) (ifr) + sizeof(*(ifr)) + \
      MAX(0, (int) (ifr)->ifr_addr.sa_len - (int) sizeof((ifr)->ifr_addr))))

static OSErr GetEthernetAddr(uuid_address_t *addr) {
    struct ifconf ifc;
    struct ifreq ifrbuf[30], *ifr;
    register int s, i;
    Boolean foundIt = false;

    if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        return -1;
    }

    ifc.ifc_buf = (caddr_t)ifrbuf;
    ifc.ifc_len = sizeof (ifrbuf);
    if (ioctl(s, SIOCGIFCONF, &ifc) ==  -1) {
        close(s);
        return -1;
    }

    for (ifr = (struct ifreq *)ifc.ifc_buf, i=0; (char *)ifr < &ifc.ifc_buf[ifc.ifc_len]; ifr = IFR_NEXT(ifr), i++) {
        unsigned char *p, c;

        if (*ifr->ifr_name == '\0') {
            continue;
        }
        /*
         * Adapt to buggy kernel implementation (> 9 of a type)
         */

        p = &ifr->ifr_name[strlen(ifr->ifr_name)-1];
        if ((c = *p) > '0'+9) {
            sprintf(p, "%d", c-'0');
        }

        if (strcmp(ifr->ifr_name, "en0") == 0) {
            if (ifr->ifr_addr.sa_family == AF_LINK) {
                struct sockaddr_dl *sa = ((struct sockaddr_dl *)&ifr->ifr_addr);
                if (sa->sdl_type == IFT_ETHER || sa->sdl_type == IFT_FDDI || sa->sdl_type == IFT_ISO88023 || sa->sdl_type == IFT_ISO88024 || sa->sdl_type == IFT_ISO88025) {
                    for (i=0, p=&sa->sdl_data[sa->sdl_nlen] ; i++ < sa->sdl_alen; p++) {
                        addr->eaddr[i-1] = *p;
                    }
                    foundIt = true;
                    break;
                }
            }
        }
    }
    close(s);
    return (foundIt ? 0 : -1);
}

#elif defined (__WIN32__)

#error Dont know how to find Ethernet Address on Win32
// MF:!!! Maybe on Windows we should just call the COM library's UUID generator...

#elif defined (__LINUX__) || defined(__FREEBSD__)

static OSErr GetEthernetAddr(uuid_address_t *addr) {
    return -1;
}

#endif

#undef HI_WORD
#undef RAND_MASK
#undef TIME_MID_MASK
#undef TIME_HIGH_MASK
#undef TIME_HIGH_SHIFT_COUNT
#undef MAX_TIME_ADJUST
#undef CLOCK_SEQ_LOW_MASK
#undef CLOCK_SEQ_HIGH_MASK
#undef CLOCK_SEQ_HIGH_SHIFT_COUNT
#undef CLOCK_SEQ_FIRST
#undef CLOCK_SEQ_LAST
#undef CLOCK_SEQ_BIT_BANG
#undef CLOCK_SEQ_BUMP
#undef UUID_VERSION_BITS
#undef UUID_RESERVED_BITS
#undef IS_OLD_UUID
#undef EmptyArg
#undef UUID_VERIFY_INIT
#undef CHECK_STRUCTURE
#undef bCHECK_STRUCTURE
#undef vCHECK_STRUCTURE
#undef rCHECK_STRUCTURE
#undef uuid_c_os_base_time_diff_lo
#undef uuid_c_os_base_time_diff_hi
#undef UUID_C_100NS_PER_SEC
#undef UUID_C_100NS_PER_USEC
#undef UADD_UVLW_2_UVLW
#undef UADD_UW_2_UVLW
#undef IFR_NEXT

#endif

