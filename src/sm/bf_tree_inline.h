#ifndef BF_TREE_INLINE_H
#define BF_TREE_INLINE_H
// inline methods for bf_tree_m
// these methods are small and very frequently called, thus inlined here.

#include "sm_int_0.h"
#include "bf_tree.h"
#include "bf_tree_cb.h"
#include "bf_tree_vol.h"
#include "bf_hashtable.h"

inline bf_tree_cb_t* bf_tree_m::get_cb(const page_s *page) {
    bf_idx idx = page - _buffer;
    w_assert1(idx > 0 && idx < _block_cnt);
    return _control_blocks + idx;
}
inline page_s* bf_tree_m::get_page(const bf_tree_cb_t *cb) {
    bf_idx idx = cb - _control_blocks;
    w_assert1(idx > 0 && idx < _block_cnt);
    return _buffer + idx;
}
inline shpid_t bf_tree_m::get_root_page_id(volid_t vol, snum_t store) {
    if (_volumes[vol] == NULL) {
        return 0;
    }
    bf_idx idx = _volumes[vol]->_root_pages[store];
    if (idx == 0 || idx >= _block_cnt) {
        return 0;
    }
    return _control_blocks[idx]._pid_shpid;
}

///////////////////////////////////   Page fix/unfix BEGIN         ///////////////////////////////////  
const uint32_t SWIZZLED_LRU_UPDATE_INTERVAL = 1000;

inline w_rc_t bf_tree_m::refix_direct (page_s*& page, bf_idx idx, latch_mode_t mode, bool conditional) {
    bf_tree_cb_t &cb(_control_blocks[idx]);
    w_assert1(cb._pin_cnt > 0);
    W_DO(cb._latch.latch_acquire(mode, conditional ? sthread_t::WAIT_IMMEDIATE : sthread_t::WAIT_FOREVER));
#ifdef BP_MAINTAIN_PARNET_PTR
    ++cb._counter_approximate;
#endif // BP_MAINTAIN_PARNET_PTR
    ++cb._refbit_approximate;
    page = &(_buffer[idx]);
    return RCOK;
}

inline w_rc_t bf_tree_m::fix_nonroot (page_s*& page, page_s *parent, volid_t vol, shpid_t shpid, latch_mode_t mode, bool conditional, bool virgin_page) {
#ifdef SIMULATE_MAINMEMORYDB
    w_assert1((shpid & SWIZZLED_PID_BIT) == 0);
    bf_idx idx = shpid;
    w_assert1 (_is_active_idx(idx));
    bf_tree_cb_t &cb(_control_blocks[idx]);
    W_DO(cb._latch.latch_acquire(mode, conditional ? sthread_t::WAIT_IMMEDIATE : sthread_t::WAIT_FOREVER));
    page = &(_buffer[idx]);
    if (true) return RCOK;
#endif // SIMULATE_MAINMEMORYDB

    w_assert1(parent !=  NULL);
    if (!is_swizzling_enabled()) {
        return _fix_nonswizzled(NULL, page, vol, shpid, mode, conditional, virgin_page);
    }
    
    // the parent must be latched
    w_assert1(latch_mode(parent) == LATCH_SH || latch_mode(parent) == LATCH_EX);
    if((shpid & SWIZZLED_PID_BIT) == 0) {
        // non-swizzled page. or even worse it might not exist in bufferpool yet!
        W_DO (_fix_nonswizzled(parent, page, vol, shpid, mode, conditional, virgin_page));

        // also try to swizzle this page
        // TODO so far we swizzle all pages as soon as we load them to bufferpool
        // but, we might want to consider more advanced policy.
        if (!_bf_pause_swizzling && is_swizzled(parent) && !is_swizzled(page)
                && parent->btree_blink != shpid // don't swizzle foster child
            ) {
            slotid_t slot = find_page_id_slot (parent, shpid);
            w_assert1(slot >= -1);
            
#ifdef EX_LATCH_ON_SWIZZLING
            if (latch_mode(parent) != LATCH_EX) {
                ++_bf_swizzle_ex;
                bool upgraded = upgrade_latch_conditional(parent);
                if (!upgraded) {
                    ++_bf_swizzle_ex_fails;
                    DBGOUT2(<< "gave up swizzling for now because we failed to upgrade parent's latch");
                    return RCOK;
                }
            }
#endif //EX_LATCH_ON_SWIZZLING
            swizzle_child(parent, slot);
        }
    } else {
        w_assert1(!virgin_page); // virgin page can't be swizzled
        // the pointer is swizzled! we can bypass pinning
        bf_idx idx = shpid ^ SWIZZLED_PID_BIT;
        w_assert1 (_is_active_idx(idx));
        bf_tree_cb_t &cb(_control_blocks[idx]);
        w_assert1(cb._pin_cnt > 0);
        w_assert1(cb._pid_vol == vol);
        w_assert1(cb._pid_shpid == _buffer[idx].pid.page);
        W_DO(cb._latch.latch_acquire(mode, conditional ? sthread_t::WAIT_IMMEDIATE : sthread_t::WAIT_FOREVER));
#ifdef BP_MAINTAIN_PARNET_PTR
        ++cb._counter_approximate;
#endif // BP_MAINTAIN_PARNET_PTR
        //++cb._refbit_approximate;
        // also, doesn't have to unpin whether there happens an error or not. easy!
        page = &(_buffer[idx]);
        
#ifdef BP_MAINTAIN_PARNET_PTR
        // infrequently update LRU.
        if (cb._counter_approximate % SWIZZLED_LRU_UPDATE_INTERVAL == 0) {
            _update_swizzled_lru(idx);
        }
#endif // BP_MAINTAIN_PARNET_PTR
    }
    return RCOK;
}

inline w_rc_t bf_tree_m::fix_virgin_root (page_s*& page, volid_t vol, snum_t store, shpid_t shpid) {
    w_assert1(vol != 0);
    w_assert1(store != 0);
    w_assert1(shpid != 0);
    w_assert1((shpid & SWIZZLED_PID_BIT) == 0);
    bf_tree_vol_t *volume = _volumes[vol];
    w_assert1(volume != NULL);
    w_assert1(volume->_root_pages[store] == 0);

    bf_idx idx;
    
#ifdef SIMULATE_MAINMEMORYDB
    idx = shpid;
    bf_tree_cb_t &cb(_control_blocks[idx]);
    cb.clear();
    cb._pid_vol = vol;
    cb._pid_shpid = shpid;
    cb._pin_cnt = 1; // root page's pin count is always positive
    cb._used = true;
    if (true) return _latch_root_page(page, idx, LATCH_EX, false);
#endif // SIMULATE_MAINMEMORYDB

    // this page will be the root page of a new store.
    W_DO(_grab_free_block(idx));
    w_assert1 (idx > 0 && idx < _block_cnt);
    volume->_root_pages[store] = idx;
    _control_blocks[idx].clear();
    _control_blocks[idx]._pid_vol = vol;
    _control_blocks[idx]._pid_shpid = shpid;
    _control_blocks[idx]._pin_cnt = 1; // root page's pin count is always positive
    _control_blocks[idx]._used = true;
    _control_blocks[idx]._dirty = true;
    ++_dirty_page_count_approximate;
    _control_blocks[idx]._swizzled = true;
    bool inserted = _hashtable->insert_if_not_exists(bf_key(vol, shpid), idx); // for some type of caller (e.g., redo) we still need hashtable entry for root
    if (!inserted) {
        ERROUT (<<"failed to insert a virgin root page to hashtable. this must not have happened because there shouldn't be any race. wtf");
        return RC(smlevel_0::eINTERNAL);
    }
    return _latch_root_page(page, idx, LATCH_EX, false);
}

inline w_rc_t bf_tree_m::fix_root (page_s*& page, volid_t vol, snum_t store, latch_mode_t mode, bool conditional) {
    w_assert1(vol != 0);
    w_assert1(store != 0);
    bf_tree_vol_t *volume = _volumes[vol];
    w_assert1(volume != NULL);
    w_assert1(volume->_root_pages[store] != 0);

    // root page is always kept in the volume descriptor
    bf_idx idx = volume->_root_pages[store];
#ifdef SIMULATE_NO_SWIZZLING
    bf_idx idx_dummy = _hashtable->lookup(bf_key(vol, _control_blocks[volume->_root_pages[store]]._pid_shpid));
    w_assert1(idx == idx_dummy);
    idx = idx_dummy;
#else // SIMULATE_NO_SWIZZLING
    if (!is_swizzling_enabled()) {
        bf_idx idx_dummy = _hashtable->lookup(bf_key(vol, _control_blocks[volume->_root_pages[store]]._pid_shpid));
        w_assert1(idx == idx_dummy);
        idx = idx_dummy;
    }
#endif // SIMULATE_NO_SWIZZLING

    w_assert1 (_is_active_idx(idx));
    w_assert1(_control_blocks[idx]._pid_vol == vol);
    w_assert1(_buffer[idx].pid.store() == store);

    return _latch_root_page(page, idx, mode, conditional);
}

inline w_rc_t bf_tree_m::_latch_root_page(page_s*& page, bf_idx idx, latch_mode_t mode, bool conditional) {
    
#ifdef SIMULATE_NO_SWIZZLING
    _increment_pin_cnt_no_assumption(idx);
#else // SIMULATE_NO_SWIZZLING
    if (!is_swizzling_enabled()) {
        _increment_pin_cnt_no_assumption(idx);
    }
#endif // SIMULATE_NO_SWIZZLING

    // root page is always swizzled. thus we don't need to increase pin. just take latch.
    W_DO(_control_blocks[idx]._latch.latch_acquire(mode, conditional ? sthread_t::WAIT_IMMEDIATE : sthread_t::WAIT_FOREVER));
    // also, doesn't have to unpin whether there happens an error or not. easy!
    page = &(_buffer[idx]);

#ifdef SIMULATE_NO_SWIZZLING
    _decrement_pin_cnt_assume_positive(idx);
#else // SIMULATE_NO_SWIZZLING
    if (!is_swizzling_enabled()) {
        _decrement_pin_cnt_assume_positive(idx);
    }
#endif // SIMULATE_NO_SWIZZLING
    return RCOK;
}

inline void bf_tree_m::unfix(const page_s* p) {
    uint32_t idx = p - _buffer;
    w_assert1 (_is_active_idx(idx));
    bf_tree_cb_t &cb(_control_blocks[idx]);
    w_assert1(cb._latch.held_by_me()); 
    cb._latch.latch_release();
}

inline void bf_tree_m::set_dirty(const page_s* p) {
    uint32_t idx = p - _buffer;
    w_assert1 (_is_active_idx(idx));
    bf_tree_cb_t &cb(_control_blocks[idx]);
    if (!cb._dirty) {
        cb._dirty = true;
        ++_dirty_page_count_approximate;
    }
}
inline bool bf_tree_m::is_dirty(const page_s* p) const {
    uint32_t idx = p - _buffer;
    w_assert1 (_is_active_idx(idx));
    return _control_blocks[idx]._dirty;
}

inline latch_mode_t bf_tree_m::latch_mode(const page_s* p) {
    uint32_t idx = p - _buffer;
    w_assert1 (_is_active_idx(idx));
    return _control_blocks[idx]._latch.mode();
}

inline void bf_tree_m::downgrade_latch(const page_s* p) {
    uint32_t idx = p - _buffer;
    w_assert1 (_is_active_idx(idx));
    bf_tree_cb_t &cb(_control_blocks[idx]);
    w_assert1(cb._latch.held_by_me()); 
    cb._latch.downgrade();
}

inline bool bf_tree_m::upgrade_latch_conditional(const page_s* p) {
    uint32_t idx = p - _buffer;
    w_assert1 (_is_active_idx(idx));
    bf_tree_cb_t &cb(_control_blocks[idx]);
    w_assert1(cb._latch.held_by_me()); 
    if (cb._latch.mode() == LATCH_EX) {
        return true;
    }
    bool would_block = false;
    cb._latch.upgrade_if_not_block(would_block);
    if (!would_block) {
        w_assert1 (cb._latch.mode() == LATCH_EX);
        return true;
    } else {
        return false;
    }
}

///////////////////////////////////   Page fix/unfix END         ///////////////////////////////////  

///////////////////////////////////   LRU/Freelist BEGIN ///////////////////////////////////  
#ifdef BP_MAINTAIN_PARNET_PTR
inline bool bf_tree_m::_is_in_swizzled_lru (bf_idx idx) const {
    w_assert1 (_is_active_idx(idx));
    return SWIZZLED_LRU_NEXT(idx) != 0 || SWIZZLED_LRU_PREV(idx) != 0 || SWIZZLED_LRU_HEAD == idx;
}
#endif // BP_MAINTAIN_PARNET_PTR
inline bool bf_tree_m::is_swizzled(const page_s* page) const
{
    bf_idx idx = page - _buffer;
    w_assert1 (_is_active_idx(idx));
    return _control_blocks[idx]._swizzled;
}

///////////////////////////////////   LRU/Freelist END ///////////////////////////////////  

inline bool bf_tree_m::_is_active_idx (bf_idx idx) const {
    if (idx <= 0 || idx > _block_cnt) {
        return false;
    }
    return _control_blocks[idx]._used;
}


inline void pin_for_refix_holder::release() {
    if (_idx != 0) {
        smlevel_0::bf->unpin_for_refix(_idx);
        _idx = 0;
    }
}

#endif // BF_TREE_INLINE_H
