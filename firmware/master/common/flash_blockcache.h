/*
 * Thundercracker Firmware -- Confidential, not for redistribution.
 * Copyright <c> 2012 Sifteo, Inc. All rights reserved.
 */

/*
 * The second layer of the flash stack: Cached access to physical
 * flash blocks. This layer knows nothing of virtual-to-physical address
 * translation, only of retrieving and caching physical blocks.
 */

#ifndef FLASH_BLOCKCACHE_H_
#define FLASH_BLOCKCACHE_H_

#include "svmvalidator.h"
#include "systime.h"
#include "machine.h"
#include "macros.h"
#include <stdint.h>
#include <string.h>

#ifdef SIFTEO_SIMULATOR
#  define FLASHLAYER_STATS_ONLY(x)  x
#else
#  define FLASHLAYER_STATS_ONLY(x)
#endif

class FlashBlockRef;

struct FlashStats {
    unsigned blockHitSame;
    unsigned blockHitOther;
    unsigned blockMiss;
    unsigned blockTotal;
    unsigned globalRefcount;
    SysTime::Ticks timestamp;
    bool enabled;
};

extern FlashStats gFlashStats;


/**
 * A single flash block, fetched via a globally shared cache.
 * This is the general-purpose mechansim used to randomly access arbitrary
 * sized data items from flash.
 */
class FlashBlock
{
public:
    static const unsigned NUM_CACHE_BLOCKS = 16;
    static const unsigned BLOCK_SIZE = 256;     // Power of two
    static const unsigned BLOCK_MASK = BLOCK_SIZE - 1;
    static const unsigned MAX_REFCOUNT = NUM_CACHE_BLOCKS;
    static const uint32_t INVALID_ADDRESS = (uint32_t)-1;
    #define BLOCK_ALIGN __attribute__((aligned(256)))

private:
    friend class FlashBlockRef;

    uint32_t stamp;
    uint32_t address;
    uint16_t validCodeBytes;
    uint8_t refCount;

    static uint8_t mem[NUM_CACHE_BLOCKS][BLOCK_SIZE] SECTION(".blockcache");
    static FlashBlock instances[NUM_CACHE_BLOCKS];
    static uint32_t referencedBlocksMap;
    static uint32_t latestStamp;

public:
    inline unsigned id() {
        return (unsigned)(this - instances);
    }
    
    inline unsigned bit() {
        return Intrinsic::LZ(id());
    }

    inline uint32_t getAddress() {
        return address;
    }
    
    inline uint8_t *getData() {
        return &mem[id()][0];
    }

    inline bool isCodeOffsetValid(unsigned offset) {
        // Misaligned offsets are never valid
        if (offset & 3)
            return false;
        
        // Lazily validate
        if (validCodeBytes == 0)
            validCodeBytes = SvmValidator::validBytes(getData(), BLOCK_SIZE);

        return offset < validCodeBytes;
    }
    
    /**
     * Quick predicate to check a physical address. Used only in simulation.
     */
#ifdef SIFTEO_SIMULATOR
    static bool isAddrValid(uintptr_t pa) {
        uintptr_t offset = reinterpret_cast<uint8_t*>(pa) - &mem[0][0];
        return offset < sizeof mem;
    }

    static void enableStats() {
        gFlashStats.enabled = true;
    }
#endif

    static void init();
    static void preload(uint32_t blockAddr);
    static void invalidate();
    static void get(FlashBlockRef &ref, uint32_t blockAddr);

private:
    inline void incRef() {
        ASSERT(refCount <= MAX_REFCOUNT);
        ASSERT(refCount == 0 || (referencedBlocksMap & bit()));
        ASSERT(refCount != 0 || !(referencedBlocksMap & bit()));

        if (!refCount++)
            referencedBlocksMap |= bit();

        FLASHLAYER_STATS_ONLY({
            gFlashStats.globalRefcount++;
            ASSERT(gFlashStats.globalRefcount <= MAX_REFCOUNT);
        })
    }

    inline void decRef() {
        ASSERT(refCount <= MAX_REFCOUNT);
        ASSERT(refCount != 0);
        ASSERT(referencedBlocksMap & bit());

        if (!--refCount)
            referencedBlocksMap &= ~bit();

        FLASHLAYER_STATS_ONLY({
            ASSERT(gFlashStats.globalRefcount > 0);
            gFlashStats.globalRefcount--;
        })
    }
    
    static FlashBlock *lookupBlock(uint32_t blockAddr);
    static FlashBlock *recycleBlock();
    void load(uint32_t blockAddr);
};


/**
 * A reference to a single cached flash block. While the reference is held,
 * the block will be maintained in the cache. These objects can be used
 * transiently during a single memory operation, or they can be held for
 * longer periods of time.
 */
class FlashBlockRef
{
public:
    FlashBlockRef() : block(0) {}

    FlashBlockRef(FlashBlock *block) : block(block) {
        block->incRef();
    }

    FlashBlockRef(const FlashBlockRef &r) : block(&*r) {
        block->incRef();
    }
    
    inline bool isHeld() const {
        if (block) {
            ASSERT(block->refCount != 0);
            ASSERT(block->refCount <= block->MAX_REFCOUNT);
            return true;
        }
        return false;
    }

    inline void set(FlashBlock *b) {
        if (isHeld())
            block->decRef();
        block = b;
        if (b)
            b->incRef();
    }
    
    inline void release() {
        set(0);
    }

    ~FlashBlockRef() {
        release();
    }

    inline FlashBlock& operator*() const {
        return *block;
    }

    inline FlashBlock* operator->() const {
        ASSERT(isHeld());
        return block;
    }

private:
    FlashBlock *block;
};


#endif
