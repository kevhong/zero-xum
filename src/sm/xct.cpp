/*
 * (c) Copyright 2011-2014, Hewlett-Packard Development Company, LP
 */

#include "w_defines.h"

#define SM_SOURCE
#define XCT_C

#include <new>
#define SM_LEVEL 0
#include "sm_int_1.h"

#include "tls.h"

#include "lock.h"
#include <sm_int_4.h>
#include "xct_dependent.h"
#include "xct.h"
#include "lock_x.h"
#include "lock_lil.h"
#include <w_strstream.h>

#include <sm.h>
#include "tls.h"
#include "chkpt_serial.h"
#include <sstream>
#include "crash.h"
#include "chkpt.h"
#include "logrec.h"
#include "bf_tree.h"
#include "lock_raw.h"
#include "log_lsn_tracker.h"

#include "allocator.h"

const std::string xct_t::IMPL_NAME = "traditional";

#ifdef EXPLICIT_TEMPLATE
template class w_list_t<xct_t, queue_based_lock_t>;
template class w_list_i<xct_t, queue_based_lock_t>;
template class w_list_t<xct_dependent_t,queue_based_lock_t>;
template class w_list_i<xct_dependent_t,queue_based_lock_t>;
template class w_keyed_list_t<xct_t, queue_based_lock_t, tid_t>;
template class w_descend_list_t<xct_t, queue_based_lock_t, tid_t>;
template class w_list_t<stid_list_elem_t, queue_based_lock_t>;
template class w_list_i<stid_list_elem_t, queue_based_lock_t>;
template class w_auto_delete_array_t<lockid_t>;
template class w_auto_delete_array_t<stid_t>;

#endif /* __GNUG__*/

// definition of LOGTRACE is in crash.h
#define DBGX(arg) DBG(<<" th."<<me()->id << " " << "tid." << _tid  arg)

// If we run into btree shrinking activity, we'll bump up the
// fudge factor, b/c to undo a lot of btree removes (incremental
// tree removes) takes about 4X the logging...
extern double logfudge_factors[logrec_t::t_max_logrec]; // in logstub.cpp
#define UNDO_FUDGE_FACTOR(t, nbytes) int((logfudge_factors[t])*(nbytes))

#ifdef W_TRACE
extern "C" void debugflags(const char *);
void
debugflags(const char *a)
{
   _w_debug.setflags(a);
}
#endif /* W_TRACE */

SPECIALIZE_CS(xct_t, int _dummy, (_dummy=0),
            _mutex->acquire_1thread_xct_mutex(),
            _mutex->release_1thread_xct_mutex());

int auto_rollback_t::_count = 0;

/*********************************************************************
 *
 *  The xct list is sorted for easy access to the oldest and
 *  youngest transaction. All instantiated xct_t objects are
 *  in the list.
 *
 *  Here are the transaction list and the mutex that protects it.
 *
 *********************************************************************/

queue_based_lock_t        xct_t::_xlist_mutex;

w_descend_list_t<xct_t, queue_based_lock_t, tid_t>
        xct_t::_xlist(W_KEYED_ARG(xct_t, _tid,_xlink), &_xlist_mutex);

bool xct_t::xlist_mutex_is_mine()
{
     bool is =
        me()->get_xlist_mutex_node()._held
        &&
        (me()->get_xlist_mutex_node()._held->
            is_mine(&me()->get_xlist_mutex_node()));
     return is;
}
void xct_t::assert_xlist_mutex_not_mine()
{
    w_assert1(
            (me()->get_xlist_mutex_node()._held == 0)
           ||
           (me()->get_xlist_mutex_node()._held->
               is_mine(&me()->get_xlist_mutex_node())==false));
}
void xct_t::assert_xlist_mutex_is_mine()
{
#if W_DEBUG_LEVEL > 1
    bool res =
     me()->get_xlist_mutex_node()._held
        && (me()->get_xlist_mutex_node()._held->
            is_mine(&me()->get_xlist_mutex_node()));
    if(!res) {
        fprintf(stderr, "held: %p\n",
             me()->get_xlist_mutex_node()._held );
        if ( me()->get_xlist_mutex_node()._held  )
        {
        fprintf(stderr, "ismine: %d\n",
            me()->get_xlist_mutex_node()._held->
            is_mine(&me()->get_xlist_mutex_node()));
        }
        w_assert1(0);
    }
#else
     w_assert1(me()->get_xlist_mutex_node()._held
        && (me()->get_xlist_mutex_node()._held->
            is_mine(&me()->get_xlist_mutex_node())));
#endif
}

w_rc_t  xct_t::acquire_xlist_mutex()
{
     assert_xlist_mutex_not_mine();
     _xlist_mutex.acquire(&me()->get_xlist_mutex_node());
     assert_xlist_mutex_is_mine();
     return RCOK;
}

void  xct_t::release_xlist_mutex()
{
     assert_xlist_mutex_is_mine();
     _xlist_mutex.release(me()->get_xlist_mutex_node());
     assert_xlist_mutex_not_mine();
}

/*********************************************************************
 *
 *  _nxt_tid is used to generate unique transaction id
 *  _1thread_name is the name of the mutex protecting the xct_t from
 *          multi-thread access
 *
 *********************************************************************/
tid_t                                 xct_t::_nxt_tid = tid_t::null;

/*********************************************************************
 *
 *  _oldest_tid is the oldest currently-running tx (well, could be
 *  committed by now - the xct destructor updates this)
 *  This corresponds to the Shore-MT paper section 7.3, top of
 *  2nd column, page 10.
 *
 *********************************************************************/
tid_t                                xct_t::_oldest_tid = tid_t::null;

inline bool   xct_t::should_consume_rollback_resv(int t) const
{
     if(state() == xct_aborting) {
         w_assert0(_rolling_back);
     } // but not the reverse: rolling_back
     // could be true while we're active
     // _core->xct_aborted means we called abort but
     // we might be in freeing_space state right now, in
     // which case, _rolling_back isn't true.
    return
        // _rolling_back means in rollback(),
        // which can be in abort or in
        // rollback_work.
        _rolling_back || _core->_xct_aborting
        // compensate is a special case:
        // consume rollback space
        || t == logrec_t::t_compensate ;
 }

struct lock_info_ptr {
    xct_lock_info_t* _ptr;

    lock_info_ptr() : _ptr(0) { }

    xct_lock_info_t* take() {
        if(xct_lock_info_t* rval = _ptr) {
            _ptr = 0;
            return rval;
        }
        return new xct_lock_info_t;
    }
    void put(xct_lock_info_t* ptr) {
        if(_ptr)
            delete _ptr;
        _ptr = ptr? ptr->reset_for_reuse() : 0;
    }

    ~lock_info_ptr() { put(0); }
};

DECLARE_TLS(lock_info_ptr, agent_lock_info);

struct lil_lock_info_ptr {
    lil_private_table* _ptr;

    lil_lock_info_ptr() : _ptr(0) { }

    lil_private_table* take() {
        if(lil_private_table* rval = _ptr) {
            _ptr = 0;
            return rval;
        }
        return new lil_private_table;
    }
    void put(lil_private_table* ptr) {
        if(_ptr)
            delete _ptr;
        if (ptr) {
            ptr->clear();
        }
        _ptr = ptr;
    }

    ~lil_lock_info_ptr() { put(0); }
};

DECLARE_TLS(lil_lock_info_ptr, agent_lil_lock_info);

// Define customized new and delete operators for sm allocation
DEFINE_SM_ALLOC(xct_t);
DEFINE_SM_ALLOC(xct_t::xct_core);

xct_t::xct_core::xct_core(tid_t const &t, state_t s, timeout_in_ms timeout)
    :
    _tid(t),
    _timeout(timeout),
    _warn_on(true),
    _lock_info(agent_lock_info->take()),
    _lil_lock_info(agent_lil_lock_info->take()),
    _raw_lock_xct(NULL),
    _updating_operations(0),
    _threads_attached(0),
    _state(s),
    _read_only(false),
    _storesToFree(stid_list_elem_t::link_offset(), &_1thread_xct),
    _loadStores(stid_list_elem_t::link_offset(), &_1thread_xct),
    _xct_ended(0), // for assertions
    _xct_aborting(0)
{
    _lock_info->set_tid(_tid);
    w_assert1(_tid == _lock_info->tid());

    w_assert1(_lil_lock_info);
    if (smlevel_0::lm) {
        _raw_lock_xct = smlevel_0::lm->allocate_xct();
    }

    INC_TSTAT(begin_xct_cnt);

}

/*********************************************************************
 *
 *  xct_t::xct_t(that, type)
 *
 *  Begin a transaction. The transaction id is assigned automatically,
 *  and the xct record is inserted into _xlist.
 *
 *********************************************************************/
xct_t::xct_t(sm_stats_info_t* stats, timeout_in_ms timeout, bool sys_xct,
           bool single_log_sys_xct, const tid_t& given_tid, const lsn_t& last_lsn,
           const lsn_t& undo_nxt, bool loser_xct
            )
    :
    __stats(stats),
    __saved_lockid_t(0),
    __saved_xct_log_t(0),
    _tid(_core->_tid),
    _xct_chain_len(0),
    _ssx_chain_len(0),
    _query_concurrency (smlevel_0::t_cc_none),
    _query_exlock_for_select(false),
    _piggy_backed_single_log_sys_xct(false),
    _sys_xct (sys_xct),
    _single_log_sys_xct (single_log_sys_xct),
    _inquery_verify(false),
    _inquery_verify_keyorder(false),
    _inquery_verify_space(false),
    // _first_lsn, _last_lsn, _undo_nxt,
    _last_lsn(last_lsn),
    _undo_nxt(undo_nxt),
    _read_watermark(lsn_t::null),
    _elr_mode (elr_none),
    _dependent_list(W_LIST_ARG(xct_dependent_t, _link), &_core->_1thread_xct),
    _last_log(0),
    _log_buf(0),
    _log_bytes_rsvd(0),
    _log_bytes_ready(0),
    _log_bytes_used(0),
    _log_bytes_reserved_space(0),
    _rolling_back(false),
#if CHECK_NESTING_VARIABLES
#endif
    _in_compensated_op(0),
    _core(new xct_core(
                given_tid == tid_t::null ? _nxt_tid.atomic_incr() : given_tid,
                xct_active, timeout))
#if W_DEBUG_LEVEL > 2
    ,
    _had_error(false)
#endif
{
    if (given_tid != tid_t::null) {
        // tid is given if transaction is being built during restart
        _nxt_tid.atomic_assign_max(given_tid);
    }

    w_assert1(tid() == _core->_tid);
    w_assert3(tid() <= _nxt_tid);
    w_assert2(tid() <= _nxt_tid);
    w_assert1(tid() == _core->_lock_info->tid());

    if (true == loser_xct)
        _loser_xct = loser_true;  // A loser transaction
    else
        _loser_xct = loser_false; // Not a loser transaction

    _log_buf = new logrec_t; // deleted when xct goes away
    _log_buf_for_piggybacked_ssx = new logrec_t;

#ifdef ZERO_INIT
    memset(_log_buf, '\0', sizeof(logrec_t));
    memset(_log_buf_for_piggybacked_ssx, '\0', sizeof(logrec_t));
#endif

    if (!_log_buf || !_log_buf_for_piggybacked_ssx)  {
        W_FATAL(eOUTOFMEMORY);
    }

    if (timeout_c() == WAIT_SPECIFIED_BY_THREAD) {
        // override in this case
        set_timeout(me()->lock_timeout());
    }
    w_assert9(timeout_c() >= 0 || timeout_c() == WAIT_FOREVER);

    put_in_order();

    if (given_tid == tid_t::null) {
        me()->attach_xct(this);
    }
    else {
        w_assert1(me()->xct() == 0);
    }
}


xct_t::xct_core::~xct_core()
{
    w_assert3(_state == xct_ended);
    if(_lock_info) {
        agent_lock_info->put(_lock_info);
    }
    if (_lil_lock_info) {
        agent_lil_lock_info->put(_lil_lock_info);
    }
    if (_raw_lock_xct) {
        smlevel_0::lm->deallocate_xct(_raw_lock_xct);
    }
}
/*********************************************************************
 *
 *  xct_t::~xct_t()
 *
 *  Clean up and free up memory used by the transaction. The
 *  transaction has normally ended (committed or aborted)
 *  when this routine is called.
 *
 *********************************************************************/
xct_t::~xct_t()
{
    FUNC(xct_t::~xct_t);
    DBGX( << " ended: _log_bytes_rsvd " << _log_bytes_rsvd
            << " _log_bytes_ready " << _log_bytes_ready
            << " _log_bytes_used " << _log_bytes_used
            );

    w_assert9(__stats == 0);

    if (!_sys_xct && smlevel_0::log) {
        smlevel_0::log->get_oldest_lsn_tracker()->leave(
                reinterpret_cast<uintptr_t>(this));
    }
    LOGREC_ACCOUNTING_PRINT // see logrec.h

    _teardown(false);
    w_assert3(_in_compensated_op==0);

    if (shutdown_clean)  {
        // if this transaction is system transaction,
        // the thread might be still conveying another thread
        w_assert1(is_sys_xct() || me()->xct() == 0);
    }

    w_assert1(one_thread_attached());
    {
        CRITICAL_SECTION(xctstructure, *this);
        // w_assert1(is_1thread_xct_mutex_mine());

        while (_dependent_list.pop()) ;

        delete _log_buf;
        delete _log_buf_for_piggybacked_ssx;

        // clean up what's stored in the thread
        me()->no_xct(this);
    }

    if(__saved_lockid_t)  {
        delete[] __saved_lockid_t;
        __saved_lockid_t=0;
    }

    if(__saved_xct_log_t) {
        delete __saved_xct_log_t;
        __saved_xct_log_t=0;
    }

    if (LATCH_NL != latch().mode())
    {
        // Someone is accessing this txn, wait until it finished
        w_rc_t latch_rc = latch().latch_acquire(LATCH_EX, WAIT_FOREVER);

        // Now we can delete the core, no one can acquire latch on this txn after this point
        // since transaction is being destroyed
        if(_core)
            delete _core;
        _core = NULL;

        if (false == latch_rc.is_error())
        {
            if (latch().held_by_me())
                latch().latch_release();
        }
    }
}

#if W_DEBUG_LEVEL > 2
/* debugger-callable */
extern "C" void dumpXct(const xct_t *x) { if(x) { cout << *x <<endl;} }

/* help for debugger-callable dumpThreadById() below */
class PrintSmthreadById : public SmthreadFunc
{
    public:
        PrintSmthreadById(ostream& out, int i ) : o(out), _i(0) {
                _i = sthread_base_t::id_t(i);
        };
        void operator()(const smthread_t& smthread);
    private:
        ostream&        o;
        sthread_base_t::id_t                 _i;
};
void PrintSmthreadById::operator()(const smthread_t& smthread)
{
    if (smthread.id == _i)  {
        o << "--------------------" << "\n" << smthread;
    }
}

/* debugger-callable */
extern "C" void
dumpThreadById(int i) {
    PrintSmthreadById f(cout, i);
    smthread_t::for_each_smthread(f);
}
#endif

/*
 * Clean up existing transactions at ssm shutdown.
 * -- called from ~ss_m, so this should never be
 * subject to multiple threads using the xct list.
 *
 * Must abort the transactions as if they had been
 * called through ssm API to preserve assertions deep
 * in the ssm regarding update-threads.
 */
int
xct_t::cleanup(bool /*dispose_prepared*/)
{
    bool        changed_list;
    int         nprepared = 0;
    xct_t*      xd;
    W_COERCE(acquire_xlist_mutex());
    do {
        /*
         *  We cannot delete an xct while iterating. Use a loop
         *  to iterate and delete one xct for each iteration.
         */
        xct_i i(false); // do acquire the list mutex. Noone
        // else should be iterating over the xcts at this point.
        changed_list = false;
        xd = i.next();
        if (xd) {
            // Release the mutex so we can delete the xd if need be...
            release_xlist_mutex();
            switch(xd->state()) {
            case xct_active: {
                    me()->attach_xct(xd);
                    int num = xd->attach_update_thread();
                    /*
                     *  We usually want to shutdown cleanly. For debugging
                     *  purposes, it is sometimes desirable to simply quit.
                     *
                     *  NB:  if a vas has multiple threads running on behalf
                     *  of a tx at this point, it's going to run into trouble.
                     */
                    if (shutdown_clean) {
                        w_assert0(num==1);
                        W_COERCE( xd->abort() );
                    } else {
                        W_COERCE( xd->dispose() );
                    }
                    delete xd;
                    changed_list = true;
                }
                break;

            case xct_freeing_space:
            case xct_ended: {
                    DBG(<< xd->tid() <<"deleting "
                            << " w/ state=" << xd->state() );
                    delete xd;
                    changed_list = true;
                }
                break;

            default: {
                    DBG(<< xd->tid() <<"skipping "
                            << " w/ state=" << xd->state() );
                }
                break;

            } // switch on xct state
            W_COERCE(acquire_xlist_mutex());
        } // xd not null
    } while (xd && changed_list);
    release_xlist_mutex();
    return nprepared;
}




/*********************************************************************
 *
 *  xct_t::num_active_xcts()
 *
 *  Return the number of active transactions (equivalent to the
 *  size of _xlist.
 *
 *********************************************************************/
uint32_t
xct_t::num_active_xcts()
{
    uint32_t num;
    W_COERCE(acquire_xlist_mutex());
    num = _xlist.num_members();
    release_xlist_mutex();
    return  num;
}



/*********************************************************************
 *
 *  xct_t::look_up(tid)
 *
 *  Find the record for tid and return it. If not found, return 0.
 *
 *********************************************************************/
xct_t*
xct_t::look_up(const tid_t& tid)
{
    xct_t* xd;
    xct_i iter(true);

    while ((xd = iter.next())) {
        if (xd->tid() == tid) {
            return xd;
        }
    }
    return 0;
}

xct_lock_info_t*
xct_t::lock_info() const {
    return _core->_lock_info;
}

lil_private_table* xct_t::lil_lock_info() const
{
    return _core->_lil_lock_info;
}
RawXct* xct_t::raw_lock_xct() const {
    return _core->_raw_lock_xct;
}

timeout_in_ms
xct_t::timeout_c() const {
    return _core->_timeout;
}

/*********************************************************************
 *
 *  xct_t::oldest_tid()
 *
 *  Return the tid of the oldest active xct.
 *
 *********************************************************************/
tid_t
xct_t::oldest_tid()
{
    return _oldest_tid;
}


rc_t
xct_t::abort(bool save_stats_structure /* = false */)
{
    if(is_instrumented() && !save_stats_structure) {
        delete __stats;
        __stats = 0;
    }
    return _abort();
}

int
xct_t::num_threads()
{
    return _core->_threads_attached;
}

#if CHECK_NESTING_VARIABLES


int
xct_t::compensated_op_depth() const
{
    return _in_compensated_op;
}

int
check_compensated_op_nesting::compensated_op_depth(xct_t* xd, int dflt)
{
    // all bets are off if there's another thread attached to this xct.
    // return the default, which will allow the asserts to pass
    if(xd->num_threads() > 1) return dflt;
    return xd->compensated_op_depth();
}
#endif



void
xct_t::force_nonblocking()
{
//    lock_info()->set_nonblocking();
}

rc_t
xct_t::commit(bool lazy,lsn_t* plastlsn)
{
    // w_assert9(one_thread_attached());
    // removed because a checkpoint could
    // be going on right now.... see comments
    // in log_prepared and chkpt.cpp

    return _commit(t_normal | (lazy ? t_lazy : t_normal), plastlsn);
}

rc_t
xct_t::commit_as_group_member()
{
    w_assert1(me()->xct() == this);
    return _commit(t_normal|t_group);
}

/* Group commit: static; write the list of xct ids in the single loc record */
rc_t
xct_t::group_commit(const xct_t *list[], int listlen)
{
    // can we fit this list into the log record?
    if(listlen > xct_list_t::max)
        return RC(eLISTTOOLONG);

    // Log the whole bunch.
    return log_xct_end_group(list, listlen);
}

rc_t
xct_t::chain(bool lazy)
{
    w_assert9(one_thread_attached());
    return _commit(t_chain | (lazy ? t_lazy : t_chain));
}

xct_log_t*
xct_t::new_xct_log_t()
{
    xct_log_t*  l = new xct_log_t;
    if (!l) W_FATAL(eOUTOFMEMORY);
    return l;
}

/**\brief Used by smthread upon attach_xct() to avoid excess heap activity.
 *
 * \details
 * If the xct has a stashed copy of the caches, hand them over to the
 * calling smthread. If not, allocate some off the stack.
 */
void
xct_t::steal(xct_log_t*&x)
{
    /* See comments in smthread_t::new_xct() */
    w_assert1(is_1thread_xct_mutex_mine());

    if( (x = __saved_xct_log_t) ) {
        __saved_xct_log_t = 0;
    } else {
        x = new_xct_log_t(); // deleted when thread detaches or xct finishes
    }
    // Don't dup release
    // release_1thread_xct_mutex();
}

/**\brief Used by smthread upon detach_xct() to avoid excess heap activity.
 *
 * \details
 * If the xct has a stashed copy of the caches, free the caches
 * passed in, otherwise, hang onto them to hand over to the next
 * thread that attaches to this xct.
 */
void
xct_t::stash(xct_log_t*&x)
{
    /* See comments in smthread_t::new_xct() */
    w_assert1(is_1thread_xct_mutex_mine());

    if(__saved_xct_log_t) {
        DBGX(<<"stash: delete " << x);
        delete x;
    }
    else { __saved_xct_log_t = x; }
    x = 0;
    // dup acquire/release removed release_1thread_xct_mutex();
}

/**\brief Set the log state for this xct/thread pair to the value \e s.
 */
smlevel_0::switch_t
xct_t::set_log_state(switch_t s)
{
    xct_log_t *mine = me()->xct_log();

    switch_t old = (mine->xct_log_is_off()? OFF: ON);

    if(s==OFF) mine->set_xct_log_off();

    else mine->set_xct_log_on();

    return old;
}

void
xct_t::restore_log_state(switch_t s)
{
    (void) set_log_state(s);
}


tid_t
xct_t::youngest_tid()
{
    ASSERT_FITS_IN_LONGLONG(tid_t);
    return _nxt_tid;
}

void
xct_t::update_youngest_tid(const tid_t &t)
{
    _nxt_tid.atomic_assign_max(t);
}


void
xct_t::put_in_order() {
    W_COERCE(acquire_xlist_mutex());
    _xlist.put_in_order(this);
    _oldest_tid = _xlist.last()->_tid;
    release_xlist_mutex();

// TODO(Restart)... enable the checking in retail build, also generate error in retail
//                           this is to prevent missing something in retail and weird error
//                           shows up in retail build much later

// #if W_DEBUG_LEVEL > 2
    W_COERCE(acquire_xlist_mutex());
    {
        // make sure that _xlist is in order
        w_list_i<xct_t, queue_based_lock_t> i(_xlist);
        tid_t t = tid_t::null;
        xct_t* xd;
        while ((xd = i.next()))  {
            if (t >= xd->_tid)
                ERROUT(<<"put_in_order: failed to satisfy t < xd->_tid, t: " << t << ", xd->tid: " << xd->_tid);
            w_assert1(t < xd->_tid);
        }
        if (t > _nxt_tid)
            ERROUT(<<"put_in_order: failed to satisfy t <= _nxt_tid, t: " << t << ", _nxt_tid: " << _nxt_tid);
        w_assert1(t <= _nxt_tid);
    }
    release_xlist_mutex();
// #endif
}


smlevel_0::fileoff_t
xct_t::get_log_space_used() const
{
    return _log_bytes_used
    + _log_bytes_ready
    + _log_bytes_rsvd;
}

rc_t
xct_t::wait_for_log_space(fileoff_t amt) {
    rc_t rc = RCOK;
    if(log) {
        fileoff_t still_needed = amt;
        // check whether we even need to wait...
        if(log->reserve_space(still_needed)) {
            _log_bytes_reserved_space += still_needed;
            w_assert1(_log_bytes_reserved_space >= 0);
            still_needed = 0;
        }
        else {
            timeout_in_ms timeout = first_lsn().valid()? 100 : WAIT_FOREVER;
            fprintf(stderr, "%s:%d: first_lsn().valid()? %d    timeout=%d\n",
                __FILE__, __LINE__, first_lsn().valid(), timeout);
            rc = log->wait_for_space(still_needed, timeout);
            if(rc.is_error()) {
            //rc = RC(eOUTOFLOGSPACE);
            }
        }

        // update our reservation with whatever we got
        _log_bytes_ready += amt - still_needed;
    }
    return rc;
}

void
xct_t::dump(ostream &out)
{
    W_COERCE(acquire_xlist_mutex());
    out << "xct_t: "
            << _xlist.num_members() << " transactions"
        << endl;
    w_list_i<xct_t, queue_based_lock_t> i(_xlist);
    xct_t* xd;
    while ((xd = i.next()))  {
        out << "********************" << "\n";
        out << *xd << endl;
    }
    release_xlist_mutex();
}

void
xct_t::set_timeout(timeout_in_ms t)
{
    _core->_timeout = t;
}

w_rc_t
xct_log_warn_check_t::check(xct_t *& _victim)
{
    /* FRJ: TODO: use this with the new log reservation code. One idea
       would be to return eLOGSPACEWARN if this transaction (or some
       other?) has been forced nonblocking. Another would be to hook
       in with the LOG_RESERVATIONS stuff and warn if transactions are
       having to wait to acquire log space. Yet another way would be
       to hook in with the checkpoint thread and see if it feels
       stressed...
     */
    /*
     * NEH In the meantime, we do this crude check in prologues
     * to sm routines, if for no other reason than to test
     * the callbacks.  User can turn off log_warn_check at will with option:
     * -sm_log_warn
     */

    DBG(<<"generate_log_warnings " <<  me()->generate_log_warnings());

    // default is true
    // User can turn it off, too
    if (me()->generate_log_warnings() &&
            smlevel_0::log &&
            smlevel_0::log_warn_trigger > 0)
    {
        _victim = NULL;
        w_assert1(smlevel_1::log != NULL);

        // Heuristic, pretty crude:
        smlevel_0::fileoff_t left = smlevel_1::log->space_left() ;
        DBG(<<"left " << left << " trigger " << smlevel_0::log_warn_trigger
                << " log durable_lsn " << log->durable_lsn()
                << " log curr_lsn " << log->curr_lsn()
                //<< " segment_size " << log->segment_size()
                );

        if( left < smlevel_0::log_warn_trigger )
        {
            // Try to force the log first
            log->flush(log->curr_lsn());
        }
        if( left < smlevel_0::log_warn_trigger )
        {
            if(log_warn_callback) {
                xct_t *v = xct();
                // Check whether we have log warning on - to avoid
                // cascading errors.
                if(v && v->log_warn_is_on()) {
                    xct_i i(true);
                    lsn_t l = smlevel_1::log->global_min_lsn();
                    char  buf[max_devname];
                    log->make_log_name(l.file(), buf, max_devname);
                    w_rc_t rc = (*log_warn_callback)(
                        &i,   // iterator
                        v,    // victim
                        left, // space left
                        smlevel_0::log_warn_trigger, // threshold
                        buf
                    );
                    if(rc.is_error() && (rc.err_num() == eUSERABORT)) {
                        _victim = v;
                    }
                    return rc;
                }
            } else {
                return  RC(eLOGSPACEWARN);
            }
        }
    }
    return RCOK;
}



/*********************************************************************
 *
 *  Print out tid and status
 *
 *********************************************************************/
ostream&
operator<<(ostream& o, const xct_t& x)
{
    o << "tid="<< x.tid();

    o << "\n" << " state=" << x.state() << " num_threads=" << x._core->_threads_attached << "\n" << "   ";

    o << " defaultTimeout=";
    print_timeout(o, x.timeout_c());
    o << " first_lsn=" << x._first_lsn << " last_lsn=" << x._last_lsn << "\n" << "   ";

    o << " num_storesToFree=" << x._core->_storesToFree.num_members()
      << " num_loadStores=" << x._core->_loadStores.num_members() << "\n" << "   ";

    o << " in_compensated_op=" << x._in_compensated_op << " anchor=" << x._anchor;

    if(x.raw_lock_xct()) {
         x.raw_lock_xct()->dump_lockinfo(o);
    }

    return o;
}

// common code needed by _commit(t_chain) and ~xct_t()
void
xct_t::_teardown(bool is_chaining) {
    W_COERCE(acquire_xlist_mutex());

    _xlink.detach();
    if(is_chaining) {
        _tid = _core->_tid = _nxt_tid.atomic_incr();
        _core->_lock_info->set_tid(_tid); // WARNING: duplicated in
        // lock_x and in core
        _xlist.put_in_order(this);
    }

    // find the new oldest xct
    xct_t* xd = _xlist.last();
    _oldest_tid = xd ? xd->_tid : _nxt_tid;
    release_xlist_mutex();

    DBGX( << " commit: _log_bytes_rsvd " << _log_bytes_rsvd
         << " _log_bytes_ready " << _log_bytes_ready
         << " _log_bytes_used " << _log_bytes_used
         );
    if(long leftovers = _log_bytes_rsvd + _log_bytes_ready) {
        w_assert2(smlevel_0::log);
        smlevel_0::log->release_space(leftovers);
        _log_bytes_reserved_space -= leftovers;
        w_assert1(_log_bytes_reserved_space >= 0);

        DBG( <<  "At commit: rsvd " << _log_bytes_rsvd
            << " ready " << _log_bytes_ready
            << " used " << _log_bytes_used);

        DBG( << "Log space available is now " << log->space_left() );
    };
    _log_bytes_reserved_space =
    _log_bytes_rsvd = _log_bytes_ready = _log_bytes_used = 0;

}

/*********************************************************************
 *
 *  xct_t::change_state(new_state)
 *
 *  Change the status of the transaction to new_state. All
 *  dependents are informed of the change.
 *
 *********************************************************************/
void
xct_t::change_state(state_t new_state)
{
    FUNC(xct_t::change_state);
    w_assert1(one_thread_attached());

    // Acquire a write latch, the traditional read latch is used by checkpoint
    w_rc_t latch_rc = latch().latch_acquire(LATCH_EX, WAIT_FOREVER);
    if (latch_rc.is_error())
    {
        // Unable to the read acquire latch, cannot continue, raise an internal error
        DBGOUT2 (<< "Unable to acquire LATCH_EX for transaction object. tid = "
                 << tid() << ", rc = " << latch_rc);
        W_FATAL_MSG(fcINTERNAL, << "unable to write latch a transaction object to change state");
        return;
    }

    CRITICAL_SECTION(xctstructure, *this);
    w_assert1(is_1thread_xct_mutex_mine());

    w_assert2(_core->_state != new_state);
    w_assert2((new_state > _core->_state) ||
            (_core->_state == xct_chaining && new_state == xct_active));

    state_t old_state = _core->_state;
    _core->_state = new_state;
    switch(new_state) {
        case xct_aborting: _core->_xct_aborting = true; break;
        // the whole poiint of _xct_aborting is to
        // preserve it through xct_freeing space
        // rather than create two versions of xct_freeing_space, which
        // complicates restart
        case xct_freeing_space: break;
        case xct_ended: break; // arg see comments and logic in xct_t::_abort
        default: _core->_xct_aborting = false; break;
    }

    w_list_i<xct_dependent_t,queue_based_lock_t> i(_dependent_list);
    xct_dependent_t* d;
    while ((d = i.next()))  {
        d->xct_state_changed(old_state, new_state);
    }

    // Release the write latch
    latch().latch_release();

}


/**\todo Figure out how log space warnings will interact with mtxct */
void
xct_t::log_warn_disable()
{
    _core->_warn_on = true;
}

void
xct_t::log_warn_resume()
{
    _core->_warn_on = false;
}

bool
xct_t::log_warn_is_on() const
{
    return _core->_warn_on;
}

/**\todo Figure out how _updating_operations will interact with mtxct */
int
xct_t::attach_update_thread()
{
    w_assert2(_core->_updating_operations >= 0);
    int res = _core->_updating_operations++ + 1;
    me()->set_is_update_thread(true);
    return res;
}

void
xct_t::detach_update_thread()
{
    me()->set_is_update_thread(false);
    _core->_updating_operations--;
    w_assert2(_core->_updating_operations >= 0);
}

int
xct_t::update_threads() const
{
    return _core->_updating_operations;
}

/*********************************************************************
 *
 *  xct_t::add_dependent(d)
 *  xct_t::remove_dependent(d)
 *
 *  Add a dependent to the dependent list of the transaction.
 *
 *********************************************************************/
rc_t
xct_t::add_dependent(xct_dependent_t* dependent)
{
    FUNC(xct_t::add_dependent);
    CRITICAL_SECTION(xctstructure, *this);
    w_assert9(dependent->_link.member_of() == 0);

    w_assert1(is_1thread_xct_mutex_mine());
    _dependent_list.push(dependent);
    dependent->xct_state_changed(_core->_state, _core->_state);
    return RCOK;
}
rc_t
xct_t::remove_dependent(xct_dependent_t* dependent)
{
    FUNC(xct_t::remove_dependent);
    CRITICAL_SECTION(xctstructure, *this);
    w_assert9(dependent->_link.member_of() != 0);

    w_assert1(is_1thread_xct_mutex_mine());
    dependent->_link.detach(); // is protected
    return RCOK;
}

/*********************************************************************
 *
 *  xct_t::find_dependent(d)
 *
 *  Return true iff a given dependent(ptr) is in the transaction's
 *  list.   This must cleanly return false (rather than crashing)
 *  if d is a garbage pointer, so it cannot dereference d
 *
 *  **** Used by value-added servers. ****
 *
 *********************************************************************/
bool
xct_t::find_dependent(xct_dependent_t* ptr)
{
    FUNC(xct_t::find_dependent);
    xct_dependent_t        *d;
    CRITICAL_SECTION(xctstructure, *this);
    w_assert1(is_1thread_xct_mutex_mine());
    w_list_i<xct_dependent_t,queue_based_lock_t>    iter(_dependent_list);
    while((d=iter.next())) {
        if(d == ptr) {
            return true;
        }
    }
    return false;
}


/*********************************************************************
 *
 *  xct_t::commit(flags)
 *
 *  Commit the transaction. If flag t_lazy, log is not synced.
 *  If flag t_chain, a new transaction is instantiated inside
 *  this one, and inherits all its locks.
 *
 *  In *plastlsn it returns the lsn of the last log record for this
 *  xct.
 *
 *********************************************************************/
rc_t
xct_t::_commit(uint32_t flags, lsn_t* plastlsn /* default NULL*/)
{
    DBGX( << " commit: _log_bytes_rsvd " << _log_bytes_rsvd
            << " _log_bytes_ready " << _log_bytes_ready
            << " _log_bytes_used " << _log_bytes_used
            );
    // W_DO(check_one_thread_attached()); // now checked in prologue
    w_assert1(one_thread_attached());

    // "normal" means individual commit; not group commit.
    // Group commit cannot be lazy or chained.
    bool individual = ! (flags & xct_t::t_group);
    w_assert2(individual || ((flags & xct_t::t_chain) ==0));
    w_assert2(individual || ((flags & xct_t::t_lazy) ==0));

    w_assert1(_core->_state == xct_active);

    w_assert1(_core->_xct_ended++ == 0);

//    W_DO( ConvertAllLoadStoresToRegularStores() );

    change_state(flags & xct_t::t_chain ? xct_chaining : xct_committing);

    // when chaining, we inherit the read_watermark from the previous xct
    // in case the next transaction are read-only.
    lsn_t inherited_read_watermark;

    if (_last_lsn.valid() || !smlevel_1::log)  {
        /*
         *  If xct generated some log, write a synchronous
         *  Xct End Record.
         *  Do this if logging is turned off. If it's turned off,
         *  we won't have a _last_lsn, but we still have to do
         *  some work here to complete the tx; in particular, we
         *  have to destroy files...
         *
         *  Logging a commit must be serialized with logging
         *  prepares (done by chkpt).
         */

        // Does not wait for the checkpoint to finish, checkpoint is a non-blocking operation
        // chkpt_serial_m::read_acquire();

        // Have to re-check since in the meantime another thread might
        // have attached. Of course, that's always the case... we
        // can't avoid such server errors.
        W_DO(check_one_thread_attached());

        // OLD: don't allow a chkpt to occur between changing the
        // state and writing the log record,
        // since otherwise it might try to change the state
        // to the current state (which causes an assertion failure).
        // NEW: had to allow this below, because the freeing of
        // locks needs to happen after the commit log record is written.
        //
        // Note freeing the locks and log flush occur after 'log_xct_end',
        // and then change state.
        // if the logic changes here, need to visit chkpt logic which is
        // depending on the logic here when recording active transactions

        state_t old_state = _core->_state;
        change_state(xct_freeing_space);
        rc_t rc = RCOK;
        if (!is_sys_xct()) { // system transaction has nothing to free, so this log is not needed
            rc = log_xct_freeing_space();
        }

        // Does not wait for the checkpoint to finish, checkpoint is a non-blocking operation
        // chkpt_serial_m::read_release();

        if(rc.is_error()) {
            // Log insert failed.
            // restore the state.
            // Do this by hand; we'll fail the asserts if we
            // use change_state.
            _core->_state = old_state;
            return rc;
        }

        // We should always be able to insert this log
        // record, what with log reservations.
        if(individual && !is_single_log_sys_xct()) { // is commit record fused?
            W_COERCE(log_xct_end());
        }
        // now we have xct_end record though it might not be flushed yet. so,
        // let's do ELR
        W_DO(early_lock_release());

        if (!(flags & xct_t::t_lazy))  {
            _sync_logbuf();
        }
        else { // IP: If lazy, wake up the flusher but do not block
            _sync_logbuf(false, !is_sys_xct()); // if system transaction, don't even wake up flusher
        }

        // IP: Before destroying anything copy last_lsn
        if (plastlsn != NULL) *plastlsn = _last_lsn;

        change_state(xct_ended);

        // Free all locks. Do not free locks if chaining.
        if(individual && ! (flags & xct_t::t_chain) && _elr_mode != elr_sx)  {
            W_DO(commit_free_locks());
        }

        if(flags & xct_t::t_chain)  {
            // in this case the dependency is the previous xct itself, so take the commit LSN.
            inherited_read_watermark = _last_lsn;
        }
    }  else  {
        // Nothing logged; no need to write a log record.
        change_state(xct_ended);

        if(individual && !is_sys_xct() && ! (flags & xct_t::t_chain)) {
            W_DO(commit_free_locks());

            // however, to make sure the ELR for X-lock and CLV is
            // okay (ELR for S-lock is anyway okay) we need to make
            // sure this read-only xct (no-log=read-only) didn't read
            // anything not yet durable. Thus,
            if ((_elr_mode==elr_sx || _elr_mode==elr_clv) &&
                _query_concurrency != t_cc_none && _query_concurrency != t_cc_bad && _read_watermark.valid()) {
                // to avoid infinite sleep because of dirty pages changed by aborted xct,
                // we really output a log and flush it
                bool flushed = false;
                timeval start, now, result;
                ::gettimeofday(&start,NULL);
                while (true) {
                    W_DO(log->flush(_read_watermark, false, true, &flushed));
                    if (flushed) {
                        break;
                    }

                    // in some OS, usleep() has a very low accuracy.
                    // So, we check the current time rather than assuming
                    // elapsed time = ELR_READONLY_WAIT_MAX_COUNT * ELR_READONLY_WAIT_USEC.
                    ::gettimeofday(&now,NULL);
                    timersub(&now, &start, &result);
                    int elapsed = (result.tv_sec * 1000000 + result.tv_usec);
                    if (elapsed > ELR_READONLY_WAIT_MAX_COUNT * ELR_READONLY_WAIT_USEC) {
#if W_DEBUG_LEVEL>0
                        // this is NOT an error. it's fine.
                        cout << "ELR timeout happened in readonly xct's watermark check. outputting xct_end log..." << endl;
#endif // W_DEBUG_LEVEL>0
                        break; // timeout
                    }
                    ::usleep(ELR_READONLY_WAIT_USEC);
                }

                if (!flushed) {
                    // now we suspect that we might saw a bogus tag for some reason.
                    // so, let's output a real xct_end log and flush it.
                    // See jira ticket:99 "ELR for X-lock" (originally trac ticket:101).
                    // NOTE this should not be needed now that our algorithm is based
                    // on lock bucket tag, which is always exact, not too conservative.
                    // should consider removing this later, but for now keep it.
                    W_COERCE(log_xct_end());
                    _sync_logbuf();
                }
                _read_watermark = lsn_t::null;
            }
        } else {
            if(flags & xct_t::t_chain)  {
                inherited_read_watermark = _read_watermark;
            }
            // even if chaining or grouped xct, we can do ELR
            W_DO(early_lock_release());
        }
    }

    INC_TSTAT(commit_xct_cnt);

    me()->detach_xct(this);        // no transaction for this thread

    /*
     *  Xct is now committed
     */

    if (flags & xct_t::t_chain)  {
        w_assert0(!is_sys_xct()); // system transaction cannot chain (and never has to)

        w_assert1(individual==true);

        ++_xct_chain_len;
        /*
         *  Start a new xct in place
         */
        _teardown(true);
        _first_lsn = _last_lsn = _undo_nxt = lsn_t::null;
        if (inherited_read_watermark.valid()) {
            _read_watermark = inherited_read_watermark;
        }
        // we do NOT reset _read_watermark here. the last xct of the chain
        // is responsible to flush if it's read-only. (if read-write, it anyway flushes)
        _core->_xct_ended = 0;
        w_assert1(_core->_xct_aborting == false);
        _last_log = 0;

        // should already be out of compensated operation
        w_assert3( _in_compensated_op==0 );

        me()->attach_xct(this);
        INC_TSTAT(begin_xct_cnt);
        _core->_state = xct_chaining; // to allow us to change state back
        // to active: there's an assert about this where we don't
        // have context to know that it's where we're chaining.
        change_state(xct_active);
    } else {
        _xct_chain_len = 0;
    }

    return RCOK;
}

rc_t
xct_t::commit_free_locks(bool read_lock_only, lsn_t commit_lsn)
{
    // system transaction doesn't acquire locks
    if (!is_sys_xct()) {
        W_COERCE( lm->unlock_duration(read_lock_only, commit_lsn) );
    }
    return RCOK;
}

rc_t xct_t::early_lock_release() {
    if (!_sys_xct) { // system transaction anyway doesn't have locks
        switch (_elr_mode) {
            case elr_none: break;
            case elr_s:
                // release only S and U locks
                W_DO(commit_free_locks(true));
                break;
            case elr_sx:
            case elr_clv: // TODO see below
                // simply release all locks
                // update tag for safe SX-ELR with _last_lsn which should be the commit lsn
                // (we should have called log_xct_end right before this)
                W_DO(commit_free_locks(false, _last_lsn));
                break;
                // TODO Controlled Lock Violation is tentatively replaced with SX-ELR.
                // In RAW-style lock manager, reading the permitted LSN needs another barrier.
                // In reality (not concept but dirty impl), they are doing the same anyways.
                /*
            case elr_clv:
                // release no locks, but give permission to violate ours:
                lm->give_permission_to_violate(_last_lsn);
                break;
                */
            default:
                w_assert1(false); // wtf??
        }
    }
    return RCOK;
}



/*********************************************************************
 *
 *  xct_t::abort()
 *
 *  Abort the transaction by calling rollback().
 *
 *********************************************************************/
rc_t
xct_t::_abort()
{
    // If there are too many threads attached, tell the VAS and let it
    // ensure that only one does this.
    // W_DO(check_one_thread_attached()); // now done in the prologues.

    w_assert1(one_thread_attached());

    // The transaction abort function is shared by :
    // 1. Normal transaction abort, in such case the state would be in xct_active,
    //     xct_committing, or xct_freeing_space, and the _loser_xct flag off
    // 2. UNDO phase in Recovery, in such case the state would be in xct_active
    //     but the _loser_xct flag is on to indicating a loser transaction
    // Note that if we open the store for new transaction during Recovery
    // we could encounter normal transaction abort while Recovery is going on,
    // in such case the aborting transaction state would fall into case #1 above

    if ((false == in_recovery()) && (false == is_loser_xct()))
    {
        // Not a loser txn
        w_assert1(_core->_state == xct_active
                || _core->_state == xct_committing /* if it got an error in commit*/
                || _core->_state == xct_freeing_space /* if it got an error in commit*/
                );
        if(_core->_state != xct_committing && _core->_state != xct_freeing_space) {
            w_assert1(_core->_xct_ended++ == 0);
        }
    }

    if (true == is_loser_xct())
    {
        // Loser transaction rolling back, set the flag to indicate the status
        set_loser_xct_in_undo();
    }

#if X_LOG_COMMENT_ON
    // Do this BEFORE changing state so that we
    // have, for log-space-reservations purposes,
    // ensured that we inserted a record during
    // forward processing, thereby reserving something
    // for aborting, even if this is a read-only xct.
    {
        w_ostrstream s;
        s << "aborting... ";
        // TODO, bug... commenting this out, it appears
        // Single Page Recovery when collecting log records
        // it might have bugs dealing with this log record (before txn aborting)
//        W_DO(log_comment(s.c_str()));
    }
#endif

    change_state(xct_aborting);

    /*
     * clear the list of load stores as they are going to be destroyed
     */
    //ClearAllLoadStores();

    W_DO( rollback(lsn_t::null) );

    // if this is not part of chain or both-SX-ELR mode,
    // we can safely release all locks at this point.
    bool all_lock_released = false;
    if (_xct_chain_len == 0 || _elr_mode == elr_sx) {
        W_COERCE( commit_free_locks());
        all_lock_released = true;
    } else {
        // if it's a part of chain, we have to make preceding
        // xcts durable. so, unless it's SX-ELR, we can release only S-locks
        W_COERCE( commit_free_locks(true));
    }

    if (_last_lsn.valid()) {
        LOGREC_ACCOUNT_END_XCT(true); // see logrec.h
        /*
         *  If xct generated some log, write a Xct End Record.
         *  We flush because if this was a prepared
         *  transaction, it really must be synchronous
         */

        // don't allow a chkpt to occur between changing the state and writing
        // the log record, since otherwise it might try to change the state
        // to the current state (which causes an assertion failure).

        // NOTE: you cannot insert a log comment here; it'll break
        // on an assertion having to do with the xct state. Wait until
        // state is changed from aborting to something else.

        // Does not wait for the checkpoint to finish, checkpoint is a non-blocking operation
        // chkpt_serial_m::read_acquire();

        change_state(xct_freeing_space);
        rc_t rc = log_xct_freeing_space();

        // Does not wait for the checkpoint to finish, checkpoint is a non-blocking operation
        // chkpt_serial_m::read_release();

        W_DO(rc);

        if (_xct_chain_len > 0) {
            // we need to flush only if it's chained or prepared xct
            _sync_logbuf();
        } else {
            // otherwise, we don't have to flush
            _sync_logbuf(false);
        }

        // don't allow a chkpt to occur between changing the state and writing
        // the log record, since otherwise it might try to change the state
        // to the current state (which causes an assertion failure).

        // Does not wait for the checkpoint to finish, checkpoint is a non-blocking operation
        // chkpt_serial_m::read_acquire();

        // Log transaction abort for both cases: 1) normal abort, 2) UNDO
        change_state(xct_ended);
        rc =  log_xct_abort();

        // Does not wait for the checkpoint to finish, checkpoint is a non-blocking operation
        // chkpt_serial_m::read_release();

        W_DO(rc);
    }  else  {
        change_state(xct_ended);
    }

    if (!all_lock_released) {
        W_COERCE( commit_free_locks());
    }

    _core->_xct_aborting = false; // couldn't have xct_ended do this, arg
    _xct_chain_len = 0;

    me()->detach_xct(this);        // no transaction for this thread
    INC_TSTAT(abort_xct_cnt);
    return RCOK;
}

/*********************************************************************
 *
 *  xct_t::save_point(lsn)
 *
 *  Generate and return a save point in "lsn".
 *
 *********************************************************************/
rc_t
xct_t::save_point(lsn_t& lsn)
{
    // cannot do this with >1 thread attached
    // W_DO(check_one_thread_attached()); // now checked in prologue
    w_assert1(one_thread_attached());

    lsn = _last_lsn;
    return RCOK;
}


/*********************************************************************
 *
 *  xct_t::dispose()
 *
 *  Make the transaction disappear.
 *  This is only for simulating crashes.  It violates
 *  all tx semantics.
 *
 *********************************************************************/
rc_t
xct_t::dispose()
{
    delete __stats;
    __stats = 0;

    W_DO(check_one_thread_attached());
    W_COERCE( commit_free_locks());
    ClearAllStoresToFree();
    ClearAllLoadStores();
    _core->_state = xct_ended; // unclean!
    me()->detach_xct(this);
    return RCOK;
}

/*********************************************************************
 *
 *  xct_t::_flush_logbuf()
 *
 *  Write the log record buffered and update lsn pointers.
 *
 *********************************************************************/
w_rc_t
xct_t::_flush_logbuf()
{
    DBGX( << " _flush_logbuf: _log_bytes_rsvd " << _log_bytes_rsvd
            << " _log_bytes_ready " << _log_bytes_ready
            << " _log_bytes_used " << _log_bytes_used);
    // ASSUMES ALREADY PROTECTED BY MUTEX

    if (_last_log)  {

        DBGX ( << " xct_t::_flush_logbuf " << _last_lsn
                << " _last_log rec type is " << _last_log->type());
        // Fill in the _xid_prev field of the log rec if this record hasn't
        // already been compensated.
        if (!_last_log->is_single_sys_xct()) { // single-log sys xct doesn't have xid/xid_prev
            _last_log->fill_xct_attr(tid(), _last_lsn);
        }

        //
        // debugging prints a * if this record was written
        // during rollback
        //
        DBGX( << " "
                << ((char *)(state()==xct_aborting)?"RB":"FW")
                << " approx lsn:" << log->curr_lsn()
                << " rec:" << *_last_log
                << " size:" << _last_log->length()
                << " xid_prevlsn:" << (_last_log->is_single_sys_xct() ? lsn_t::null : _last_log->xid_prev() )
                );

        if(log) {
            logrec_t* l = _last_log;
            bool      consuming = should_consume_rollback_resv(l->type());
            _last_log = 0;
            W_DO(log->insert(*l, &_last_lsn));

            LOGTRACE( << setiosflags(ios::right) << _last_lsn
                      << resetiosflags(ios::right) << " I: " << *l
                      );

            LOGREC_ACCOUNT(*l, !consuming); // see logrec.h

            /* LOG_RESERVATIONS

               Now that we know the size of the log record which was
               generated, charge the bytes to the appropriate location.

               Normal log inserts consume /length/ available bytes and
               reserve an additional /length/ bytes against future
               rollbacks; undo records consume /length/ previously
               reserved bytes and leave available bytes unchanged..

               NOTE: we only track reservations during forward
               processing. The SM can no longer run out of log space,
               so during recovery we can assume that the log was not
               wedged at the time of the crash and will not become so
               during recovery (because redo generates only log
               compensations and undo was already accounted for)
            */
            if(smlevel_0::operating_mode == t_forward_processing) {
                long bytes_used = l->length();
                if(consuming)
                {
                    ADD_TSTAT(log_bytes_generated_rb,bytes_used);
                    DBG(<<"_log_bytes_rsvd " << _log_bytes_rsvd
                            << "(about to subtract bytes_used " << bytes_used
                            << ") _log_bytes_used " << _log_bytes_used
                            );

                    // NOTE: this assert can fail when we are rolling
                    // back a btree activity and find that we need to
                    // perform a compensating SMO
                    // that wasn't done by this xct.
                    // So I've added the OR consuming here, and
                    // and for safety, don't let the _log_bytes_rsvd
                    // go negative.
                    w_assert1((_log_bytes_rsvd >= bytes_used) || consuming);

                    _log_bytes_rsvd -= bytes_used;
                    if(consuming && _log_bytes_rsvd < 0) {
                        _log_bytes_rsvd = 0;
                    }

                    if(_log_bytes_rsvd < 0) {
                        // print some useful info
                        w_ostrstream out;
                        out
                        << " _log_bytes_rsvd " << _log_bytes_rsvd
                        << " bytes_used " << bytes_used
                        << " _rolling_back " << _rolling_back
                        << " consuming " << consuming
                        << "\n"
                        << " xct state " << state()
                        << " fudge factor is "
                        << UNDO_FUDGE_FACTOR(l->type(),1)
                        ;

                        fprintf(stderr, "%s\n", out.c_str());
                    }
                    w_assert0(_log_bytes_rsvd >= 0);
                    // This is what happens when we didn't
                    // reserve enough space for the rollback
                    // because our fudge factor was insufficient.
                    // It's a lousy heuristic.
                    // Arg.
                }
                else {
                    long to_reserve = UNDO_FUDGE_FACTOR(l->type(), bytes_used);
                    w_assert0(_log_bytes_ready >= bytes_used + to_reserve);
                    DBG(<<"_log_bytes_ready " << _log_bytes_ready
                            << "(about to subtract bytes_used "
                            << bytes_used << " + to_reserve(undo fudge) " << to_reserve
                            << ") "
                            );
                    _log_bytes_ready -= bytes_used + to_reserve;
                    DBG(<<"_log_bytes_rsvd " << _log_bytes_rsvd
                            << "(about to subtract to_reserve " << to_reserve
                            << ") "
                            );
                    _log_bytes_rsvd += to_reserve;
                }
                _log_bytes_used += bytes_used;
            }

            // log insert effectively set_lsn to the lsn of the *next* byte of
            // the log.
            if ( ! _first_lsn.valid())  _first_lsn = _last_lsn;

            if (!l->is_single_sys_xct()) {
                _undo_nxt = ( l->is_undoable_clr() ? _last_lsn :
                           l->is_cpsn() ? l->undo_nxt() : _last_lsn);
            }
        } // log non-null
    }

    return RCOK;
}

/*********************************************************************
 *
 *  xct_t::_sync_logbuf()
 *
 *  Force log entries up to the most recently written to disk.
 *
 *  block: If not set it does not block, but kicks the flusher. The
 *         default is to block, the no block option is used by AsynchCommit
 * signal: Whether we even fire the log buffer
 *********************************************************************/
w_rc_t
xct_t::_sync_logbuf(bool block, bool signal)
{
    if(log) {
        INC_TSTAT(xct_log_flush);
        return log->flush(_last_lsn,block,signal);
    }
    return RCOK;
}

rc_t
xct_t::get_logbuf(logrec_t*& ret, int t)
{
    // then , use tentative log buffer.
    if (is_piggy_backed_single_log_sys_xct()) {
        ret = _log_buf_for_piggybacked_ssx;
        return RCOK;
    }

    // protect the log buf that we'll return
#if W_DEBUG_LEVEL > 0
    fileoff_t rsvd = _log_bytes_rsvd;
    fileoff_t ready = _log_bytes_ready;
    fileoff_t used = _log_bytes_used;
    fileoff_t requested = _log_bytes_reserved_space;
    w_assert1(_log_bytes_reserved_space >= 0);
#endif

    ret = 0;

    INC_TSTAT(get_logbuf);

    // Instead of flushing here, we'll flush at the end of give_logbuf()
    // and assert here that we've got nothing buffered:
    w_assert1(!_last_log);

    if(smlevel_0::operating_mode == t_forward_processing) {
    /* LOG_RESERVATIONS
    // logrec_t is 3 pages, even though the real record is shorter.

       The log keeps its idea what space is available and as long
       as every xct calls log->reserve_space(amt) before inserting, to
       make sure there is adequate space for the insertion, we're ok.
       The xct, for its part, "carves" up the amt and puts in in one
       of two "pools": ready space -- for log insertions prior to
       rollback, and rsvd space -- for rolling back if needed.
       When the xct ends, it gives back to the log that which it
       no longer needs.

       Log reservations are not done in recovery with the following
       two exceptions: prepared xcts, and checkpoints.
       The log constructor initializes its space available; checkpoint
       and prepared transactions may reserve some of that during redo
       and end-of-recovery checkpoint; then the sm "activates " reservations,
       meaning simply that the log then figures out how much space it
       has left, and then we go into forward_processing mode. From then
       on, the xcts have the responsibility for making log reservations.

       -----------------

       Make sure we have enough space, both to continue now and to
       guarantee the ability to roll back should something go wrong
       later. This means we need to reserve double space for each log
       record inserted (one for now, one for the potential undo).

       Unfortunately, we don't actually know the size of the log
       record to be inserted, so we have to be conservative and assume
       maximum size. Similarly, we don't know whether we'll eventually
       abort. We'll deal with the former by adjusting our reservation
       in _flush_logbuf, where we do know the log record's size; we deal
       with the undo reservation at commit time, releasing it all en
       masse.

       NOTE: during rollback we don't check reservations because undo
       was already paid-for when the original record was inserted.

       NOTE: we require three logrec_t worth of reservation: one each
       for forward and undo of the log record we're about to insert,
       and the third to handle asymmetric log records required to end
       the transaction, such as the commit/abort record and any
       top-level actions generated unexpectedly during rollback.
     */

    static u_int const MIN_BYTES_READY = 2*sizeof(logrec_t) +
                                         UNDO_FUDGE_FACTOR(t,sizeof(logrec_t));
    static u_int const MIN_BYTES_RSVD =  sizeof(logrec_t);
    DBGX(<<" get_logbuf: START reserved for rollback " << _log_bytes_reserved_space
            << "\n"
            << " need ready " << MIN_BYTES_READY
            << " have " << _log_bytes_ready
            );
    bool reserving = should_reserve_for_rollback(t);
    if(reserving
       && _log_bytes_ready < MIN_BYTES_READY) {
        fileoff_t needed = MIN_BYTES_READY;
        DBGX(<<" try to reserve " << needed);
        if(!log->reserve_space(needed)) {
            DBGX(<<" no luck: my first_lsn " << _first_lsn
                    << " global min " << log->global_min_lsn()
                    );
            /*
               Yikes! Log full!

               In order to reclaim space the oldest log partition must
               have no dirty pages or active transactions associated
               with it. If we're one of those overly old transactions
               we have no choice but to abort.
             */
            INC_TSTAT(log_full);
            bool badnews=false;
            if(_first_lsn.valid() && _first_lsn.hi() ==
                    log->global_min_lsn().hi()) {
                INC_TSTAT(log_full_old_xct);
                badnews = true;
            }

            DBGX(<<" no luck: forcing my dirty pages "
                    << " badnews " << badnews);
            if(!badnews) {
                /* Now it's safe to wait for more log space to open up
                   But before we do anything, let's try to grab the
                   chkpt serial mutex so ongoing checkpoint has a
                   chance to complete before we go crazy.
                */

                // Does not wait for the checkpoint to finish, checkpoint is a non-blocking operation
                // chkpt_serial_m::read_acquire(); // wait for chkpt to finish
                // chkpt_serial_m::read_release();

                static queue_based_block_lock_t emergency_log_flush_mutex;
                CRITICAL_SECTION(cs, emergency_log_flush_mutex);
                for(int tries_left=3; tries_left > 0; tries_left--) {
                    DBGX(<<" wait for more log space tries_left " << tries_left);
                    if(tries_left == 1) {
                    // the checkpoint should also do this, but just in case...
                    lsn_t target = log_m::first_lsn(log->global_min_lsn().hi()+1);
                    w_rc_t rc = bf->force_until_lsn(target);
                    // did the force succeed?
                    if(rc.is_error()) {
                        INC_TSTAT(log_full_giveup);
                        fprintf(stderr, "Log recovery failed\n");
#if W_DEBUG_LEVEL > 0
                        extern void dump_all_sm_stats();
                        dump_all_sm_stats();
#endif
                        if(rc.err_num() == eBPFORCEFAILED)
                        return RC(eOUTOFLOGSPACE);
                        return rc;
                    }
                    }

                    // most likely it's well aware, but just in case...
                    DBGOUT3(<< "chkpt 4");

                    chkpt->wakeup_and_take();
                    W_IGNORE(log->wait_for_space(needed, 100));
                    if(!needed) {
                        if(tries_left > 1) {
                            INC_TSTAT(log_full_wait);
                        }
                        else {
                            INC_TSTAT(log_full_force);
                        }
                        goto success;
                    }

                    // try again...
                }

                if(!log->reserve_space(needed)) {
                    // won't do any good now...
                    log->release_space(MIN_BYTES_READY - needed);
                    _log_bytes_reserved_space -= (MIN_BYTES_READY - needed);
                    w_assert1(_log_bytes_reserved_space >= 0);

                    // nothing's working... give up and abort
                    stringstream tmp;
                    tmp << "Log too full. me=" << me()->id
                    << " pthread=" << pthread_self()
                    << ": min_chkpt_rec_lsn=" << log->min_chkpt_rec_lsn()
                    << ", curr_lsn=" << log->curr_lsn()
                    // also print info that shows why we didn't croak
                    // on the first check
                    << "; space left " << log->space_left()
                    << "\n";
                    fprintf(stderr, "%s\n", tmp.str().c_str());
                    INC_TSTAT(log_full_giveup);
                    badnews = true;
                }
            }

            if(badnews) {

#if W_DEBUG_LEVEL > 1
                // Dump relevant information
                stringstream tmp;
                tmp << "Log too full. " << __LINE__ << " " << __FILE__
                << "\n";
                tmp << "Thread: me=" << me()->id
                << " pthread=" << pthread_self()
                << "\n";
                tmp
                << " xct() " << tid()
                << " _rolling_back " << _rolling_back
                << " _core->_xct_aborting " << _core->_xct_aborting
                << " _state " << _core->_state
                << " _log_bytes_ready " << _log_bytes_ready
                << " log bytes needed " << needed
                << "\n";

                tmp
                << " _log_bytes_used for fwd " << _log_bytes_used
                << " _log_bytes_rsvd for rollback " << _log_bytes_rsvd
                << " rollback/used= " << double(_log_bytes_rsvd)/
                double(_log_bytes_used)
                << " fudge factor is " << UNDO_FUDGE_FACTOR(t, 1)
                << "\n";

                tmp
                    << " first_lsn="  << _first_lsn
                << "\n";
                tmp
                << "Log: min_chkpt_rec_lsn=" << log->min_chkpt_rec_lsn()
                << ", curr_lsn=" << log->curr_lsn() << "\n";

                tmp
                << "; master_lsn " << log->master_lsn()
                << "; durable_lsn " << log->durable_lsn()
                << "; curr_lsn " << log->curr_lsn()
                << ", global_min_lsn=" << log->global_min_lsn()
                << "\n";

                tmp
                // also print info that shows why we didn't croak
                // on the first check
                << "; space left " << log->space_left()
                << "; rsvd for checkpoint " << log->space_for_chkpt() 
                //<< "; max checkpoint size " << log->max_chkpt_size() 
                << "\n";

                tmp
                << " MIN_BYTES_READY " << MIN_BYTES_READY
                << " MIN_BYTES_RSVD " << MIN_BYTES_RSVD
                << "\n";

                if(errlog) {
                    errlog->clog << error_prio
                    << tmp.str().c_str()
                    << flushl;
                } else {
                    fprintf(stderr, "%s\n", tmp.str().c_str());
                }
#endif

                return RC(eOUTOFLOGSPACE);
            }
        }
    success:
        _log_bytes_reserved_space += needed;
        w_assert1(_log_bytes_reserved_space >= 0);
        _log_bytes_ready += MIN_BYTES_READY;
        DBGX( << " get_logbuf: now "
            << " _log_bytes_ready " << _log_bytes_ready
            << " _log_bytes_used " << _log_bytes_used
            << " _log_bytes_reserved_space (for rollback)"
            << _log_bytes_reserved_space
            );
    }

    /* Transactions must make some log entries as they complete
       (commit or abort), so we have to have always some bytes
       reserved. This is the third of those three logrec_t in
       MIN_BYTES_READY, so we don't have to reserve more ready bytes
       just because of this.
     */
    DBGX(<< " get_logbuf: NEAR END "
                << " _log_bytes_rsvd " << _log_bytes_rsvd
                << " MIN_BYTES_RSVD " << MIN_BYTES_RSVD
                );
    if(reserving
            && _log_bytes_rsvd < MIN_BYTES_RSVD) {
        // transfer bytes from the ready pool to the reserved pool
        _log_bytes_ready -= MIN_BYTES_RSVD;
        _log_bytes_rsvd += MIN_BYTES_RSVD;
        DBGX(<< " get_logbuf: NEAR END transferred from ready to rsvd "
                << MIN_BYTES_RSVD
                << " bytes because _log_bytes_rsvd < MIN_BYTES_RSVD "
                );
    }

    DBGX( << " get_logbuf: END _log_bytes_rsvd " << _log_bytes_rsvd
            << " _log_bytes_ready " << _log_bytes_ready
            << " _log_bytes_used " << _log_bytes_used
            );
    }
    ret = _last_log = _log_buf;


#if W_DEBUG_LEVEL > 0
    DBG(
            << " state() " << state()
            << " _rolling_back " << _rolling_back
            << " _core->_xct_aborting " << _core->_xct_aborting
            << " should_reserve_for_rollback( " << t
            << ") = " << should_reserve_for_rollback(t)
            << "\n"
            << " _log_bytes_rsvd " << _log_bytes_rsvd
                << " orig rsvd " << rsvd
            << "\n"
                << " _log_bytes_used " << _log_bytes_used
                << " orig used " << used
            << "\n"
                << " _log_bytes_ready " << _log_bytes_ready
                << " orig ready " << ready
                << " _log_bytes_reserved_space " << _log_bytes_reserved_space
                << " orig reserved " << requested
                );
    w_assert1(_log_bytes_reserved_space >= 0);
    w_assert1(_log_bytes_ready <= _log_bytes_reserved_space);
    w_assert1(_log_bytes_rsvd <= _log_bytes_reserved_space);
    w_assert1((_log_bytes_used <= _log_bytes_reserved_space) ||
            should_consume_rollback_resv(t)
            );

    if(smlevel_0::operating_mode == t_forward_processing) {
    if(should_reserve_for_rollback(t))
    {
        // Must be reserving for rollback
        w_assert1(_log_bytes_rsvd >= rsvd); // monotonically increasing here
        w_assert1(_log_bytes_used == used); // consumed in _flush_logbuf
        w_assert1(_log_bytes_reserved_space >= requested); //monotonically incr
    }
    else
    {
        // Must be consuming rollback space
        w_assert1(_log_bytes_rsvd == rsvd); // strictly decreasing, in
                                            // _flush_logbuf
        w_assert1(_log_bytes_used == used); // increasing,  in _flush_logbuf
        w_assert1(_log_bytes_reserved_space >= requested); //monotonically incr
    }
    }
    else
    {
        w_assert1(_log_bytes_reserved_space == requested);
        w_assert1(_log_bytes_rsvd == rsvd);
        w_assert1(_log_bytes_used == used);
        w_assert1(_log_bytes_ready == ready);
    }
#endif

    return RCOK;
}

void _update_page_lsns(const fixable_page_h *page, const lsn_t &new_lsn) {
    if (page != NULL) {
        if (page->latch_mode() == LATCH_EX) {
            const_cast<fixable_page_h*>(page)->update_initial_and_last_lsn(new_lsn);
            const_cast<fixable_page_h*>(page)->set_dirty();
        } else {
            // In some log type (so far only log_page_evict), we might update LSN only with
            // SH latch. In that case, we might have a race to update the LSN.
            // We should leave a larger value of LSN in that case.
            DBGOUT3(<<"Update LSN without EX latch. Atomic CAS to deal with races");
            const lsndata_t new_lsn_data = new_lsn.data();
            lsndata_t *addr = reinterpret_cast<lsndata_t*>(&page->get_generic_page()->lsn);
            lsndata_t cas_tmp = *addr;
            while (!lintel::unsafe::atomic_compare_exchange_strong<lsndata_t>(
                addr, &cas_tmp, new_lsn_data)) {
                if (lsn_t(cas_tmp) > new_lsn) {
                    DBGOUT1(<<"Someone else has already set a larger LSN. ");
                    break;
                }
            }
            w_assert1(page->lsn() >= new_lsn);
        }
        const_cast<fixable_page_h*>(page)->set_dirty();
    }
}

rc_t
xct_t::give_logbuf(logrec_t* l, const fixable_page_h *page, const fixable_page_h *page2)
{
    FUNC(xct_t::give_logbuf);
    // set page LSN chain
    if (page != NULL) {
        l->set_page_prev_lsn(page->lsn());
        if (page2 != NULL) {
            // For multi-page log, also set LSN chain with a branch.
            w_assert1(l->is_multi_page());
            w_assert1(l->is_single_sys_xct());
            multi_page_log_t *multi = l->data_ssx_multi();
            w_assert1(multi->_page2_pid != 0);
            multi->_page2_prv = page2->lsn();
        }
    }
    // If it's a log for piggy-backed SSX, we call log->insert without updating _last_log
    // because this is a single log independent from other logs in outer transaction.
    if (is_piggy_backed_single_log_sys_xct()) {
        w_assert1(l->is_single_sys_xct());
        w_assert1(l == _log_buf_for_piggybacked_ssx);
        lsn_t lsn;
        W_DO( log->insert(*l, &lsn) );
        // Mark dirty flags for both pages
        _update_page_lsns(page, lsn);
        _update_page_lsns(page2, lsn);
        DBGOUT3(<< " SSX logged: " << l->type() << "\n new_lsn= " << lsn);
        return RCOK;
    }

#if W_DEBUG_LEVEL > 0
    fileoff_t rsvd = _log_bytes_rsvd;
    fileoff_t ready = _log_bytes_ready;
    fileoff_t used = _log_bytes_used;
    fileoff_t requested = _log_bytes_reserved_space;
    w_assert1(_log_bytes_reserved_space >= 0);
#endif
    DBGX(<<"_last_log contains: "   << *l );

    // ALREADY PROTECTED from get_logbuf() call

    w_assert1(l == _last_log);

    rc_t rc = _flush_logbuf();
                      // stuffs tid, _last_lsn into our record,
                      // then inserts it into the log, getting _last_lsn
    if(!rc.is_error()) {
        _update_page_lsns(page, _last_lsn);
        _update_page_lsns(page2, _last_lsn);
    }

#if W_DEBUG_LEVEL > 0
    DBGOUT4(
            << " state() " << state()
            << " _rolling_back " << _rolling_back
            << " _core->_xct_aborting " << _core->_xct_aborting
            << " should_reserve_for_rollback( " << l->type()
            << ") = " << should_reserve_for_rollback(l->type())
            << "\n"
            << " _log_bytes_rsvd " << _log_bytes_rsvd
                << " orig rsvd " << rsvd
            << "\n"
                << " _log_bytes_used " << _log_bytes_used
                << " orig used " << used
            << "\n"
                << " _log_bytes_ready " << _log_bytes_ready
                << " orig ready " << ready
                << " _log_bytes_reserved_space " << _log_bytes_reserved_space
                << " orig reserved " << requested
                );
    if(smlevel_0::operating_mode == t_forward_processing) {
        if(should_reserve_for_rollback(l->type()))
        {
            // Must be reserving for rollback
            w_assert1(_log_bytes_rsvd >= rsvd); // consumed in _flush_logbuf
            w_assert1(_log_bytes_used > used); // consumed in _flush_logbuf
            // but the above 2 numbers are increased by the actual
            // log bytes used or a fudge function of that size.
            w_assert1(_log_bytes_reserved_space >= requested); //monotonically incr
        }
        else
        {
            // Must be consuming rollback space (or is xct_freeing_space)
            // would be < but for special rollback case, in which
            // we do a SMO, in which case they both can be 0.
            w_assert1(_log_bytes_rsvd <= rsvd); // consumed in _flush_logbuf
            w_assert1(_log_bytes_used >= used); // consumed in _flush_logbuf
            // but the above 2 numbers are adjusted by the actual
            // log bytes used
            w_assert1(_log_bytes_reserved_space >= requested); //monotonically incr
        }
    }
    else
    {
        w_assert1(_log_bytes_reserved_space == requested);
        w_assert1(_log_bytes_rsvd == rsvd);
        w_assert1(_log_bytes_used == used);
        w_assert1(_log_bytes_ready == ready);
    }

#endif

    return rc;
}

/*********************************************************************
 *
 *  xct_t::release_anchor(and_compensate)
 *
 *  stop critical sections vis-a-vis compensated operations
 *  If and_compensate==true, it makes the _last_log a clr
 *
 *********************************************************************/
void
xct_t::release_anchor( bool and_compensate ADD_LOG_COMMENT_SIG )
{
    FUNC(xct_t::release_anchor);

#if X_LOG_COMMENT_ON
    if(and_compensate) {
        w_ostrstream s;
        s << "release_anchor at "
            << debugmsg;
        W_COERCE(log_comment(s.c_str()));
    }
#endif
    DBGX(
            << " RELEASE ANCHOR "
            << " in compensated op==" << _in_compensated_op
            << " holds xct_mutex_1=="
            /*<< (const char *)(_1thread_xct.is_mine()? "true" : "false"*)*/
    );

    w_assert3(_in_compensated_op>0);

    if(_in_compensated_op == 1) { // will soon be 0

        // NB: this whole section could be made a bit
        // more efficient in the -UDEBUG case, but for
        // now, let's keep in all the checks

        // don't flush unless we have popped back
        // to the last compensate() of the bunch

        // Now see if this last item was supposed to be
        // compensated:
        if(and_compensate && (_anchor != lsn_t::null)) {
           VOIDSSMTEST("compensate");
           if(_last_log) {
               if ( _last_log->is_cpsn()) {
                    DBGX(<<"already compensated");
                    w_assert3(_anchor == _last_log->undo_nxt());
               } else {
                   DBGX(<<"SETTING anchor:" << _anchor);
                   w_assert3(_anchor <= _last_lsn);
                   _last_log->set_clr(_anchor);
               }
           } else {
               DBGX(<<"no _last_log:" << _anchor);
               /* Can we update the log record in the log buffer ? */
               if( log &&
                   !log->compensate(_last_lsn, _anchor).is_error()) {
                   // Yup.
                    INC_TSTAT(compensate_in_log);
               } else {
                   // Nope, write a compensation log record.
                   // Really, we should return an rc from this
                   // method so we can W_DO here, and we should
                   // check for eBADCOMPENSATION here and
                   // return all other errors  from the
                   // above log->compensate(...)

                   // compensations use _log_bytes_rsvd,
                   // not _log_bytes_ready
                   W_COERCE(log_compensate(_anchor));
                   INC_TSTAT(compensate_records);
               }
            }
        }

        _anchor = lsn_t::null;

    }
    // UN-PROTECT
    _in_compensated_op -- ;

    DBGX(
        << " out compensated op=" << _in_compensated_op
        << " holds xct_mutex_1=="
        /*        << (const char *)(_1thread_xct.is_mine()? "true" : "false")*/
    );
}

/*********************************************************************
 *
 *  xct_t::anchor( bool grabit )
 *
 *  Return a log anchor (begin a top level action).
 *
 *  If argument==true (most of the time), it stores
 *  the anchor for use with compensations.
 *
 *  When the  argument==false, this is used (by I/O monitor) not
 *  for compensations, but only for concurrency control.
 *
 *********************************************************************/
const lsn_t&
xct_t::anchor(bool grabit)
{
    // PROTECT
    _in_compensated_op ++;

    INC_TSTAT(anchors);
    DBGX(
            << " GRAB ANCHOR "
            << " in compensated op==" << _in_compensated_op
    );


    if(_in_compensated_op == 1 && grabit) {
        // _anchor is set to null when _in_compensated_op goes to 0
        w_assert3(_anchor == lsn_t::null);
        _anchor = _last_lsn;
        DBGX(    << " anchor =" << _anchor);
    }
    DBGX(    << " anchor returns " << _last_lsn );

    return _last_lsn;
}


/*********************************************************************
 *
 *  xct_t::compensate_undo(lsn)
 *
 *  compensation during undo is handled slightly differently--
 *  the gist of it is the same, but the assertions differ, and
 *  we have to acquire the mutex first
 *********************************************************************/
void
xct_t::compensate_undo(const lsn_t& lsn)
{
    DBGX(    << " compensate_undo (" << lsn << ") -- state=" << state());

    w_assert3(_in_compensated_op);
    // w_assert9(state() == xct_aborting); it's active if in sm::rollback_work

    _compensate(lsn, _last_log?_last_log->is_undoable_clr() : false);

}

/*********************************************************************
 *
 *  xct_t::compensate(lsn, bool undoable)
 *
 *  Generate a compensation log record to compensate actions
 *  started at "lsn" (commit a top level action).
 *  Generates a new log record only if it has to do so.
 *
 *********************************************************************/
void
xct_t::compensate(const lsn_t& lsn, bool undoable ADD_LOG_COMMENT_SIG)
{
    DBGX(    << " compensate(" << lsn << ") -- state=" << state());

    // acquire_1thread_mutex(); should already be mine

    _compensate(lsn, undoable);

    release_anchor(true ADD_LOG_COMMENT_USE);
}

/*********************************************************************
 *
 *  xct_t::_compensate(lsn, bool undoable)
 *
 *
 *  Generate a compensation log record to compensate actions
 *  started at "lsn" (commit a top level action).
 *  Generates a new log record only if it has to do so.
 *
 *  Special case of undoable compensation records is handled by the
 *  boolean argument. (NOT USED FOR NOW -- undoable_clrs were removed
 *  in 1997 b/c they weren't needed anymore;they were originally
 *  in place for an old implementation of extent-allocation. That's
 *  since been replaced by the dealaying of store deletion until end
 *  of xct).  The calls to the methods and infrastructure regarding
 *  undoable clrs was left in place in case it must be resurrected again.
 *  The reason it was removed is that there was some complexity involved
 *  in hanging onto the last log record *in the xct* in order to be
 *  sure that the compensation happens *in the correct log record*
 *  (because an undoable compensation means the log record holding the
 *  compensation isn't being compensated around, whereas turning any
 *  other record into a clr or inserting a stand-alone clr means the
 *  last log record inserted is skipped on undo).
 *  That complexity remains, since log records are flushed to the log
 *  immediately now (which was precluded for undoable_clrs ).
 *
 *********************************************************************/
void
xct_t::_compensate(const lsn_t& lsn, bool undoable)
{
    DBGX(    << "_compensate(" << lsn << ") -- state=" << state());

    bool done = false;
    if ( _last_log ) {
        // We still have the log record here, and
        // we can compensate it.
        // NOTE: we used to use this a lot but now the only
        // time this is possible (due to the fact that we flush
        // right at insert) is when the logging code is hand-written,
        // rather than Perl-generated.

        /*
         * lsn is got from anchor(), and anchor() returns _last_lsn.
         * _last_lsn is the lsn of the last log record
         * inserted into the log, and, since
         * this log record hasn't been inserted yet, this
         * function can't make a log record compensate to itself.
         */
        w_assert3(lsn <= _last_lsn);
        _last_log->set_clr(lsn);
        INC_TSTAT(compensate_in_xct);
        done = true;
    } else {
        /*
        // Log record has already been inserted into the buffer.
        // Perhaps we can update the log record in the log buffer.
        // However,  it's conceivable that nothing's been written
        // since _last_lsn, and we could be trying to compensate
        // around nothing.  This indicates an error in the calling
        // code.
        */
        if( lsn >= _last_lsn) {
            INC_TSTAT(compensate_skipped);
        }
        if( log && (! undoable) && (lsn < _last_lsn)) {
            if(!log->compensate(_last_lsn, lsn).is_error()) {
                INC_TSTAT(compensate_in_log);
                done = true;
            }
        }
    }

    if( !done && (lsn < _last_lsn) ) {
        /*
        // If we've actually written some log records since
        // this anchor (lsn) was grabbed,
        // force it to write a compensation-only record
        // either because there's no record on which to
        // piggy-back the compensation, or because the record
        // that's there is an undoable/compensation and will be
        // undone (and we *really* want to compensate around it)
        */

        // compensations use _log_bytes_rsvd, not _log_bytes_ready
        W_COERCE(log_compensate(lsn));
        INC_TSTAT(compensate_records);
    }
}



/*********************************************************************
 *
 *  xct_t::rollback(savept)
 *
 *  Rollback transaction up to "savept".
 *
 *********************************************************************/
rc_t
xct_t::rollback(const lsn_t &save_pt)
{
    FUNC(xct_t::rollback);
    DBGTHRD(<< "xct_t::rollback to " << save_pt);
    // W_DO(check_one_thread_attached()); // now checked in prologue
    // w_assert1(one_thread_attached());
    // Now we must just assert that at most 1 update thread is
    // attached
    w_assert0(update_threads()<=1);

    if(!log) {
        ss_m::errlog->clog  << emerg_prio
        << "Cannot roll back with logging turned off. "
        << flushl;
        return RC(eNOABORT);
    }

    w_rc_t            rc;
    logrec_t*         buf =0;

    if(_in_compensated_op > 0) {
        w_assert3(save_pt >= _anchor);
    } else {
        w_assert3(_anchor == lsn_t::null);
    }

    DBGX( << " in compensated op depth " <<  _in_compensated_op
            << " save_pt " << save_pt << " anchor " << _anchor);
    _in_compensated_op++;

    // rollback is only one type of compensated op, and it doesn't nest
    w_assert0(!_rolling_back);
    _rolling_back = true;

    // undo_nxt is the lsn of last recovery log for this txn
    lsn_t nxt = _undo_nxt;

    DBGOUT3(<<"Initial rollback, from: " << nxt << " to: " << save_pt);
    LOGTRACE( << setiosflags(ios::right) << nxt
              << resetiosflags(ios::right)
              << " Roll back " << " " << tid()
              << " to " << save_pt );
    _log_bytes_used_fwd  = _log_bytes_used; // for debugging

    { // Contain the scope of the following __copy__buf:

    logrec_t* __copy__buf = new logrec_t; // auto-del
    if(! __copy__buf)
        { W_FATAL(eOUTOFMEMORY); }
    w_auto_delete_t<logrec_t> auto_del(__copy__buf);
    logrec_t&         r = *__copy__buf;

    while (save_pt < nxt)
    {
        rc =  log->fetch(nxt, buf, 0, true);        
        if(rc.is_error() && rc.err_num()==eEOF) 
        {
            LOGTRACE2( << "U: end of log looking to fetch nxt=" << nxt);
            DBGX(<< " fetch returns EOF" );
            log->release();
            goto done;
        }
        else
        {
             LOGTRACE2( << "U: fetch nxt=" << nxt << "  returns rc=" << rc);

             logrec_t& temp = *buf;
             w_assert3(!temp.is_skip());

             /* Only copy the valid portion of
              * the log record, then release it
              */
             memcpy(__copy__buf, &temp, temp.length());

             log->release();
        }

        DBGOUT1(<<"Rollback, current undo lsn: " << nxt);

        if (r.is_undo())
        {
           w_assert1(nxt == r.lsn_ck());
            // r is undoable
            w_assert1(!r.is_single_sys_xct());
            w_assert1(!r.is_multi_page()); // All multi-page logs are SSX, so no UNDO.
            /*
             *  Undo action of r.
             */
            LOGTRACE1( << setiosflags(ios::right) << nxt
                      << resetiosflags(ios::right) << " U: " << r );

#if W_DEBUG_LEVEL > 2
            u_int    was_rsvd = _log_bytes_rsvd;
#endif
            lpid_t pid = r.construct_pid();
            fixable_page_h page;

            if (! r.is_logical())
            {
                // Operations such as foster adoption, load balance, etc.

                DBGOUT3 (<<"physical UNDO.. which is not quite good");
                // tentatively use fix_direct for this. eventually all physical UNDOs should go away
                rc = page.fix_direct(pid.vol().vol, pid.page, LATCH_EX);
                if(rc.is_error())
                {
                    goto done;
                }
                w_assert1(page.pid() == pid);
            }


            r.undo(page.is_fixed() ? &page : 0);

#if W_DEBUG_LEVEL > 2
            if(was_rsvd - _log_bytes_rsvd  > r.length()) {
                  LOGTRACE2(<< "U: len=" << r.length() << " B= " <<
                          (was_rsvd - _log_bytes_rsvd));
            }
#endif
            if(r.is_cpsn())
            {
                // A compensation log record
                w_assert1(r.is_undoable_clr());
                LOGTRACE2( << "U: compensating to " << r.undo_nxt() );
                nxt = r.undo_nxt();
                DBGOUT1(<<"Rollback, log record is compensation, undo_nxt: " << nxt);
            }
            else
            {
                // Not a compensation log record, use xid_prev() which is
                // previous logrec of this xct
                LOGTRACE2( << "U: undoing to " << r.xid_prev() );
                nxt = r.xid_prev();
                DBGOUT1(<<"Rollback, log record is not compensation, xid_prev: " << nxt);
            }
        }
        else  if (r.is_cpsn())
        {
            LOGTRACE2( << setiosflags(ios::right) << nxt
                      << resetiosflags(ios::right) << " U: " << r
                      << " compensating to " << r.undo_nxt() );
            if (r.is_single_sys_xct())
            {
                nxt = lsn_t::null;
            }
            else
            {
                nxt = r.undo_nxt();
            }
            // r.xid_prev() could just as well be null

        }
        else
        {
            // r is not undoable
            LOGTRACE2( << setiosflags(ios::right) << nxt
               << resetiosflags(ios::right) << " U: " << r
               << " skipping to " << r.xid_prev());
            if (r.is_single_sys_xct())
            {
                nxt = lsn_t::null;
            }
            else
            {
                nxt = r.xid_prev();
            }
            // w_assert9(r.undo_nxt() == lsn_t::null);
        }
    }

    // close scope so the
    // auto-release will free the log rec copy buffer, __copy__buf
    }

    _undo_nxt = nxt;
    _read_watermark = lsn_t::null;
    _xct_chain_len = 0;

done:

    DBGX( << "leaving rollback: compensated op " << _in_compensated_op);
    _in_compensated_op --;
    _rolling_back = false;
    w_assert3(_anchor == lsn_t::null ||
                _anchor == save_pt);

    if(save_pt != lsn_t::null) {
        INC_TSTAT(rollback_savept_cnt);
    }

    DBGTHRD(<< "xct_t::rollback done to " << save_pt);
    return rc;
}

void xct_t::AddStoreToFree(const stid_t& stid)
{
    FUNC(x);
    CRITICAL_SECTION(xctstructure, *this);
    _core->_storesToFree.push(new stid_list_elem_t(stid));
}

void xct_t::AddLoadStore(const stid_t& stid)
{
    FUNC(x);
    CRITICAL_SECTION(xctstructure, *this);
    _core->_loadStores.push(new stid_list_elem_t(stid));
}

/*
 * clear the list of stores to be freed upon xct completion
 * this is used by abort since rollback will recreate the
 * proper list of stores to be freed.
 *
 * don't *really* need mutex since only called when aborting => 1 thread
 * but we have the acquire here for internal documentation
 */
void
xct_t::ClearAllStoresToFree()
{
    w_assert2(one_thread_attached());
    stid_list_elem_t*        s = 0;
    while ((s = _core->_storesToFree.pop()))  {
        delete s;
    }

    w_assert3(_core->_storesToFree.is_empty());
}


/*
 * this function will free all the stores which need to freed
 * by this completing xct.
 *
 * don't REALLY need mutex since only called when committing/aborting
 * => 1 thread attached
 */
void
xct_t::FreeAllStoresToFree()
{
    // TODO not implemented
}

void
xct_t::DumpStoresToFree()
{
    stid_list_elem_t*                e;
    w_list_i<stid_list_elem_t,queue_based_lock_t>        i(_core->_storesToFree);

    FUNC(xct_t::DumpStoresToFree);
    CRITICAL_SECTION(xctstructure, *this);
    cout << "list of stores to free";
    while ((e = i.next()))  {
        cout << " <- " << e->stid;
    }
    cout << endl;
}

/*
 * Moved here to work around a gcc/egcs bug that
 * caused compiler to choke.
 */
class VolidCnt {
    private:
        int unique_vols;
        int vol_map[xct_t::max_vols];
        snum_t vol_cnts[xct_t::max_vols];
    public:
        VolidCnt() : unique_vols(0) {};
        int Lookup(int vol)
            {
                for (int i = 0; i < unique_vols; i++)
                    if (vol_map[i] == vol)
                        return i;

                w_assert9(unique_vols < xct_t::max_vols);
                vol_map[unique_vols] = vol;
                vol_cnts[unique_vols] = 0;
                return unique_vols++;
            };
        int Increment(int vol)
            {
                return ++vol_cnts[Lookup(vol)];
            };
        int Decrement(int vol)
            {
                w_assert9(vol_cnts[Lookup(vol)]);
                return --vol_cnts[Lookup(vol)];
            };
#if W_DEBUG_LEVEL > 2
        ~VolidCnt()
            {
                for (int i = 0; i < unique_vols; i ++)
                    w_assert9(vol_cnts[i] == 0);
            };
#endif
};

rc_t
xct_t::ConvertAllLoadStoresToRegularStores()
{

#if X_LOG_COMMENT_ON
    {
        static int uniq=0;
        static int last_uniq=0;

        // int    nv =  atomic_inc_nv(uniq);
        int nv =  lintel::unsafe::atomic_fetch_add(&uniq, 1);
        //Even if someone else slips in here, the
        //values should never match.
        w_assert1(last_uniq != nv);
        *&last_uniq = nv;

        // this is to help us figure out if we
        // are issuing duplicate commits/log entries or
        // if the problem is in the log code or the
        // grabbing of the 1thread log mutex
        w_ostrstream s;
        s << "ConvertAllLoadStores uniq=" << uniq
            << " xct state " << _core->_state
            << " xct aborted " << _core->_xct_aborting
            << " xct ended " << _core->_xct_ended
            << " tid " << tid()
            << " thread " << me()->id;
        W_DO(log_comment(s.c_str()));
    }
#endif

    w_assert2(one_thread_attached());
    stid_list_elem_t*        s = 0;
    VolidCnt cnt;

    {
        w_list_i<stid_list_elem_t,queue_based_lock_t> i(_core->_loadStores);

        while ((s = i.next()))  {
            cnt.Increment(s->stid.vol);
        }
    }

    while ((s = _core->_loadStores.pop()))  {
        bool sync_volume = (cnt.Decrement(s->stid.vol) == 0);
        store_flag_t f;
        W_DO( io->get_store_flags(s->stid, f, true /*ok if deleting*/));
        // if any combo of  st_tmp, st_insert_file, st_load_file, convert
        // but only insert and load are put into this list.
        if(f != st_regular) {
            W_DO( io->set_store_flags(s->stid, st_regular, sync_volume) );
        }
        delete s;
    }

    w_assert3(_core->_loadStores.is_empty());

    return RCOK;
}


void
xct_t::ClearAllLoadStores()
{
    w_assert2(one_thread_attached());
    stid_list_elem_t*        s = 0;
    while ((s = _core->_loadStores.pop()))  {
        delete s;
    }

    w_assert3(_core->_loadStores.is_empty());
}

void
xct_t::attach_thread()
{
    FUNC(xct_t::attach_thread);
    smthread_t *thr = g_me();
    CRITICAL_SECTION(xctstructure, *this);

    w_assert2(is_1thread_xct_mutex_mine());

    if (_core->_threads_attached++ > 0) {
        INC_TSTAT(mpl_attach_cnt);
    }
    w_assert2(_core->_threads_attached >=0);
    w_assert2(is_1thread_xct_mutex_mine());
    thr->new_xct(this);
    w_assert2(is_1thread_xct_mutex_mine());
}


void
xct_t::detach_thread()
{
    FUNC(xct_t::detach_thread);
    CRITICAL_SECTION(xctstructure, *this);
    w_assert3(is_1thread_xct_mutex_mine());
    _core->_threads_attached--;
    w_assert2(_core->_threads_attached >=0);
    me()->no_xct(this);
}

//
// one_thread_attached() does not acquire the 1thread mutex; it
// just checks that the vas isn't calling certain methods
// when other threads are still working on behalf of the same xct.
// It doesn't protect the vas from trying calling, say, commit and
// later attaching another thread while the commit is going on.
// --- Can't protect a vas from itself in all cases.
rc_t
xct_t::check_one_thread_attached() const
{
    if(one_thread_attached()) return RCOK;
    return RC(eTWOTHREAD);
}

bool
xct_t::one_thread_attached() const
{
    // This function is called in multiple places, including txn commit, abort,
    // savepoint, chain, change state, etc.
    // The original code (commented out) would acquire the read mutex on checkpoint,
    // which wiats until the checkpoint is done, it makes the checkpoint a blocking operation.
    //
    // The current code commented out the read mutex on checkpoint, in other words,
    // checkpoint is not a blocking operation anymore, but when checkpoint gathering
    // the txn data, it reads stable txn data by grabing a latch on txn object
    //
    if( _core->_threads_attached > 1) {
        // Does not wait for checkpoint to finish, checkpoint is a non-blocking operation
        // chkpt_serial_m::read_acquire();
        if( _core->_threads_attached > 1) {
            // chkpt_serial_m::read_release();

#if W_DEBUG_LEVEL > 2
            fprintf(stderr,
                    "Fatal VAS or SSM error: %s %d %s %d.%d \n",
                    "Only one thread allowed in this operation at any time.",
                    _core->_threads_attached.load(),
                    "threads are attached to xct",
                    tid().get_hi(), tid().get_lo()
            );
#endif
            return false;
        }
    }
    return true;
}

bool
xct_t::is_1thread_xct_mutex_mine() const
{
  return _core->_1thread_xct.is_mine(&me()->get_1thread_xct_me());
}

// Should be used with CRITICAL_SECTION
void
xct_t::acquire_1thread_xct_mutex() const // default: true
{
    w_assert1( ! is_1thread_xct_mutex_mine()) ;
    // We can already own the 1thread log mutx, if we're
    // in a top-level action or in the io_m.
    DBGX( << " acquire xct mutex");
    if(is_1thread_xct_mutex_mine()) {
        w_assert0(0); // we should not already own this.
        DBGX(<< "already mine");
        return;
    }
    // the queue_based_lock_t implementation can tell if it was
    // free or held; the w_pthread_lock_t cannot,
    // and always returns false.
    bool was_contended = _core->_1thread_xct.acquire(&me()->get_1thread_xct_me());
    if(was_contended)
        INC_TSTAT(await_1thread_xct);
    DBGX(    << " acquireD xct mutex");
    w_assert2(is_1thread_xct_mutex_mine());
}

void
xct_t::release_1thread_xct_mutex() const
{
    DBGX( << " release xct mutex");
    w_assert1(is_1thread_xct_mutex_mine());
    _core->_1thread_xct.release(me()->get_1thread_xct_me());
    DBGX(    << " releaseD xct mutex");
    w_assert1(!is_1thread_xct_mutex_mine());
}

ostream &
xct_t::dump_locks(ostream &out) const
{
    raw_lock_xct()->dump_lockinfo(out);
    return out;
}


smlevel_0::switch_t
xct_t::set_log_state(switch_t s, bool &)
{
    xct_log_t *mine = me()->xct_log();
    switch_t old = (mine->xct_log_is_off()? OFF: ON);
    if(s==OFF) mine->set_xct_log_off();
    else mine->set_xct_log_on();
    return old;
}

void
xct_t::restore_log_state(switch_t s, bool n )
{
    (void) set_log_state(s, n);
}

NORET
xct_dependent_t::xct_dependent_t(xct_t* xd) : _xd(xd), _registered(false)
{
}

void
xct_dependent_t::register_me() {
    // it's possible that there is no active xct when this
    // function is called, so be prepared for null
    xct_t* xd = _xd;
    if (xd) {
        W_COERCE( xd->add_dependent(this) );
    }
    _registered = true;
}

NORET
xct_dependent_t::~xct_dependent_t()
{
    w_assert2(_registered);
    // it's possible that there is no active xct the constructor
    // was called, so be prepared for null
    if (_link.member_of() != NULL) {
        w_assert1(_xd);
        // Have to remove it under protection of the 1thread_xct_mutex
        W_COERCE(_xd->remove_dependent(this));
    }
}
/**\endcond skip */


sys_xct_section_t::sys_xct_section_t(bool single_log_sys_xct)
{
    _original_xct_depth = me()->get_tcb_depth();
    _error_on_start = ss_m::begin_sys_xct(single_log_sys_xct);
}
sys_xct_section_t::~sys_xct_section_t()
{
    size_t xct_depth = me()->get_tcb_depth();
    if (xct_depth > _original_xct_depth) {
        rc_t result = ss_m::abort_xct();
        if (result.is_error()) {
#if W_DEBUG_LEVEL>0
            cerr << "system transaction automatic abort failed: " << result.err_num() << "\n";
#endif // W_DEBUG_LEVEL>0
        }
    }
}
rc_t sys_xct_section_t::end_sys_xct (rc_t result)
{
    if (result.is_error()) {
        W_DO (ss_m::abort_xct());
    } else {
        W_DO (ss_m::commit_sys_xct());
    }
    return RCOK;
}
