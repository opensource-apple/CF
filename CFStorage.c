/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
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

/*	CFStorage.c
	Copyright (c) 1999-2009, Apple Inc. All rights reserved.
	Responsibility: Ali Ozer
*/

/*
2-3 tree storing arbitrary sized values.

??? Currently elementSize cannot be greater than storage->maxLeafCapacity, which is less than or equal to __CFStorageMaxLeafCapacity

CFStorage is thread-safe for multiple readers, but not thread safe for simultaneous reading and  writing.
*/

#include "CFStorage.h"
#include "CFInternal.h"

#if !defined(PAGE_SIZE)
#define PAGE_SIZE 4096
#endif

// Above 15K, malloc switches (??? or used to in early Leopard) to using vm allocates for blocks; it seems best to avoid that.
// Also, tests with StorageTimer.c done in 4/07 indicate that 4096 * 3 is better than smaller or larger node sizes.
#define __CFStorageMaxLeafCapacity (4096 * 3)

// If the max length of a node is less than can fit in a half-word (half of a long), we can get away without ever caching the high half-word the cachedRange
#if __LP64__
#if __CFStorageMaxLeafCapacity > 0xFFFFFFFFULL
#define POSSIBLE_TO_HAVE_LENGTH_MORE_THAN_HALFWORD 1
#endif
#else
#if __CFStorageMaxLeafCapacity > 0xFFFFUL
#define POSSIBLE_TO_HAVE_LENGTH_MORE_THAN_HALFWORD 1
#endif
#endif

#define COPYMEM(src,dst,n) objc_memmove_collectable((dst), (src), (n))
#define PAGE_LIMIT ((CFIndex)PAGE_SIZE / 2)

CF_INLINE int32_t roundToPage(int32_t num) {
    return (num + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}



/* Each node in the storage.  isLeaf determines whether the node is a leaf node or a node inside the tree. If latter, number of children are determined by the number of non-NULL entries in child[]. (NULL entries are at the end.)
*/
typedef struct __CFStorageNode {
    CFIndex numBytes;	/* Number of actual bytes in this node and all its children */
    bool isLeaf;
    union {
        struct {
            CFIndex capacityInBytes;	// capacityInBytes is capacity of memory; this is either 0, or >= numBytes
            uint8_t *memory;
        } leaf;
        struct {
            struct __CFStorageNode *child[3];
        } notLeaf;
    } info;
} CFStorageNode;

/* This is the struct used to store the cache in the CFStorage; it enables us to make the cache thread-safe for multiple readers (which update the cache).  The values in this cache store half the desired value in the top half, and the genCount of the writer in the low half. This cache is consistent only if all of the genCounts are the same.  Note that the cache does not provide thread-safety for readers in the presence of a writer.

The cached range (location, length) is in terms of values; if the cached range is not (0,0), then the cached node needs to be non-NULL and pointing at a leaf node.
*/
typedef struct {
    unsigned long locationHi, locationLo;	// cachedRange.location
#if POSSIBLE_TO_HAVE_LENGTH_MORE_THAN_HALFWORD
    unsigned long lengthHi;		    
#endif
    unsigned long lengthLo;			// cachedRange.length; note that we actually do not bother with lengthHi if __CFStorageMaxLeafCapacity is less than half-word
    unsigned long cachedNodeHi, cachedNodeLo;	// cachedNode
} CFStorageAccessCacheParts;

/* The CFStorage object.
*/
struct __CFStorage {
    CFRuntimeBase base;
    CFIndex valueSize;
    CFSpinLock_t cacheReaderMemoryAllocationLock;
    int cacheGenerationCount;
    CFStorageAccessCacheParts cacheParts;
    CFIndex maxLeafCapacity;	    // In terms of bytes
    CFStorageNode rootNode;
    CFOptionFlags nodeHint;	    // __kCFAllocatorGCScannedMemory or 0.
};




/* Allocates the memory and initializes the capacity in a leaf. __CFStorageAllocLeafNodeMemory() is the entry point; __CFStorageAllocLeafNodeMemoryAux is called if actual reallocation is needed. __CFStorageAllocLeafNodeMemoryAux() locks not for mutations (mutations are not thread-safe in general), but for lazy allocation of storage during reading.
*/
static void __CFStorageAllocLeafNodeMemoryAux(CFAllocatorRef allocator, CFStorageRef storage, CFStorageNode *node, CFIndex cap) {
    __CFSpinLock(&(storage->cacheReaderMemoryAllocationLock));
    __CFAssignWithWriteBarrier((void **)&node->info.leaf.memory, _CFAllocatorReallocateGC(allocator, node->info.leaf.memory, cap, storage->nodeHint));	// This will free...
    if (__CFOASafe) __CFSetLastAllocationEventName(node->info.leaf.memory, "CFStorage (node bytes)");
    node->info.leaf.capacityInBytes = cap;
    __CFSpinUnlock(&(storage->cacheReaderMemoryAllocationLock));
}

CF_INLINE void __CFStorageAllocLeafNodeMemory(CFAllocatorRef allocator, CFStorageRef storage, CFStorageNode *node, CFIndex cap, bool compact) {
    if (cap > PAGE_LIMIT) {
        cap = roundToPage(cap);
	if (cap > storage->maxLeafCapacity) cap = storage->maxLeafCapacity;
    } else {
        cap = (((cap + 63) / 64) * 64);
    }
    if (compact ? (cap != node->info.leaf.capacityInBytes) : (cap > node->info.leaf.capacityInBytes)) __CFStorageAllocLeafNodeMemoryAux(allocator, storage, node, cap);
}




#if __LP64__
#define genCountMask	0x00000000FFFFFFFFUL
#define dataMask	0xFFFFFFFF00000000UL
#define shiftLowWordBy	32
#else
#define genCountMask	0x0000FFFFUL
#define dataMask	0xFFFF0000UL
#define shiftLowWordBy	16
#endif

/* Sets the cache to point at the specified node. loc and len are in terms of values, not bytes. To clear the cache set these two to 0.
   At least one of node or memory should be non-NULL. memory is consulted first when using the cache.
*/
CF_INLINE void __CFStorageSetCache(CFStorageRef storage, CFStorageNode *node, CFIndex loc, CFIndex len) {
    unsigned int genCount = ((unsigned int)OSAtomicIncrement32(&storage->cacheGenerationCount)) & genCountMask;
    CFStorageAccessCacheParts cacheParts;
    cacheParts.locationHi = (loc & dataMask) | genCount;
    cacheParts.locationLo = (loc << shiftLowWordBy) | genCount;
#if POSSIBLE_TO_HAVE_LENGTH_MORE_THAN_HALFWORD
    cacheParts.lengthHi = (len & dataMask) | genCount;
#endif
    cacheParts.lengthLo = (len << shiftLowWordBy) | genCount;
    cacheParts.cachedNodeHi = ((unsigned long)node & dataMask) | genCount;
    cacheParts.cachedNodeLo = ((unsigned long)node << shiftLowWordBy) | genCount;
    storage->cacheParts = cacheParts;
}

/* Thread-safely get the cached range and node info.
*/
CF_INLINE Boolean __CFStorageGetCache(CFStorageRef storage, CFRange *cachedRangePtr, CFStorageNode **cachedNodePtr) {
    CFStorageAccessCacheParts cacheParts = storage->cacheParts;

    unsigned int genCount = cacheParts.locationHi & genCountMask;
    
    // Check to make sure the genCounts of all the items are the same; if not, the cache was inconsistent
    if ((cacheParts.locationLo & genCountMask) == genCount &&
#if POSSIBLE_TO_HAVE_LENGTH_MORE_THAN_HALFWORD
	(cacheParts.lengthHi & genCountMask) == genCount &&
#endif
	(cacheParts.lengthLo & genCountMask) == genCount &&
	(cacheParts.cachedNodeHi & genCountMask) == genCount &&
	(cacheParts.cachedNodeLo & genCountMask) == genCount) {
	
	*cachedNodePtr = (CFStorageNode *)((cacheParts.cachedNodeHi & dataMask) | ((cacheParts.cachedNodeLo & dataMask) >> shiftLowWordBy));
	cachedRangePtr->location = (cacheParts.locationHi & dataMask) | ((cacheParts.locationLo & dataMask) >> shiftLowWordBy);
	cachedRangePtr->length = (cacheParts.lengthLo & dataMask) >> shiftLowWordBy;
#if POSSIBLE_TO_HAVE_LENGTH_MORE_THAN_HALFWORD
	cachedRangePtr->length |= (cacheParts.lengthHi & dataMask);
#endif

	return true;
    }
    return false;
}

/* Gets the location for the specified absolute loc from the cached info.
   Returns NULL if the location is not in the cache.
*/
CF_INLINE uint8_t *__CFStorageGetFromCache(CFStorageRef storage, CFIndex loc, CFRange *validConsecutiveValueRange) {
    CFRange cachedRange = {0, 0};
    CFStorageNode *cachedNode = 0;

    // If we can't get values from the cache, return NULL
    if (!__CFStorageGetCache(storage, &cachedRange, &cachedNode)) return NULL;

    // Check to see if the index is in the cache
    if (loc < cachedRange.location || loc >= cachedRange.location + cachedRange.length) return NULL;

    // If the cached node has no memory, return here; it will be allocated as a result of the non-cached lookup.
    if (!cachedNode->info.leaf.memory) return NULL;
    
    // The cache has consistent values, and in fact, the values we're looking for!
    uint8_t *result = cachedNode->info.leaf.memory + (loc - cachedRange.location) * storage->valueSize;
    *validConsecutiveValueRange = cachedRange;
    
    return result;
}




/* Returns the number of the child containing the desired value and the relative index of the value in that child.
   forInsertion = true means that we are looking for the child in which to insert; this changes the behavior when the index is at the end of a child
   relativeByteNum (not optional, for performance reasons) returns the relative byte number of the specified byte in the child.
   Don't call with leaf nodes!
*/
CF_INLINE void __CFStorageFindChild(CFStorageNode *node, CFIndex byteNum, bool forInsertion, CFIndex *childNum, CFIndex *relativeByteNum) {
    if (forInsertion) byteNum--;	/* If for insertion, we do <= checks, not <, so this accomplishes the same thing */
    if (byteNum < node->info.notLeaf.child[0]->numBytes) *childNum = 0;
    else {
        byteNum -= node->info.notLeaf.child[0]->numBytes;
        if (byteNum < node->info.notLeaf.child[1]->numBytes) *childNum = 1;
        else {
            byteNum -= node->info.notLeaf.child[1]->numBytes;
            *childNum = 2;
        }
    }
    if (forInsertion) byteNum++;
    *relativeByteNum = byteNum;
}

/* Finds the location where the specified byte is stored. If validConsecutiveByteRange is not NULL, returns
   the range of bytes that are consecutive with this one.
   !!! Assumes the byteNum is within the range of this node.
*/
static void *__CFStorageFindByte(CFStorageRef storage, CFStorageNode *node, CFIndex byteNum, CFStorageNode **resultNode, CFRange *validConsecutiveByteRange) {
    if (node->isLeaf) {
        if (validConsecutiveByteRange) *validConsecutiveByteRange = CFRangeMake(0, node->numBytes);
        __CFStorageAllocLeafNodeMemory(CFGetAllocator(storage), storage, node, node->numBytes, false);
	if (resultNode) *resultNode = node;
        return node->info.leaf.memory + byteNum; 
    } else {
        void *result;
        CFIndex childNum;
        CFIndex relativeByteNum;
        __CFStorageFindChild(node, byteNum, false, &childNum, &relativeByteNum);
        result = __CFStorageFindByte(storage, node->info.notLeaf.child[childNum], relativeByteNum, resultNode, validConsecutiveByteRange);
        if (validConsecutiveByteRange) {
            if (childNum > 0) validConsecutiveByteRange->location += node->info.notLeaf.child[0]->numBytes;
            if (childNum > 1) validConsecutiveByteRange->location += node->info.notLeaf.child[1]->numBytes;
        }
        return result;
    }
}

/* Guts of CFStorageGetValueAtIndex(); note that validConsecutiveValueRange is not optional.
   Consults and updates cache.
*/
CF_INLINE void *__CFStorageGetValueAtIndex(CFStorageRef storage, CFIndex idx, CFRange *validConsecutiveValueRange) {
    uint8_t *result;
    if (!(result = __CFStorageGetFromCache(storage, idx, validConsecutiveValueRange))) {
        CFRange rangeInBytes;
	CFStorageNode *resultNode;
        result = (uint8_t *)__CFStorageFindByte(storage, &storage->rootNode, idx * storage->valueSize, &resultNode, &rangeInBytes);
	CFRange rangeInValues = CFRangeMake(rangeInBytes.location / storage->valueSize, rangeInBytes.length / storage->valueSize);
        __CFStorageSetCache(storage, resultNode, rangeInValues.location, rangeInValues.length);
	*validConsecutiveValueRange = rangeInValues;
    }
    return result;
}

// returns refcount==1 node under GC
static CFStorageNode *__CFStorageCreateNode(CFAllocatorRef allocator, bool isLeaf, CFIndex numBytes) {
    CFStorageNode *newNode = (CFStorageNode *)CFAllocatorAllocate(allocator, sizeof(CFStorageNode), __kCFAllocatorGCScannedMemory);
    if (__CFOASafe) __CFSetLastAllocationEventName(newNode, "CFStorage (node)");
    newNode->isLeaf = isLeaf;
    newNode->numBytes = numBytes;
    if (isLeaf) {
        newNode->info.leaf.capacityInBytes = 0;
        newNode->info.leaf.memory = NULL;
    } else {
        newNode->info.notLeaf.child[0] = newNode->info.notLeaf.child[1] = newNode->info.notLeaf.child[2] = NULL;
    }
    return newNode;
}

static void __CFStorageNodeDealloc(CFAllocatorRef allocator, CFStorageNode *node, bool freeNodeItself) {
    if (node->isLeaf) {
        _CFAllocatorDeallocateGC(allocator, node->info.leaf.memory);
    } else {
        int cnt;
        for (cnt = 0; cnt < 3; cnt++) if (node->info.notLeaf.child[cnt]) __CFStorageNodeDealloc(allocator, node->info.notLeaf.child[cnt], true);
    }
    if (freeNodeItself) _CFAllocatorDeallocateGC(allocator, node);
}

static CFIndex __CFStorageGetNumChildren(CFStorageNode *node) {
    if (!node || node->isLeaf) return 0;
    if (node->info.notLeaf.child[2]) return 3;
    if (node->info.notLeaf.child[1]) return 2;
    if (node->info.notLeaf.child[0]) return 1;
    return 0;
}

/* The boolean compact indicates whether leaf nodes that get smaller should be realloced.
*/
static void __CFStorageDelete(CFAllocatorRef allocator, CFStorageRef storage, CFStorageNode *node, CFRange range, bool compact) {
    if (node->isLeaf) {
	node->numBytes -= range.length;
        // If this node had memory allocated, readjust the bytes...
	if (node->info.leaf.memory) {
            COPYMEM(node->info.leaf.memory + range.location + range.length, node->info.leaf.memory + range.location, node->numBytes - range.location);
	    if (compact) __CFStorageAllocLeafNodeMemory(allocator, storage, node, node->numBytes, true);
	}
   } else {
        bool childrenAreLeaves = node->info.notLeaf.child[0]->isLeaf;
	node->numBytes -= range.length;
	while (range.length > 0) {
            CFRange rangeToDelete;
            CFIndex relativeByteNum;
            CFIndex childNum;
            __CFStorageFindChild(node, range.location + range.length, true, &childNum, &relativeByteNum);
            if (range.length > relativeByteNum) {
                rangeToDelete.length = relativeByteNum;
                rangeToDelete.location = 0;
            } else {
                rangeToDelete.length = range.length;
                rangeToDelete.location = relativeByteNum - range.length;
            }
            __CFStorageDelete(allocator, storage, node->info.notLeaf.child[childNum], rangeToDelete, compact);
            if (node->info.notLeaf.child[childNum]->numBytes == 0) {		// Delete empty node and compact
                int cnt;
                _CFAllocatorDeallocateGC(allocator, node->info.notLeaf.child[childNum]);
                for (cnt = childNum; cnt < 2; cnt++) {
                    __CFAssignWithWriteBarrier((void **)&node->info.notLeaf.child[cnt], node->info.notLeaf.child[cnt+1]);
                }
                node->info.notLeaf.child[2] = NULL;
            }
	    range.length -= rangeToDelete.length;
	}
        // At this point the remaining children are packed
        if (childrenAreLeaves) {
            // Children are leaves; if their total bytes is smaller than a leaf's worth, collapse into one...
            if (node->numBytes > 0 && node->numBytes <= storage->maxLeafCapacity) {
                __CFStorageAllocLeafNodeMemory(allocator, storage, node->info.notLeaf.child[0], node->numBytes, false);
                if (node->info.notLeaf.child[1] && node->info.notLeaf.child[1]->numBytes) {
                    COPYMEM(node->info.notLeaf.child[1]->info.leaf.memory, node->info.notLeaf.child[0]->info.leaf.memory + node->info.notLeaf.child[0]->numBytes, node->info.notLeaf.child[1]->numBytes);
                    if (node->info.notLeaf.child[2] && node->info.notLeaf.child[2]->numBytes) {
                        COPYMEM(node->info.notLeaf.child[2]->info.leaf.memory, node->info.notLeaf.child[0]->info.leaf.memory + node->info.notLeaf.child[0]->numBytes + node->info.notLeaf.child[1]->numBytes, node->info.notLeaf.child[2]->numBytes);
                        __CFStorageNodeDealloc(allocator, node->info.notLeaf.child[2], true);
                        node->info.notLeaf.child[2] = NULL;
                    }
                    __CFStorageNodeDealloc(allocator, node->info.notLeaf.child[1], true);
                    node->info.notLeaf.child[1] = NULL;
                }
                node->info.notLeaf.child[0]->numBytes = node->numBytes;
            }
        } else {
            // Children are not leaves; combine their children to assure each node has 2 or 3 children...
	    // (Could try to bypass all this by noting up above whether the number of grandchildren changed...)
            CFStorageNode *gChildren[9];
            CFIndex cCnt, gCnt, cnt;
            CFIndex totalG = 0;	// Total number of grandchildren
            for (cCnt = 0; cCnt < 3; cCnt++) {
                CFStorageNode *child = node->info.notLeaf.child[cCnt];
                if (child) {
		    for (gCnt = 0; gCnt < 3; gCnt++) if (child->info.notLeaf.child[gCnt]) {
                        gChildren[totalG++] = child->info.notLeaf.child[gCnt];
                        child->info.notLeaf.child[gCnt] = NULL;
                    }
		    child->numBytes = 0;
		}
            }
            gCnt = 0;	// Total number of grandchildren placed
	    for (cCnt = 0; cCnt < 3; cCnt++) {
                // These tables indicate how many children each child should have, given the total number of grandchildren (last child gets remainder)
                static const unsigned char forChild0[10] = {0, 1, 2, 3, 2, 3, 3, 3, 3, 3};
                static const unsigned char forChild1[10] = {0, 0, 0, 0, 2, 2, 3, 2, 3, 3};
		// sCnt is the number of grandchildren to be placed into child cCnt
		// Depending on child number, pick the right number
                CFIndex sCnt = (cCnt == 0) ? forChild0[totalG] : ((cCnt == 1) ? forChild1[totalG] : totalG);
		// Assure we have that many grandchildren...
		if (sCnt > totalG - gCnt) sCnt = totalG - gCnt;
                if (sCnt) {
                    if (!node->info.notLeaf.child[cCnt]) {
                        CFStorageNode *newNode = __CFStorageCreateNode(allocator, false, 0);
                        __CFAssignWithWriteBarrier((void **)&node->info.notLeaf.child[cCnt], newNode);
                        Boolean GC = CF_IS_COLLECTABLE_ALLOCATOR(allocator);
                        if (GC) auto_zone_release(auto_zone(), newNode);
                    }
                    for (cnt = 0; cnt < sCnt; cnt++) {
                        node->info.notLeaf.child[cCnt]->numBytes += gChildren[gCnt]->numBytes;
                        __CFAssignWithWriteBarrier((void **)&node->info.notLeaf.child[cCnt]->info.notLeaf.child[cnt], gChildren[gCnt++]);
                    }
                } else {
                    if (node->info.notLeaf.child[cCnt]) {
                        _CFAllocatorDeallocateGC(allocator, node->info.notLeaf.child[cCnt]);
                        node->info.notLeaf.child[cCnt] = NULL;
                    }
                }
	    }
        }
    }
}


/* Returns NULL or additional node to come after this node
   Assumption: size is never > storage->maxLeafCapacity
   Under GC node has a retain count to keep it alive in unregistered pthreads
*/
static CFStorageNode *__CFStorageInsert(CFAllocatorRef allocator, CFStorageRef storage, CFStorageNode *node, CFIndex byteNum, CFIndex size, CFIndex absoluteByteNum) {
    if (node->isLeaf) {
        if (size + node->numBytes > storage->maxLeafCapacity) {	// Need to create more child nodes
            if (byteNum == node->numBytes) {	// Inserting at end; easy...
                CFStorageNode *newNode = __CFStorageCreateNode(allocator, true, size);
                __CFStorageSetCache(storage, newNode, absoluteByteNum / storage->valueSize, size / storage->valueSize);
                return newNode;
            } else if (byteNum == 0) {	// Inserting at front; also easy, but we need to swap node and newNode
                CFStorageNode *newNode = __CFStorageCreateNode(allocator, true, 0);
                objc_memmove_collectable(newNode, node, sizeof(CFStorageNode));
                node->isLeaf = true;
                node->numBytes = size;
                node->info.leaf.capacityInBytes = 0;
                node->info.leaf.memory = NULL;
                __CFStorageSetCache(storage, node, absoluteByteNum / storage->valueSize, size / storage->valueSize);
                return newNode;
            } else if (byteNum + size <= storage->maxLeafCapacity) {	// Inserting at middle; inserted region will fit into existing child
                // Create new node to hold the overflow
                CFStorageNode *newNode = __CFStorageCreateNode(allocator, true, node->numBytes - byteNum);
                if (node->info.leaf.memory) {	// We allocate memory lazily...
                    __CFStorageAllocLeafNodeMemory(allocator, storage, newNode, node->numBytes - byteNum, false);
                    COPYMEM(node->info.leaf.memory + byteNum, newNode->info.leaf.memory, node->numBytes - byteNum);
                    __CFStorageAllocLeafNodeMemory(allocator, storage, node, byteNum + size, false);
                }
                node->numBytes = byteNum + size;
                __CFStorageSetCache(storage, node, (absoluteByteNum - byteNum) / storage->valueSize, node->numBytes / storage->valueSize);
                return newNode;
            } else {	// Inserting some of new into one node, rest into another; remember that the assumption is size <= storage->maxLeafCapacity
                CFStorageNode *newNode = __CFStorageCreateNode(allocator, true, node->numBytes + size - storage->maxLeafCapacity);	// New stuff
                if (node->info.leaf.memory) {	// We allocate memory lazily...
                    __CFStorageAllocLeafNodeMemory(allocator, storage, newNode, node->numBytes + size - storage->maxLeafCapacity, false);
                    COPYMEM(node->info.leaf.memory + byteNum, newNode->info.leaf.memory + byteNum + size - storage->maxLeafCapacity, node->numBytes - byteNum);
                    __CFStorageAllocLeafNodeMemory(allocator, storage, node, storage->maxLeafCapacity, false);
                }
                node->numBytes = storage->maxLeafCapacity;
                __CFStorageSetCache(storage, node, (absoluteByteNum - byteNum) / storage->valueSize, node->numBytes / storage->valueSize);
                return newNode;
            }
        } else {	// No need to create new nodes!
            if (node->info.leaf.memory) {
                __CFStorageAllocLeafNodeMemory(allocator, storage, node, node->numBytes + size, false);
                COPYMEM(node->info.leaf.memory + byteNum, node->info.leaf.memory + byteNum + size, node->numBytes - byteNum);
            }
            node->numBytes += size;
            __CFStorageSetCache(storage, node, (absoluteByteNum - byteNum) / storage->valueSize, node->numBytes / storage->valueSize);
            return NULL;
        }
    } else {
        CFIndex relativeByteNum;
        CFIndex childNum;
        CFStorageNode *newNode;
        __CFStorageFindChild(node, byteNum, true, &childNum, &relativeByteNum);
        newNode = __CFStorageInsert(allocator, storage, node->info.notLeaf.child[childNum], relativeByteNum, size, absoluteByteNum);
        if (newNode) {
            if (node->info.notLeaf.child[2] == NULL) {	// There's an empty slot for the new node, cool
                if (childNum == 0) __CFAssignWithWriteBarrier((void **)&node->info.notLeaf.child[2], node->info.notLeaf.child[1]);	// Make room
                __CFAssignWithWriteBarrier((void **)&node->info.notLeaf.child[childNum + 1], newNode);
                Boolean GC = CF_IS_COLLECTABLE_ALLOCATOR(allocator);
                if (GC) auto_zone_release(auto_zone(), newNode);
                node->numBytes += size;
                return NULL;
            } else {
                CFStorageNode *anotherNode = __CFStorageCreateNode(allocator, false, 0);	// Create another node
                if (childNum == 0) {	// Last two children go to new node
                    __CFAssignWithWriteBarrier((void **)&anotherNode->info.notLeaf.child[0], node->info.notLeaf.child[1]);
                    __CFAssignWithWriteBarrier((void **)&anotherNode->info.notLeaf.child[1], node->info.notLeaf.child[2]);
                    __CFAssignWithWriteBarrier((void **)&node->info.notLeaf.child[1], newNode);
                    node->info.notLeaf.child[2] = NULL;
                } else if (childNum == 1) {	// Last child goes to new node
                    __CFAssignWithWriteBarrier((void **)&anotherNode->info.notLeaf.child[0], newNode);
                    __CFAssignWithWriteBarrier((void **)&anotherNode->info.notLeaf.child[1], node->info.notLeaf.child[2]);
                    node->info.notLeaf.child[2] = NULL;
                } else {	// New node contains the new comers...
                    __CFAssignWithWriteBarrier((void **)&anotherNode->info.notLeaf.child[0], node->info.notLeaf.child[2]);
                    __CFAssignWithWriteBarrier((void **)&anotherNode->info.notLeaf.child[1], newNode);
                    node->info.notLeaf.child[2] = NULL;
                }
                if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
                    auto_zone_release(auto_zone(), newNode);
                }
                node->numBytes = node->info.notLeaf.child[0]->numBytes + node->info.notLeaf.child[1]->numBytes;
                anotherNode->numBytes = anotherNode->info.notLeaf.child[0]->numBytes + anotherNode->info.notLeaf.child[1]->numBytes;
                return anotherNode;
            }
        } else {
            node->numBytes += size;
        }
    }
    return NULL;
}

CF_INLINE CFIndex __CFStorageGetCount(CFStorageRef storage) {
    return storage->rootNode.numBytes / storage->valueSize;
}

static Boolean __CFStorageEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFStorageRef storage1 = (CFStorageRef)cf1;
    CFStorageRef storage2 = (CFStorageRef)cf2;
    CFIndex loc, count, valueSize;
    CFRange range1, range2;
    uint8_t *ptr1, *ptr2;

    count = __CFStorageGetCount(storage1);
    if (count != __CFStorageGetCount(storage2)) return false;

    valueSize = __CFStorageGetValueSize(storage1);
    if (valueSize != __CFStorageGetValueSize(storage2)) return false;

    loc = range1.location = range1.length = range2.location = range2.length = 0;
    ptr1 = ptr2 = NULL;

    while (loc < count) {
	CFIndex cntThisTime;
	if (loc >= range1.location + range1.length) ptr1 = (uint8_t *)CFStorageGetValueAtIndex(storage1, loc, &range1);
	if (loc >= range2.location + range2.length) ptr2 = (uint8_t *)CFStorageGetValueAtIndex(storage2, loc, &range2);
	cntThisTime = range1.location + range1.length;
	if (range2.location + range2.length < cntThisTime) cntThisTime = range2.location + range2.length;
	cntThisTime -= loc;
	if (memcmp(ptr1, ptr2, valueSize * cntThisTime) != 0) return false;
	ptr1 += valueSize * cntThisTime;
	ptr2 += valueSize * cntThisTime;
	loc += cntThisTime;
    }
    return true;
}

static CFHashCode __CFStorageHash(CFTypeRef cf) {
    CFStorageRef Storage = (CFStorageRef)cf;
    return __CFStorageGetCount(Storage);
}

static void __CFStorageDescribeNode(CFStorageNode *node, CFMutableStringRef str, CFIndex level) {
    int cnt;
    for (cnt = 0; cnt < level; cnt++) CFStringAppendCString(str, "  ", CFStringGetSystemEncoding());

    if (node->isLeaf) {
        CFStringAppendFormat(str, NULL, CFSTR("Leaf %d/%d\n"), node->numBytes, node->info.leaf.capacityInBytes);
    } else {
        CFStringAppendFormat(str, NULL, CFSTR("Node %d\n"), node->numBytes);
        for (cnt = 0; cnt < 3; cnt++) if (node->info.notLeaf.child[cnt]) __CFStorageDescribeNode(node->info.notLeaf.child[cnt], str, level+1);
    }
}

static CFIndex __CFStorageGetNodeCapacity(CFStorageNode *node) {
    if (!node) return 0;
    if (node->isLeaf) return node->info.leaf.capacityInBytes;
    return __CFStorageGetNodeCapacity(node->info.notLeaf.child[0]) + __CFStorageGetNodeCapacity(node->info.notLeaf.child[1]) + __CFStorageGetNodeCapacity(node->info.notLeaf.child[2]);
}

CFIndex __CFStorageGetCapacity(CFStorageRef storage) {
    return __CFStorageGetNodeCapacity(&storage->rootNode) / storage->valueSize;
}

CFIndex __CFStorageGetValueSize(CFStorageRef storage) {
    return storage->valueSize;
}

static CFStringRef __CFStorageCopyDescription(CFTypeRef cf) {
    CFStorageRef storage = (CFStorageRef)cf;
    CFMutableStringRef result;
    CFAllocatorRef allocator = CFGetAllocator(storage);
    result = CFStringCreateMutable(allocator, 0);
    CFStringAppendFormat(result, NULL, CFSTR("<CFStorage %p [%p]>[count = %u, capacity = %u]\n"), storage, allocator, __CFStorageGetCount(storage), __CFStorageGetCapacity(storage));
    __CFStorageDescribeNode(&storage->rootNode, result, 0);
    return result;
}

static void __CFStorageDeallocate(CFTypeRef cf) {
    CFStorageRef storage = (CFStorageRef)cf;
    CFAllocatorRef allocator = CFGetAllocator(storage);
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) return; // XXX_PCB GC will take care of us.
    __CFStorageNodeDealloc(allocator, &storage->rootNode, false);
}

static CFTypeID __kCFStorageTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFStorageClass = {
    _kCFRuntimeScannedObject,
    "CFStorage",
    NULL,	// init
    NULL,	// copy
    __CFStorageDeallocate,
    __CFStorageEqual,
    __CFStorageHash,
    NULL,	// 
    __CFStorageCopyDescription
};

__private_extern__ void __CFStorageInitialize(void) {
    __kCFStorageTypeID = _CFRuntimeRegisterClass(&__CFStorageClass);
}

/*** Public API ***/

CFStorageRef CFStorageCreate(CFAllocatorRef allocator, CFIndex valueSize) {
    CFStorageRef storage;
    CFIndex size = sizeof(struct __CFStorage) - sizeof(CFRuntimeBase);
    storage = (CFStorageRef)_CFRuntimeCreateInstance(allocator, __kCFStorageTypeID, size, NULL);
    if (NULL == storage) {
	return NULL;
    }
    storage->valueSize = valueSize;
    CF_SPINLOCK_INIT_FOR_STRUCTS(storage->cacheReaderMemoryAllocationLock);
    storage->cacheGenerationCount = 0;
    storage->cacheParts.locationHi = 0;
    storage->cacheParts.locationLo = 0;
#if POSSIBLE_TO_HAVE_LENGTH_MORE_THAN_HALFWORD
    storage->cacheParts.lengthHi = 0;
#endif
    storage->cacheParts.lengthLo = 0;
    storage->cacheParts.cachedNodeHi = 0;
    storage->cacheParts.cachedNodeLo = 0;
    storage->maxLeafCapacity = __CFStorageMaxLeafCapacity;
    if (valueSize && ((storage->maxLeafCapacity % valueSize) != 0)) {	
        storage->maxLeafCapacity = (storage->maxLeafCapacity / valueSize) * valueSize;	// Make it fit perfectly (3406853)
    }
    memset(&(storage->rootNode), 0, sizeof(CFStorageNode));
    storage->rootNode.isLeaf = true;
    storage->nodeHint = __kCFAllocatorGCScannedMemory;
    if (__CFOASafe) __CFSetLastAllocationEventName(storage, "CFStorage");
    return storage;    
}

CFTypeID CFStorageGetTypeID(void) {
    return __kCFStorageTypeID;
}

CFIndex CFStorageGetCount(CFStorageRef storage) {
    return __CFStorageGetCount(storage);
}

/* Returns pointer to the specified value
   index and validConsecutiveValueRange are in terms of values
*/
void *CFStorageGetValueAtIndex(CFStorageRef storage, CFIndex idx, CFRange *validConsecutiveValueRange) {
    CFRange range;
    return __CFStorageGetValueAtIndex(storage, idx, validConsecutiveValueRange ? validConsecutiveValueRange : &range);
}

/* Makes space for range.length values at location range.location
   This function deepens the tree if necessary...
*/
void CFStorageInsertValues(CFStorageRef storage, CFRange range) {
    CFIndex numBytesToInsert = range.length * storage->valueSize;
    CFIndex byteNum = range.location * storage->valueSize;
    while (numBytesToInsert > 0) {
        CFStorageNode *newNode;
        CFAllocatorRef allocator = CFGetAllocator(storage);
        CFIndex insertThisTime = numBytesToInsert;
        if (insertThisTime > storage->maxLeafCapacity) {
            insertThisTime = (storage->maxLeafCapacity / storage->valueSize) * storage->valueSize;
        }
        newNode = __CFStorageInsert(allocator, storage, &storage->rootNode, byteNum, insertThisTime, byteNum);
        if (newNode) {
            CFStorageNode *tempRootNode = __CFStorageCreateNode(allocator, false, 0);	// Will copy the (static) rootNode over to this
            objc_memmove_collectable(tempRootNode, &storage->rootNode, sizeof(CFStorageNode));
            storage->rootNode.isLeaf = false;
            __CFAssignWithWriteBarrier((void **)&storage->rootNode.info.notLeaf.child[0], tempRootNode);
            __CFAssignWithWriteBarrier((void **)&storage->rootNode.info.notLeaf.child[1], newNode);
            if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
                auto_zone_release(auto_zone(), tempRootNode);
                auto_zone_release(auto_zone(), newNode);
            }
            storage->rootNode.info.notLeaf.child[2] = NULL;
            storage->rootNode.numBytes = tempRootNode->numBytes + newNode->numBytes;
#if 1
	    // ???
	    __CFStorageSetCache(storage, NULL, 0, 0);
#else
            if (storage->cache.cachedNode == &(storage->rootNode)) __CFAssignWithWriteBarrier((void **)&storage->cache.cachedNode, tempRootNode);	// The cache should follow the node
#endif
	}
        numBytesToInsert -= insertThisTime;
        byteNum += insertThisTime;
    }
}

/* Deletes the values in the specified range
   This function gets rid of levels if necessary...
*/
void CFStorageDeleteValues(CFStorageRef storage, CFRange range) {
    CFAllocatorRef allocator = CFGetAllocator(storage);
    range.location *= storage->valueSize;
    range.length *= storage->valueSize;
    __CFStorageDelete(allocator, storage, &storage->rootNode, range, true);
    while (__CFStorageGetNumChildren(&storage->rootNode) == 1) {
        CFStorageNode *child = storage->rootNode.info.notLeaf.child[0];	// The single child
        objc_memmove_collectable(&storage->rootNode, child, sizeof(CFStorageNode));
        _CFAllocatorDeallocateGC(allocator, child);
    }
    if (__CFStorageGetNumChildren(&storage->rootNode) == 0 && !storage->rootNode.isLeaf) {
	storage->rootNode.isLeaf = true;
	storage->rootNode.info.leaf.capacityInBytes = 0;
	storage->rootNode.info.leaf.memory = NULL;
    }
    // !!! Need to update the cache
    __CFStorageSetCache(storage, NULL, 0, 0);
}

void CFStorageGetValues(CFStorageRef storage, CFRange range, void *values) {
    while (range.length > 0) {
        CFRange leafRange;
        void *storagePtr = __CFStorageGetValueAtIndex(storage, range.location, &leafRange);
        CFIndex cntThisTime = range.length;
        if (cntThisTime > leafRange.length - (range.location - leafRange.location)) cntThisTime = leafRange.length - (range.location - leafRange.location);
        COPYMEM(storagePtr, values, cntThisTime * storage->valueSize);
        values = (uint8_t *)values + (cntThisTime * storage->valueSize);
        range.location += cntThisTime;
        range.length -= cntThisTime;
    }
}

unsigned long _CFStorageFastEnumeration(CFStorageRef storage, struct __objcFastEnumerationStateEquivalent *state, void *stackbuffer, unsigned long count) {
    // without trying to understand the data structure, each time through search for block containing index
    CFRange leafRange;
    if (state->state == 0) { /* first time, get length */
        state->extra[0] = __CFStorageGetCount(storage);
    }
    if (state->state >= state->extra[0]) return 0;
    state->itemsPtr = (unsigned long *)CFStorageGetValueAtIndex(storage, state->state, &leafRange);
    state->state += leafRange.length;
    return leafRange.length;
}

void CFStorageApplyFunction(CFStorageRef storage, CFRange range, CFStorageApplierFunction applier, void *context) {
    while (0 < range.length) {
        CFRange leafRange;
        const void *storagePtr;
        CFIndex idx, cnt;
        storagePtr = CFStorageGetValueAtIndex(storage, range.location, &leafRange);
        cnt = __CFMin(range.length, leafRange.location + leafRange.length - range.location);
        for (idx = 0; idx < cnt; idx++) {
            applier(storagePtr, context);
            storagePtr = (const char *)storagePtr + storage->valueSize;
        }
        range.length -= cnt;
        range.location += cnt;
    }
}

void CFStorageReplaceValues(CFStorageRef storage, CFRange range, const void *values) {
    while (range.length > 0) {
        CFRange leafRange;
        void *storagePtr = __CFStorageGetValueAtIndex(storage, range.location, &leafRange);
        CFIndex cntThisTime = range.length;
        if (cntThisTime > leafRange.length - (range.location - leafRange.location)) cntThisTime = leafRange.length - (range.location - leafRange.location);
        COPYMEM(values, storagePtr, cntThisTime * storage->valueSize);
		values = (const uint8_t *)values + (cntThisTime * storage->valueSize);
        range.location += cntThisTime;
        range.length -= cntThisTime;
    }
}

/* Used by CFArray.c */

static void __CFStorageNodeSetUnscanned(CFStorageNode *node, auto_zone_t *zone) {
    if (node->isLeaf) {
        auto_zone_set_unscanned(zone, node->info.leaf.memory);
    } else {
        CFStorageNode **children = node->info.notLeaf.child;
        if (children[0]) __CFStorageNodeSetUnscanned(children[0], zone);
        if (children[1]) __CFStorageNodeSetUnscanned(children[1], zone);
        if (children[2]) __CFStorageNodeSetUnscanned(children[2], zone);
    }
}

__private_extern__ void _CFStorageSetWeak(CFStorageRef storage) {
    storage->nodeHint = 0;
    __CFStorageNodeSetUnscanned(&storage->rootNode, (auto_zone_t *)auto_zone());
}

#undef COPYMEM
#undef PAGE_LIMIT

