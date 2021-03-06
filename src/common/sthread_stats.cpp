/*<std-header orig-src='shore'>

 $Id: sthread_stats.cpp,v 1.10 2010/07/19 18:35:15 nhall Exp $

SHORE -- Scalable Heterogeneous Object REpository

Copyright (c) 1994-99 Computer Sciences Department, University of
                      Wisconsin -- Madison
All Rights Reserved.

Permission to use, copy, modify and distribute this software and its
documentation is hereby granted, provided that both the copyright
notice and this permission notice appear in all copies of the
software, derivative works or modified versions, and any portions
thereof, and that both notices appear in supporting documentation.

THE AUTHORS AND THE COMPUTER SCIENCES DEPARTMENT OF THE UNIVERSITY
OF WISCONSIN - MADISON ALLOW FREE USE OF THIS SOFTWARE IN ITS
"AS IS" CONDITION, AND THEY DISCLAIM ANY LIABILITY OF ANY KIND
FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.

This software was developed with support by the Advanced Research
Project Agency, ARPA order number 018 (formerly 8230), monitored by
the U.S. Army Research Laboratory under contract DAAB07-91-C-Q518.
Further funding for this work was provided by DARPA through
Rome Research Laboratory Contract No. F30602-97-2-0247.

*/

#include "w_defines.h"

/*  -- do not edit anything above this line --   </std-header>*/


#include "sthread_stats.h"
#include "sthread_stats_inc_gen.cpp"
/*
 * #include "sthread_stats_stat_gen.cpp"
 * #include "sthread_stats_out_gen.cpp"
 not used
 */

/**\cond skip */
const char *sthread_stats::stat_names[] = {
#include "sthread_stats_msg_gen.h"
    0
};
/**\endcond skip */

/* This replaces that generated by stats.pl */
ostream &operator <<(ostream &o, const sthread_stats &s)
{
    o << "STHREAD STATS:" << endl;

    o << "rwlock_r_wait:" << endl 
        << "  count: " << s.rwlock_r_wait
        << endl;
    o << "rwlock_w_wait:" << endl 
        << "  count: " << s.rwlock_w_wait
        << endl;

    return o;
}
