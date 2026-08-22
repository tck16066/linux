#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include "rn_counters.h"

/* ---- transport: is_transfer_complete() ---- */
atomic_t rn_tc_call;
atomic_t rn_tc_no_xfer;
atomic_t rn_tc_no_xfer_done;
atomic_t rn_tc_no_xfer_wip;
atomic_t rn_tc_xfer_present;
atomic_t rn_tc_xfer_done;
atomic_t rn_tc_xfer_notdone;
atomic_long_t rn_tc_xfer_last_mc;
atomic_t rn_tc_stale;
atomic_t rn_tc_stale_done;
atomic_t rn_tc_stale_notdone;
atomic_t rn_tc_removed;
atomic_t rn_tc_removed_skip;

/* ---- client_cache: refault() ---- */
atomic_t rn_rf_enter;
atomic_t rn_rf_existing;
atomic_t rn_rf_existing_done;
atomic_t rn_rf_existing_wip;
atomic_t rn_rf_existing_err;
atomic_t rn_rf_evict_eagain;
atomic_t rn_rf_evict_enomem;
atomic_t rn_rf_noent;
atomic_t rn_rf_reuse_eagain;
atomic_t rn_rf_reuse_enomem;
atomic_t rn_rf_kp_null;
atomic_t rn_rf_refetch_eio;
atomic_t rn_rf_refetch_start;

/* ---- transfer-path diagnostics ---- */
atomic_t rn_dnh_enter;
atomic_t rn_dnh_noent;
atomic_t rn_dnh_send;
atomic_t rn_dnh_first_zero;
atomic_t rn_mrs_send;
atomic_t rn_sync_skip;
atomic_t rn_ack_drop_nf;
atomic_t rn_ack_drop_hack;
atomic_t rn_dmf_fail;
atomic_t rn_mrw_write;
atomic_t rn_mrw_first_zero;

/* ---- test module: stress outcomes ---- */
atomic_t rn_st_refault_ret0;
atomic_t rn_st_refault_eagain;
atomic_t rn_st_refault_enoent;
atomic_t rn_st_refault_err;
atomic_t rn_st_ret0_ok;
atomic_t rn_st_ret0_corrupt;
atomic_t rn_st_verify_none;

/* Export the stress-outcome counters so the (separate) test module can use
 * them. The transport/client_cache counters stay intra-core. */
EXPORT_SYMBOL_GPL(rn_st_refault_ret0);
EXPORT_SYMBOL_GPL(rn_st_refault_eagain);
EXPORT_SYMBOL_GPL(rn_st_refault_enoent);
EXPORT_SYMBOL_GPL(rn_st_refault_err);
EXPORT_SYMBOL_GPL(rn_st_ret0_ok);
EXPORT_SYMBOL_GPL(rn_st_ret0_corrupt);
EXPORT_SYMBOL_GPL(rn_st_verify_none);

static int rn_counters_show(struct seq_file *m, void *v)
{
	seq_puts(m, "remote_numa counters\n");
	seq_puts(m, "transport is_transfer_complete:\n");
	seq_printf(m, "  rn_tc_call=%d\n",        atomic_read(&rn_tc_call));
	seq_printf(m, "  rn_tc_no_xfer=%d\n",     atomic_read(&rn_tc_no_xfer));
	seq_printf(m, "  rn_tc_no_xfer_done=%d\n",  atomic_read(&rn_tc_no_xfer_done));
	seq_printf(m, "  rn_tc_no_xfer_wip=%d\n",   atomic_read(&rn_tc_no_xfer_wip));
	seq_printf(m, "  rn_tc_xfer_present=%d\n",  atomic_read(&rn_tc_xfer_present));
	seq_printf(m, "  rn_tc_xfer_done=%d\n",     atomic_read(&rn_tc_xfer_done));
	seq_printf(m, "  rn_tc_xfer_notdone=%d\n",  atomic_read(&rn_tc_xfer_notdone));
	seq_printf(m, "  rn_tc_xfer_last_mc=%ld\n", atomic_long_read(&rn_tc_xfer_last_mc));
	seq_printf(m, "  rn_tc_stale=%d\n",         atomic_read(&rn_tc_stale));
	seq_printf(m, "  rn_tc_stale_done=%d\n",    atomic_read(&rn_tc_stale_done));
	seq_printf(m, "  rn_tc_stale_notdone=%d\n", atomic_read(&rn_tc_stale_notdone));
	seq_printf(m, "  rn_tc_removed=%d\n",       atomic_read(&rn_tc_removed));
	seq_printf(m, "  rn_tc_removed_skip=%d\n",  atomic_read(&rn_tc_removed_skip));

	seq_puts(m, "client_cache refault:\n");
	seq_printf(m, "  rn_rf_enter=%d\n",          atomic_read(&rn_rf_enter));
	seq_printf(m, "  rn_rf_existing=%d\n",       atomic_read(&rn_rf_existing));
	seq_printf(m, "  rn_rf_existing_done=%d\n",  atomic_read(&rn_rf_existing_done));
	seq_printf(m, "  rn_rf_existing_wip=%d\n",   atomic_read(&rn_rf_existing_wip));
	seq_printf(m, "  rn_rf_existing_err=%d\n",   atomic_read(&rn_rf_existing_err));
	seq_printf(m, "  rn_rf_evict_eagain=%d\n",   atomic_read(&rn_rf_evict_eagain));
	seq_printf(m, "  rn_rf_evict_enomem=%d\n",   atomic_read(&rn_rf_evict_enomem));
	seq_printf(m, "  rn_rf_noent=%d\n",          atomic_read(&rn_rf_noent));
	seq_printf(m, "  rn_rf_reuse_eagain=%d\n",   atomic_read(&rn_rf_reuse_eagain));
	seq_printf(m, "  rn_rf_reuse_enomem=%d\n",   atomic_read(&rn_rf_reuse_enomem));
	seq_printf(m, "  rn_rf_kp_null=%d\n",        atomic_read(&rn_rf_kp_null));
	seq_printf(m, "  rn_rf_refetch_eio=%d\n",    atomic_read(&rn_rf_refetch_eio));
	seq_printf(m, "  rn_rf_refetch_start=%d\n",  atomic_read(&rn_rf_refetch_start));

	seq_puts(m, "transfer-path diagnostics:\n");
	seq_printf(m, "  rn_dnh_enter=%d\n",        atomic_read(&rn_dnh_enter));
	seq_printf(m, "  rn_dnh_noent=%d\n",        atomic_read(&rn_dnh_noent));
	seq_printf(m, "  rn_dnh_send=%d\n",         atomic_read(&rn_dnh_send));
	seq_printf(m, "  rn_dnh_first_zero=%d\n",   atomic_read(&rn_dnh_first_zero));
	seq_printf(m, "  rn_mrs_send=%d\n",         atomic_read(&rn_mrs_send));
	seq_printf(m, "  rn_sync_skip=%d\n",        atomic_read(&rn_sync_skip));
	seq_printf(m, "  rn_ack_drop_nf=%d\n",      atomic_read(&rn_ack_drop_nf));
	seq_printf(m, "  rn_ack_drop_hack=%d\n",    atomic_read(&rn_ack_drop_hack));
	seq_printf(m, "  rn_dmf_fail=%d\n",         atomic_read(&rn_dmf_fail));
	seq_printf(m, "  rn_mrw_write=%d\n",        atomic_read(&rn_mrw_write));
	seq_printf(m, "  rn_mrw_first_zero=%d\n",   atomic_read(&rn_mrw_first_zero));

	seq_puts(m, "stress outcomes:\n");
	seq_printf(m, "  rn_st_refault_ret0=%d\n",   atomic_read(&rn_st_refault_ret0));
	seq_printf(m, "  rn_st_refault_eagain=%d\n", atomic_read(&rn_st_refault_eagain));
	seq_printf(m, "  rn_st_refault_enoent=%d\n", atomic_read(&rn_st_refault_enoent));
	seq_printf(m, "  rn_st_refault_err=%d\n",    atomic_read(&rn_st_refault_err));
	seq_printf(m, "  rn_st_ret0_ok=%d\n",        atomic_read(&rn_st_ret0_ok));
	seq_printf(m, "  rn_st_ret0_corrupt=%d\n",   atomic_read(&rn_st_ret0_corrupt));
	seq_printf(m, "  rn_st_verify_none=%d\n",   atomic_read(&rn_st_verify_none));
	return 0;
}

static int rn_counters_open(struct inode *inode, struct file *file)
{
	return single_open(file, rn_counters_show, NULL);
}

static const struct file_operations rn_counters_fops = {
	.owner   = THIS_MODULE,
	.open    = rn_counters_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static struct dentry *rn_dbg_root;
static struct dentry *rn_dbg_counters;

int rn_counters_init(void)
{
	rn_dbg_root = debugfs_create_dir("remote_numa", NULL);
	if (IS_ERR(rn_dbg_root))
		return PTR_ERR(rn_dbg_root);

	rn_dbg_counters = debugfs_create_file("counters", 0444, rn_dbg_root,
					      NULL, &rn_counters_fops);
	if (IS_ERR(rn_dbg_counters)) {
		debugfs_remove(rn_dbg_root);
		rn_dbg_root = NULL;
		return PTR_ERR(rn_dbg_counters);
	}
	return 0;
}

void rn_counters_exit(void)
{
	debugfs_remove(rn_dbg_counters);
	debugfs_remove(rn_dbg_root);
	rn_dbg_counters = NULL;
	rn_dbg_root = NULL;
}

module_init(rn_counters_init);
module_exit(rn_counters_exit);
