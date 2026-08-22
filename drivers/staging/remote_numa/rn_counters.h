#ifndef REMOTE_NUMA_RN_COUNTERS_H
#define REMOTE_NUMA_RN_COUNTERS_H

#include <linux/types.h>
#include <linux/atomic.h>

/*
 * Hyper-specific diagnostic counters for the remote_numa refault/transfer
 * path.
 *
 * They are global (defined in rn_counters.c) so any core-module file can
 * increment them directly, and the exported ones can be incremented from the
 * test module. All of them are dumped on demand through the debugfs file:
 *
 *     /sys/kernel/debug/remote_numa/counters
 *
 * No printk is used for the high-frequency paths; the counters accumulate and
 * are read on demand, so they cannot be silenced by a corrupted log gate.
 */

/* ---- transport: remote_numa_transport_is_transfer_complete() ---- */
extern atomic_t rn_tc_call;        /* total calls to is_transfer_complete   */
extern atomic_t rn_tc_no_xfer;     /* xfer_get() returned NULL              */
extern atomic_t rn_tc_no_xfer_done;/* no-xfer && tip==0  -> returns true    */
extern atomic_t rn_tc_no_xfer_wip; /* no-xfer && tip!=0  -> returns false   */
extern atomic_t rn_tc_xfer_present;/* xfer_get() returned a xfer            */
extern atomic_t rn_tc_xfer_done;   /* xfer && max_contig>=PAGE -> true      */
extern atomic_t rn_tc_xfer_notdone;/* xfer && max_contig< PAGE -> false     */
extern atomic_long_t rn_tc_xfer_last_mc; /* last max_contig (not-done case) */
extern atomic_t rn_tc_stale;       /* xfer->cached_pg != cached_target      */
extern atomic_t rn_tc_stale_done;  /* stale && done (premature complete)    */
extern atomic_t rn_tc_stale_notdone;/* stale && !done                       */
extern atomic_t rn_tc_removed;     /* removed a completed xfer from table   */
extern atomic_t rn_tc_removed_skip;/* xfer already not hashed on remove     */

/* ---- client_cache: remote_numa_client_cache_refault() ---- */
extern atomic_t rn_rf_enter;       /* function entries                      */
extern atomic_t rn_rf_existing;    /* existing (mm,addr) entry found        */
extern atomic_t rn_rf_existing_done;/* existing && complete -> return 0     */
extern atomic_t rn_rf_existing_wip;/* existing && !complete && in-progress  */
extern atomic_t rn_rf_existing_err;/* existing && !complete && !in-progress */
extern atomic_t rn_rf_evict_eagain;/* maybe_evict -> -EAGAIN                */
extern atomic_t rn_rf_evict_enomem;/* maybe_evict -> other error            */
extern atomic_t rn_rf_noent;       /* refault_table_consume failed          */
extern atomic_t rn_rf_reuse_eagain;/* reuse_page fail, evict -EAGAIN/0      */
extern atomic_t rn_rf_reuse_enomem;/* reuse_page fail, evict other          */
extern atomic_t rn_rf_kp_null;     /* known_pages_lookup returned NULL      */
extern atomic_t rn_rf_refetch_eio; /* refetch_page_async failed             */
extern atomic_t rn_rf_refetch_start;/* refetch started -> return -EAGAIN    */

/* ---- transfer-path diagnostics (replaces bounded printk litter) ----
 * These live on both nodes; each node's debugfs shows its own counters.
 * The *_first_zero counters flag a page whose first byte is 0, a strong
 * indicator of a zeroed/wrong page being sent or received (corruption).
 */
extern atomic_t rn_dnh_enter;        /* donor refetch handler ENTER           */
extern atomic_t rn_dnh_noent;        /* donor refetch handler: page not found */
extern atomic_t rn_dnh_send;         /* donor refetch handler: sent a page    */
extern atomic_t rn_dnh_first_zero;   /* donor sent page with first byte == 0  */
extern atomic_t rn_mrs_send;         /* main: refetch_page_async issued       */
extern atomic_t rn_sync_skip;        /* mem_sync copy skipped (page missing)  */
extern atomic_t rn_ack_drop_nf;      /* sat_ack dropped: xfer not found       */
extern atomic_t rn_ack_drop_hack;    /* sat_ack dropped: hack mismatch        */
extern atomic_t rn_dmf_fail;         /* donor mem_alloc failed (no page)      */
extern atomic_t rn_mrw_write;        /* main: sat payload written to target   */
extern atomic_t rn_mrw_first_zero;   /* main: received first byte == 0        */

/* ---- test module: stress outcomes (globals survive per-thread resets) ---- */
extern atomic_t rn_st_refault_ret0;  /* refault returned 0                  */
extern atomic_t rn_st_refault_eagain;/* refault returned -EAGAIN (final)    */
extern atomic_t rn_st_refault_enoent;/* refault returned -ENOENT            */
extern atomic_t rn_st_refault_err;   /* refault returned other error        */
extern atomic_t rn_st_ret0_ok;       /* ret==0 and page verified good       */
extern atomic_t rn_st_ret0_corrupt;  /* ret==0 and page verified BAD        */
extern atomic_t rn_st_verify_none;   /* ret==0 but no seed to verify        */

/* debugfs plumbing (lives in the core module) */
int  rn_counters_init(void);
void rn_counters_exit(void);

#endif /* REMOTE_NUMA_RN_COUNTERS_H */
