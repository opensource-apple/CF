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
/*	CFSortFunctions.c
	Copyright 1999-2007, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

/* This file contains code copied from the Libc project's files
   qsort.c and merge.c, and modified to suit the
   needs of CF, which needs the comparison callback to have an
   additional parameter. The code is collected into this one
   file so that the individual BSD sort routines can remain
   private and static.
*/

#include <CoreFoundation/CFBase.h>
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
#include <sys/types.h>
#else
#define EINVAL		22
#endif
#include "CFInternal.h"


/* stdlib.subproj/qsort.c ============================================== */

/*-
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if 0
#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)qsort.c	8.1 (Berkeley) 6/4/93";
#endif /* LIBC_SCCS and not lint */
#include <sys/cdefs.h>
__FBSDID("$FreeBSD: src/lib/libc/stdlib/qsort.c,v 1.12 2002/09/10 02:04:49 wollman Exp $");
#endif

#include <stdlib.h>

#ifdef I_AM_QSORT_R
typedef int		 cmp_t(void *, const void *, const void *);
#else
typedef CFComparisonResult		 cmp_t(const void *, const void *, void *);
#endif
static inline char	*med3(char *, char *, char *, cmp_t *, void *);
static inline void	 swapfunc(char *, char *, long, long);

#if !defined(min)
#define min(a, b)	(a) < (b) ? a : b
#endif
/*
 * Qsort routine from Bentley & McIlroy's "Engineering a Sort Function".
 */
#define swapcode(TYPE, parmi, parmj, n) { 		\
	long i = (n) / sizeof (TYPE); 			\
	TYPE *pi = (TYPE *) (parmi); 		\
	TYPE *pj = (TYPE *) (parmj); 		\
	do { 						\
		TYPE	t = *pi;		\
		*pi++ = *pj;				\
		*pj++ = t;				\
        } while (--i > 0);				\
}

#define SWAPINIT(a, es) swaptype = ((char *)a - (char *)0) % sizeof(long) || \
	es % sizeof(long) ? 2 : es == sizeof(long)? 0 : 1;

static inline void
swapfunc(char *a, char *b, long n, long swaptype)
{
	if(swaptype <= 1)
		swapcode(long, a, b, n)
	else
		swapcode(char, a, b, n)
}

#define swap(a, b)					\
	if (swaptype == 0) {				\
		long t = *(long *)(a);			\
		*(long *)(a) = *(long *)(b);		\
		*(long *)(b) = t;			\
	} else						\
		swapfunc(a, b, es, swaptype)

#define vecswap(a, b, n) 	if ((n) > 0) swapfunc(a, b, n, swaptype)

#ifdef I_AM_QSORT_R
#define	CMP(t, x, y) (cmp((t), (x), (y)))
#else
#define	CMP(t, x, y) (cmp((x), (y), (t)))
#endif

static inline char *
med3(char *a, char *b, char *c, cmp_t *cmp, void *thunk)
{
	return CMP(thunk, a, b) < 0 ?
	       (CMP(thunk, b, c) < 0 ? b : (CMP(thunk, a, c) < 0 ? c : a ))
              :(CMP(thunk, b, c) > 0 ? b : (CMP(thunk, a, c) < 0 ? a : c ));
}

#ifdef I_AM_QSORT_R
void
qsort_r(void *a, size_t n, size_t es, void *thunk, cmp_t *cmp)
#else
static void
bsd_qsort(void *a, size_t n, size_t es, cmp_t *cmp, void *thunk)
#endif
{
	char *pa, *pb, *pc, *pd, *pl, *pm, *pn;
	long d, r, swaptype, swap_cnt;

loop:	SWAPINIT(a, es);
	swap_cnt = 0;
	if (n < 7) {
		for (pm = (char *)a + es; pm < (char *)a + n * es; pm += es)
			for (pl = pm; 
			     pl > (char *)a && CMP(thunk, pl - es, pl) > 0;
			     pl -= es)
				swap(pl, pl - es);
		return;
	}
	pm = (char *)a + (n / 2) * es;
	if (n > 7) {
		pl = a;
		pn = (char *)a + (n - 1) * es;
		if (n > 40) {
			d = (n / 8) * es;
			pl = med3(pl, pl + d, pl + 2 * d, cmp, thunk);
			pm = med3(pm - d, pm, pm + d, cmp, thunk);
			pn = med3(pn - 2 * d, pn - d, pn, cmp, thunk);
		}
		pm = med3(pl, pm, pn, cmp, thunk);
	}
	swap(a, pm);
	pa = pb = (char *)a + es;

	pc = pd = (char *)a + (n - 1) * es;
	for (;;) {
		while (pb <= pc && (r = CMP(thunk, pb, a)) <= 0) {
			if (r == 0) {
				swap_cnt = 1;
				swap(pa, pb);
				pa += es;
			}
			pb += es;
		}
		while (pb <= pc && (r = CMP(thunk, pc, a)) >= 0) {
			if (r == 0) {
				swap_cnt = 1;
				swap(pc, pd);
				pd -= es;
			}
			pc -= es;
		}
		if (pb > pc)
			break;
		swap(pb, pc);
		swap_cnt = 1;
		pb += es;
		pc -= es;
	}
	if (swap_cnt == 0) {  /* Switch to insertion sort */
		for (pm = (char *)a + es; pm < (char *)a + n * es; pm += es)
			for (pl = pm; 
			     pl > (char *)a && CMP(thunk, pl - es, pl) > 0;
			     pl -= es)
				swap(pl, pl - es);
		return;
	}

	pn = (char *)a + n * es;
	r = min(pa - (char *)a, pb - pa);
	vecswap(a, pb - r, r);
	r = min(pd - pc, pn - pd - es);
	vecswap(pb, pn - r, r);
	if ((r = pb - pa) > es)
#ifdef I_AM_QSORT_R
		qsort_r(a, r / es, es, thunk, cmp);
#else
		bsd_qsort(a, r / es, es, cmp, thunk);
#endif
	if ((r = pd - pc) > es) {
		/* Iterate rather than recurse to save stack space */
		a = pn - r;
		n = r / es;
		goto loop;
	}
/*		qsort(pn - r, r / es, es, cmp);*/
}


/* And now for something not so completely different, a copy/paste version that uses write-barriers so as to notify GC as necessary of changes */
#define ASSIGN __CFObjCStrongAssign
//#define ASSIGN objc_assign_strongCast

#define swapcode_wb(TYPE, parmi, parmj, n) { 		\
	long i = (n) / sizeof (TYPE); 			\
	TYPE *pi = (TYPE *) (parmi); 		\
	TYPE *pj = (TYPE *) (parmj); 		\
	do { 						\
		TYPE	t = *pi;		\
                ASSIGN(*pj, pi++);   \
                ASSIGN(t, pj++); \
        } while (--i > 0);				\
}


static inline void
swapfunc_wb(char *a, char *b, long n, long swaptype)
{
	if(swaptype <= 1)
		swapcode_wb(void *, a, b, n)
	else
		swapcode(char, a, b, n)
}

#define swap_wb(a, b)					\
	if (swaptype == 0) {				\
		const void *t = *(const void **)(a);			\
		ASSIGN(*(void **)(b), (const void **)a); \
		ASSIGN(t, (const void **)(b));    \
	} else \
		printf("bad things happening\n");
                //swapfunc_wb(a, b, es, swaptype)

#define vecswap_wb(a, b, n) 	if ((n) > 0) swapfunc_wb(a, b, n, swaptype)

static void
bsd_qsort_wb(void *a, size_t n, size_t es, cmp_t *cmp, void *thunk)
{
	char *pa, *pb, *pc, *pd, *pl, *pm, *pn;
	long d, r, swaptype, swap_cnt;

loop:	SWAPINIT(a, es);
	swap_cnt = 0;
	if (n < 7) {
		for (pm = (char *)a + es; pm < (char *)a + n * es; pm += es)
			for (pl = pm; 
			     pl > (char *)a && CMP(thunk, pl - es, pl) > 0;
			     pl -= es)
				swap_wb(pl, pl - es);
		return;
	}
	pm = (char *)a + (n / 2) * es;
	if (n > 7) {
		pl = a;
		pn = (char *)a + (n - 1) * es;
		if (n > 40) {
			d = (n / 8) * es;
			pl = med3(pl, pl + d, pl + 2 * d, cmp, thunk);
			pm = med3(pm - d, pm, pm + d, cmp, thunk);
			pn = med3(pn - 2 * d, pn - d, pn, cmp, thunk);
		}
		pm = med3(pl, pm, pn, cmp, thunk);
	}
	swap_wb(a, pm);
	pa = pb = (char *)a + es;

	pc = pd = (char *)a + (n - 1) * es;
	for (;;) {
		while (pb <= pc && (r = CMP(thunk, pb, a)) <= 0) {
			if (r == 0) {
				swap_cnt = 1;
				swap_wb(pa, pb);
				pa += es;
			}
			pb += es;
		}
		while (pb <= pc && (r = CMP(thunk, pc, a)) >= 0) {
			if (r == 0) {
				swap_cnt = 1;
				swap_wb(pc, pd);
				pd -= es;
			}
			pc -= es;
		}
		if (pb > pc)
			break;
		swap_wb(pb, pc);
		swap_cnt = 1;
		pb += es;
		pc -= es;
	}
	if (swap_cnt == 0) {  /* Switch to insertion sort */
		for (pm = (char *)a + es; pm < (char *)a + n * es; pm += es)
			for (pl = pm; 
			     pl > (char *)a && CMP(thunk, pl - es, pl) > 0;
			     pl -= es)
				swap_wb(pl, pl - es);
		return;
	}

	pn = (char *)a + n * es;
	r = min(pa - (char *)a, pb - pa);
	vecswap_wb(a, pb - r, r);
	r = min(pd - pc, pn - pd - es);
	vecswap_wb(pb, pn - r, r);
	if ((r = pb - pa) > es)
		bsd_qsort_wb(a, r / es, es, cmp, thunk);
	if ((r = pd - pc) > es) {
		/* Iterate rather than recurse to save stack space */
		a = pn - r;
		n = r / es;
		goto loop;
	}
/*		qsort(pn - r, r / es, es, cmp);*/
}

/* Comparator is passed the address of the values. */
void CFQSortArray(void *list, CFIndex count, CFIndex elementSize, CFComparatorFunction comparator, void *context) {
    if (CF_USING_COLLECTABLE_MEMORY && (auto_zone_get_layout_type(__CFCollectableZone, list) & AUTO_UNSCANNED) == 0)
        bsd_qsort_wb(list, count, elementSize, comparator, context);
    else
        bsd_qsort(list, count, elementSize, comparator, context);
}

#undef thunk
#undef CMP
#undef vecswap
//#undef swap
#undef SWAPINIT
#undef swapcode
#undef min

/* stdlib.subproj/mergesort.c ========================================== */

/*-
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Peter McIlroy.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if 0
#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)merge.c	8.2 (Berkeley) 2/14/94";
#endif /* LIBC_SCCS and not lint */
#include <sys/cdefs.h>
__FBSDID("$FreeBSD: src/lib/libc/stdlib/merge.c,v 1.6 2002/03/21 22:48:42 obrien Exp $");
#endif

/*
 * Hybrid exponential search/linear search merge sort with hybrid
 * natural/pairwise first pass.  Requires about .3% more comparisons
 * for random data than LSMS with pairwise first pass alone.
 * It works for objects as small as two bytes.
 */

#define NATURAL
#define THRESHOLD 16	/* Best choice for natural merge cut-off. */

/* #define NATURAL to get hybrid natural merge.
 * (The default is pairwise merging.)
 */

#include <sys/types.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void setup(u_char *, u_char *, size_t, size_t, CFComparisonResult (*)(), void *);
static void insertionsort(u_char *, size_t, size_t, CFComparisonResult (*)(), void *);

#define ISIZE sizeof(long)
#define PSIZE sizeof(u_char *)
#define ICOPY_LIST(src, dst, last)				\
	do							\
	*(long*)dst = *(long*)src, src += ISIZE, dst += ISIZE;	\
	while(src < last)
#define ICOPY_ELT(src, dst, i)					\
	do							\
	*(long*) dst = *(long*) src, src += ISIZE, dst += ISIZE;	\
	while (i -= ISIZE)

#define CCOPY_LIST(src, dst, last)		\
	do					\
		*dst++ = *src++;		\
	while (src < last)
#define CCOPY_ELT(src, dst, i)			\
	do					\
		*dst++ = *src++;		\
	while (i -= 1)

/*
 * Find the next possible pointer head.  (Trickery for forcing an array
 * to do double duty as a linked list when objects do not align with word
 * boundaries.
 */
/* Assumption: PSIZE is a power of 2. */
#define EVAL(p) (u_char **)						\
	((u_char *)0 +							\
	    (((u_char *)p + PSIZE - 1 - (u_char *) 0) & ~(PSIZE - 1)))

/*
 * Arguments are as for qsort.
 */
static int
bsd_mergesort(void *base, size_t nmemb, size_t size, CFComparisonResult (*cmp)(const void *, const void *, void *), void *context)
{
	long i, sense;
	long big, iflag;
	u_char *f1, *f2, *t, *b, *tp2, *q, *l1, *l2;
	u_char *list2, *list1, *p2, *p, *last, **p1;

	if (size < PSIZE / 2) {		/* Pointers must fit into 2 * size. */
		errno = EINVAL;
		return (-1);
	}

	if (nmemb == 0)
		return (0);

	/*
	 * XXX
	 * Stupid subtraction for the Cray.
	 */
	iflag = 0;
	if (!(size % ISIZE) && !(((char *)base - (char *)0) % ISIZE))
		iflag = 1;

	if ((list2 = malloc(nmemb * size + PSIZE)) == NULL)
		return (-1);

	list1 = base;
	setup(list1, list2, nmemb, size, cmp, context);
	last = list2 + nmemb * size;
	i = big = 0;
	while (*EVAL(list2) != last) {
	    l2 = list1;
	    p1 = EVAL(list1);
	    for (tp2 = p2 = list2; p2 != last; p1 = EVAL(l2)) {
	    	p2 = *EVAL(p2);
	    	f1 = l2;
	    	f2 = l1 = list1 + (p2 - list2);
	    	if (p2 != last)
	    		p2 = *EVAL(p2);
	    	l2 = list1 + (p2 - list2);
	    	while (f1 < l1 && f2 < l2) {
	    		if ((*cmp)(f1, f2, context) <= 0) {
	    			q = f2;
	    			b = f1, t = l1;
	    			sense = -1;
	    		} else {
	    			q = f1;
	    			b = f2, t = l2;
	    			sense = 0;
	    		}
	    		if (!big) {	/* here i = 0 */
				while ((b += size) < t && cmp(q, b, context) >sense)
	    				if (++i == 6) {
	    					big = 1;
	    					goto EXPONENTIAL;
	    				}
	    		} else {
EXPONENTIAL:	    		for (i = size; ; i <<= 1)
	    				if ((p = (b + i)) >= t) {
	    					if ((p = t - size) > b &&
						    (*cmp)(q, p, context) <= sense)
	    						t = p;
	    					else
	    						b = p;
	    					break;
	    				} else if ((*cmp)(q, p, context) <= sense) {
	    					t = p;
	    					if (i == size)
	    						big = 0;
	    					goto FASTCASE;
	    				} else
	    					b = p;
				while (t > b+size) {
	    				i = (((t - b) / size) >> 1) * size;
	    				if ((*cmp)(q, p = b + i, context) <= sense)
	    					t = p;
	    				else
	    					b = p;
	    			}
	    			goto COPY;
FASTCASE:	    		while (i > size)
	    				if ((*cmp)(q,
	    					p = b + (i >>= 1), context) <= sense)
	    					t = p;
	    				else
	    					b = p;
COPY:	    			b = t;
	    		}
	    		i = size;
	    		if (q == f1) {
	    			if (iflag) {
	    				ICOPY_LIST(f2, tp2, b);
	    				ICOPY_ELT(f1, tp2, i);
	    			} else {
	    				CCOPY_LIST(f2, tp2, b);
	    				CCOPY_ELT(f1, tp2, i);
	    			}
	    		} else {
	    			if (iflag) {
	    				ICOPY_LIST(f1, tp2, b);
	    				ICOPY_ELT(f2, tp2, i);
	    			} else {
	    				CCOPY_LIST(f1, tp2, b);
	    				CCOPY_ELT(f2, tp2, i);
	    			}
	    		}
	    	}
	    	if (f2 < l2) {
	    		if (iflag)
	    			ICOPY_LIST(f2, tp2, l2);
	    		else
	    			CCOPY_LIST(f2, tp2, l2);
	    	} else if (f1 < l1) {
	    		if (iflag)
	    			ICOPY_LIST(f1, tp2, l1);
	    		else
	    			CCOPY_LIST(f1, tp2, l1);
	    	}
	    	*p1 = l2;
	    }
	    tp2 = list1;	/* swap list1, list2 */
	    list1 = list2;
	    list2 = tp2;
	    last = list2 + nmemb*size;
	}
	if (base == list2) {
		memmove(list2, list1, nmemb*size);
		list2 = list1;
	}
	free(list2);
	return (0);
}

#define	swap(a, b) {					\
		s = b;					\
		i = size;				\
		do {					\
			tmp = *a; *a++ = *s; *s++ = tmp; \
		} while (--i);				\
		a -= size;				\
	}
#define reverse(bot, top) {				\
	s = top;					\
	do {						\
		i = size;				\
		do {					\
			tmp = *bot; *bot++ = *s; *s++ = tmp; \
		} while (--i);				\
		s -= size2;				\
	} while(bot < s);				\
}

/*
 * Optional hybrid natural/pairwise first pass.  Eats up list1 in runs of
 * increasing order, list2 in a corresponding linked list.  Checks for runs
 * when THRESHOLD/2 pairs compare with same sense.  (Only used when NATURAL
 * is defined.  Otherwise simple pairwise merging is used.)
 */
static void
setup(u_char *list1, u_char *list2, size_t n, size_t size, CFComparisonResult (*cmp)(const void *, const void *, void *), void *context)
{
	long i, length, size2, tmp, sense;
	u_char *f1, *f2, *s, *l2, *last, *p2;

	size2 = size*2;
	if (n <= 5) {
		insertionsort(list1, n, size, cmp, context);
		*EVAL(list2) = (u_char*) list2 + n*size;
		return;
	}
	/*
	 * Avoid running pointers out of bounds; limit n to evens
	 * for simplicity.
	 */
	i = 4 + (n & 1);
	insertionsort(list1 + (n - i) * size, i, size, cmp, context);
	last = list1 + size * (n - i);
	*EVAL(list2 + (last - list1)) = list2 + n * size;

#ifdef NATURAL
	p2 = list2;
	f1 = list1;
	sense = (cmp(f1, f1 + size, context) > 0);
	for (; f1 < last; sense = !sense) {
		length = 2;
					/* Find pairs with same sense. */
		for (f2 = f1 + size2; f2 < last; f2 += size2) {
			if ((cmp(f2, f2+ size, context) > 0) != sense)
				break;
			length += 2;
		}
		if (length < THRESHOLD) {		/* Pairwise merge */
			do {
				p2 = *EVAL(p2) = f1 + size2 - list1 + list2;
				if (sense > 0)
					swap (f1, f1 + size);
			} while ((f1 += size2) < f2);
		} else {				/* Natural merge */
			l2 = f2;
			for (f2 = f1 + size2; f2 < l2; f2 += size2) {
				if ((cmp(f2-size, f2, context) > 0) != sense) {
					p2 = *EVAL(p2) = f2 - list1 + list2;
					if (sense > 0)
						reverse(f1, f2-size);
					f1 = f2;
				}
			}
			if (sense > 0)
				reverse (f1, f2-size);
			f1 = f2;
			if (f2 < last || cmp(f2 - size, f2, context) > 0)
				p2 = *EVAL(p2) = f2 - list1 + list2;
			else
				p2 = *EVAL(p2) = list2 + n*size;
		}
	}
#else		/* pairwise merge only. */
	for (f1 = list1, p2 = list2; f1 < last; f1 += size2) {
		p2 = *EVAL(p2) = p2 + size2;
		if (cmp (f1, f1 + size, context) > 0)
			swap(f1, f1 + size);
	}
#endif /* NATURAL */
}

/*
 * This is to avoid out-of-bounds addresses in sorting the
 * last 4 elements.
 */
static void
insertionsort(u_char *a, size_t n, size_t size, CFComparisonResult (*cmp)(const void *, const void *, void *), void *context)
{
	u_char *ai, *s, *t, *u, tmp;
	long i;

	for (ai = a+size; --n >= 1; ai += size)
		for (t = ai; t > a; t -= size) {
			u = t - size;
			if (cmp(u, t, context) <= 0)
				break;
			swap(u, t);
		}
}

/* Another version, also not so completely different, in order to handle necessary write-barriers in the face of GC */

#undef ASSIGN
#define ASSIGN __CFObjCStrongAssign
//#define ASSIGN log_assign

static void setup_wb(u_char *, u_char *, size_t, size_t, CFComparisonResult (*)(), void *);
static void insertionsort_wb(u_char *, size_t, size_t, CFComparisonResult (*)(), void *);

#undef ICOPY_ELT

#define ICOPY_ELT(src, dst, i)					\
	do							\
	ASSIGN(*(const void**)src, (const void *)dst), src += ISIZE, dst += ISIZE;	\
	while (i -= ISIZE)

#undef ICOPY_LIST

#define ICOPY_LIST(src, dst, last) \
        do \
        ASSIGN(*(const void **)src, (const void *)dst), src += ISIZE, dst += ISIZE; \
        while (src < last)


/*
 * Arguments are as for qsort.
 */
static int
bsd_mergesort_wb(void *base, size_t nmemb, size_t size, CFComparisonResult (*cmp)(const void *, const void *, void *), void *context)
{
	long i, sense;
	long big, iflag;
	u_char *f1, *f2, *t, *b, *tp2, *q, *l1, *l2;
	u_char *list2, *list1, *p2, *p, *last, **p1;

	if (size < PSIZE / 2) {		/* Pointers must fit into 2 * size. */
		errno = EINVAL;
		return (-1);
	}

	if (nmemb == 0)
		return (0);

	/*
	 * XXX
	 * Stupid subtraction for the Cray.
	 */
	iflag = 0;
	if (!(size % ISIZE) && !(((char *)base - (char *)0) % ISIZE))
		iflag = 1;

        if (!iflag)
            return -1;      // only set up for "integer" swaps, e.g. long integer
            
	if ((list2 = CFAllocatorAllocate(NULL, (nmemb * size + PSIZE), __kCFAllocatorGCScannedMemory)) == NULL)
		return (-1);

	list1 = base;
	setup_wb(list1, list2, nmemb, size, cmp, context);
	last = list2 + nmemb * size;
	i = big = 0;
	while (*EVAL(list2) != last) {
	    l2 = list1;
	    p1 = EVAL(list1);
	    for (tp2 = p2 = list2; p2 != last; p1 = EVAL(l2)) {
	    	p2 = *EVAL(p2);
	    	f1 = l2;
	    	f2 = l1 = list1 + (p2 - list2);
	    	if (p2 != last)
	    		p2 = *EVAL(p2);
	    	l2 = list1 + (p2 - list2);
	    	while (f1 < l1 && f2 < l2) {
	    		if ((*cmp)(f1, f2, context) <= 0) {
	    			q = f2;
	    			b = f1, t = l1;
	    			sense = -1;
	    		} else {
	    			q = f1;
	    			b = f2, t = l2;
	    			sense = 0;
	    		}
	    		if (!big) {	/* here i = 0 */
				while ((b += size) < t && cmp(q, b, context) >sense)
	    				if (++i == 6) {
	    					big = 1;
	    					goto EXPONENTIAL;
	    				}
	    		} else {
EXPONENTIAL:	    		for (i = size; ; i <<= 1)
	    				if ((p = (b + i)) >= t) {
	    					if ((p = t - size) > b &&
						    (*cmp)(q, p, context) <= sense)
	    						t = p;
	    					else
	    						b = p;
	    					break;
	    				} else if ((*cmp)(q, p, context) <= sense) {
	    					t = p;
	    					if (i == size)
	    						big = 0;
	    					goto FASTCASE;
	    				} else
	    					b = p;
				while (t > b+size) {
	    				i = (((t - b) / size) >> 1) * size;
	    				if ((*cmp)(q, p = b + i, context) <= sense)
	    					t = p;
	    				else
	    					b = p;
	    			}
	    			goto COPY;
FASTCASE:	    		while (i > size)
	    				if ((*cmp)(q,
	    					p = b + (i >>= 1), context) <= sense)
	    					t = p;
	    				else
	    					b = p;
COPY:	    			b = t;
	    		}
	    		i = size;
	    		if (q == f1) {
	    			if (iflag) {
	    				ICOPY_LIST(f2, tp2, b);
	    				ICOPY_ELT(f1, tp2, i);
	    			} else {
	    				CCOPY_LIST(f2, tp2, b);
	    				CCOPY_ELT(f1, tp2, i);
	    			}
	    		} else {
	    			if (iflag) {
	    				ICOPY_LIST(f1, tp2, b);
	    				ICOPY_ELT(f2, tp2, i);
	    			} else {
	    				CCOPY_LIST(f1, tp2, b);
	    				CCOPY_ELT(f2, tp2, i);
	    			}
	    		}
	    	}
	    	if (f2 < l2) {
	    		if (iflag)
	    			ICOPY_LIST(f2, tp2, l2);
	    		else
	    			CCOPY_LIST(f2, tp2, l2);
	    	} else if (f1 < l1) {
	    		if (iflag)
	    			ICOPY_LIST(f1, tp2, l1);
	    		else
	    			CCOPY_LIST(f1, tp2, l1);
	    	}
	    	*p1 = l2;
	    }
	    tp2 = list1;	/* swap list1, list2 */
	    list1 = list2;
	    list2 = tp2;
	    last = list2 + nmemb*size;
	}
	if (base == list2) {
		CF_WRITE_BARRIER_MEMMOVE(list2, list1, nmemb*size);
		list2 = list1;
	}
	free(list2);
	return (0);
}


#define swap_wb(a, b) { \
        const void *object = *(void **)a; \
        ASSIGN(*(const void **)b, (const void *)a); \
        ASSIGN(object, (const void *)b); \
    }

#define reverse_wb(bot, top) {				\
	s = top;					\
	do {						\
                swap_wb(bot, s);                           \
                bot += size;                           \
		s -= size;				\
	} while(bot < s);				\
}

/*
 * Optional hybrid natural/pairwise first pass.  Eats up list1 in runs of
 * increasing order, list2 in a corresponding linked list.  Checks for runs
 * when THRESHOLD/2 pairs compare with same sense.  (Only used when NATURAL
 * is defined.  Otherwise simple pairwise merging is used.)
 */
static void
setup_wb(u_char *list1, u_char *list2, size_t n, size_t size, CFComparisonResult (*cmp)(const void *, const void *, void *), void *context)
{
	long i, length, size2, tmp, sense;
	u_char *f1, *f2, *s, *l2, *last, *p2;

	size2 = size*2;
	if (n <= 5) {
		insertionsort_wb(list1, n, size, cmp, context);
		*EVAL(list2) = (u_char*) list2 + n*size;
		return;
	}
	/*
	 * Avoid running pointers out of bounds; limit n to evens
	 * for simplicity.
	 */
	i = 4 + (n & 1);
	insertionsort_wb(list1 + (n - i) * size, i, size, cmp, context);
	last = list1 + size * (n - i);
	*EVAL(list2 + (last - list1)) = list2 + n * size;

#ifdef NATURAL
	p2 = list2;
	f1 = list1;
	sense = (cmp(f1, f1 + size, context) > 0);
	for (; f1 < last; sense = !sense) {
		length = 2;
					/* Find pairs with same sense. */
		for (f2 = f1 + size2; f2 < last; f2 += size2) {
			if ((cmp(f2, f2+ size, context) > 0) != sense)
				break;
			length += 2;
		}
		if (length < THRESHOLD) {		/* Pairwise merge */
			do {
				p2 = *EVAL(p2) = f1 + size2 - list1 + list2;
				if (sense > 0)
					swap (f1, f1 + size);
			} while ((f1 += size2) < f2);
		} else {				/* Natural merge */
			l2 = f2;
			for (f2 = f1 + size2; f2 < l2; f2 += size2) {
				if ((cmp(f2-size, f2, context) > 0) != sense) {
					p2 = *EVAL(p2) = f2 - list1 + list2;
					if (sense > 0) {
						reverse_wb(f1, f2-size);
                                        }
					f1 = f2;
				}
			}
			if (sense > 0) {
				reverse_wb (f1, f2-size);
                        }
			f1 = f2;
			if (f2 < last || cmp(f2 - size, f2, context) > 0) {
				p2 = *EVAL(p2) = f2 - list1 + list2;
                        }
			else {
				p2 = *EVAL(p2) = list2 + n*size;
                        }
		}
	}
#else		/* pairwise merge only. */
#error unchanged
	for (f1 = list1, p2 = list2; f1 < last; f1 += size2) {
		p2 = *EVAL(p2) = p2 + size2;
		if (cmp (f1, f1 + size, context) > 0)
			swap_wb(f1, f1 + size);
	}
#endif /* NATURAL */
}

/*
 * This is to avoid out-of-bounds addresses in sorting the
 * last 4 elements.
 */
static void
insertionsort_wb(u_char *a, size_t n, size_t size, CFComparisonResult (*cmp)(const void *, const void *, void *), void *context)
{
	u_char *ai, *s, *t, *u, tmp;
	long i;

	for (ai = a+size; --n >= 1; ai += size)
		for (t = ai; t > a; t -= size) {
			u = t - size;
			if (cmp(u, t, context) <= 0)
				break;
			swap(u, t);
		}
}

void CFMergeSortArray(void *list, CFIndex count, CFIndex elementSize, CFComparatorFunction comparator, void *context) {
    if (CF_USING_COLLECTABLE_MEMORY && (auto_zone_get_layout_type(__CFCollectableZone, list) & AUTO_UNSCANNED) == 0)
        bsd_mergesort_wb(list, count, elementSize, comparator, context);
    else
        bsd_mergesort(list, count, elementSize, comparator, context);
} 

#undef NATURAL
#undef THRESHOLD
#undef ISIZE
#undef PSIZE
#undef ICOPY_LIST
#undef ICOPY_ELT
#undef CCOPY_LIST
#undef CCOPY_ELT
#undef EVAL
#undef swap
#undef reverse

/* ===================================================================== */

#undef EINVAL

