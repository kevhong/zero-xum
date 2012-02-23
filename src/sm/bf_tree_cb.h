#ifndef BF_TREE_CB_H
#define BF_TREE_CB_H

#include "w_defines.h"
#include "bf_idx.h"
#include "vid_t.h"
#include "latch.h"
#include "sm_s.h"
#include <string.h>

/** a swizzled pointer (page ID) has this bit ON. */
const uint32_t SWIZZLED_PID_BIT = 0x80000000;

/**
 * \Brief Control block in the new buffer pool class.
 * \ingroup SSMBUFPOOL
 * 
 * \Details
 * The design of control block had at least 2 big surgeries.
 * The first one happened in the summer of 2011, making our first version of
 * Foster B-tree based on Shore-MT. We added a few relatively-minor things in bufferpool
 * 
 * Next happened in early 2012 when we overhauled the whole bufferpool code to
 * implement page swizzling and do related surgeries. At this point, all bufferpool classes are
 * renamed and rewritten from scratch.
 * 
 * \Section PinCount (_pin_cnt)
 * The value is incremented when 1) if the page is non-swizzled and some thread fixes this page,
 * 2) when the page has been swizzled, 3) when the page's child page is brought into buffer pool.
 * Decremented on a corresponding anti-actions of them (e.g., unfix).
 * Both increments and decrements as well as reading must be done atomically.
 * 
 * The block can be evicted from bufferpool only when this value is 0. So,
 * when the block is selected as the eviction victim, the eviction thread should
 * atomically set this value to be -1 from 0. In other words, it must be atomic CAS.
 * 
 * Whenever this value is -1, everyone should ignore this block as non-existent like NULL.
 * It's similar to the "in-transit" in the old buffer pool, but this is simpler and more efficient.
 * The thread that increments this value should check it, too. Thus, the atomic increments must
 * be atomic-CAS (not just atomic-INC) because the original value might be -1 as of the action!
 * However, there are some cases you can do inc/dec just as atomic-INC/DEC.
 * 
 * Decrement is always safe to be atomic-dec because (assuming there is no bug) you should
 * be always decrementing from 1 or larger. Also, increment is sometimes safe to be atomic-inc
 * when for some reason you are sure there are at least one more pins on the block, such as
 * when you are incrementing for the case of 3) above.
 * 
 * \Section Careful-Write-Order
 * To avoid logging some physical actions such as page split, we implemented careful write ordering
 * in bufferpool. For simplicity and scalability, we restrict 2 things on careful-write-order.
 * 
 * One is obvious. We don't allow a cycle in dependency.
 * Another isn't. We don't allow one page to be dependent (written later than) to more than one pages.
 * 
 * We once had an implementation of write order dependency without the second restriction,
 * but we needed std::list in each block with pointers in both directions. Quite heavy-weight. So, while the
 * second surgery, we made it much simpler and scalable by adding the restriction.
 * 
 * Because of this restriction, we don't need a fancy data structure to maintain dependency.
 * It's just one pointer from a control block to another. And we don't have back pointers.
 * The pointer is lazily left until when the page is considered for eviction.
 * 
 * The drawback of this change is that page split/merge might have to give up using the logging optimization
 * more often, however it's anyway rare and the optimization can be opportunistic rather than mandatory.
 */
struct bf_tree_cb_t {
    /** clears all properties . */
    inline void clear () {
        ::memset(this, 0, sizeof(bf_tree_cb_t));
    }
    // control block is bulk-initialized by malloc and memset. It has to be aligned.

    /** dirty flag. use locks to update/check this value. */
    bool volatile               _dirty;         // +1  -> 1
    
    /** true if this block is actually used. same warning as above. */
    bool volatile               _used;          // +1  -> 2

    /** volume ID of the page currently pinned on this block. */
    volid_t volatile            _pid_vol;       // +2  -> 4

    /** short page ID of the page currently pinned on this block. (we don't have stnum in bufferpool) */
    shpid_t volatile            _pid_shpid;     // +4  -> 8

    /** Count of pins on this block. See class comments. */
    int32_t volatile            _pin_cnt;       // +4 -> 12

    /** ref count (for clock algorithm). approximate, so not protected by locks. */
    uint16_t                    _refbit_approximate;// +2  -> 14

    /**
     * Used to trigger LRU-update 'about' once in 100 or 1000, and so on. approximate, so not protected by locks.
     */
    uint16_t                    _counter_approximate;// +2  -> 16

    /** recovery lsn. */
    lsndata_t volatile          _rec_lsn;       // +8 -> 24

    /** Pointer to the parent page. zero for root pages. */
    bf_idx volatile             _parent;        // +4 -> 28
    
    fill32                      _fill32;        // +4 -> 32

    /** if not zero, this page must be written out after this dependency page. */
    bf_idx volatile             _dependency_idx;// +4 -> 36
    
    /**
     * used with _dependency_idx. As of registration of the dependency, the page in _dependency_idx had this pid (volid was implicitly same as myself).
     * If now it's different, the page was written out and then evicted surely at/after _dependency_lsn, so that's fine.
     */
    shpid_t volatile            _dependency_shpid;// +4 -> 40

    /**
     * used with _dependency_idx. this is the _rec_lsn of the dependent page as of the registration of the dependency.
     * So, if the _rec_lsn of the page is now strictly larger than this value, it was flushed at least once after that,
     * so the dependency is resolved.
     */
    lsndata_t volatile          _dependency_lsn;// +8 -> 48

    /** the latch to protect this page. */
    latch_t                     _latch;         // +16(?) -> 64
    
    // disabled (no implementation)
    bf_tree_cb_t();
    bf_tree_cb_t(const bf_tree_cb_t&);
    bf_tree_cb_t& operator=(const bf_tree_cb_t&);
};
#endif // BF_TREE_CB_H