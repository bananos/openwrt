// SPDX-License-Identifier: GPL-2.0-only
/*
 * Optional register capture (WL_CAPTURE builds only, never shipped): logs the PHY, radio and PHY
 * table writes the wl core makes, for reference when adding a radio to an open driver.
 *
 * scripts/wl_hook.py renames the core's accessors to __real_<name> and points every caller at
 * the wrappers below. Only the outermost accessor of a call is logged (mod_radio_reg calls
 * write_radio_reg, wlc_phy_write_table calls phy_reg_write...). No SHM or key writes are logged.
 *
 * /proc/wlcap: read gives one op per line; write "on", "off", "clear" or "mark <n>".
 *
 * Copyright (c) 2026 Andrew Druchenko
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

typedef unsigned long ul;

struct wlcap_rec {
	u8 op;
	u8 width;
	u16 addr;
	u32 val;
};

/* the core's PHY table descriptor (phytbl_info_t) */
struct wlcap_tbl {
	const void *ptr;
	u32 len;
	u32 id;
	u32 offset;
	u32 width;
};

#define WLCAP_RECS	(256 * 1024)	/* 2 MB */

static struct wlcap_rec *wlcap_buf;
static unsigned int wlcap_n, wlcap_dropped;
static int wlcap_depth;
static bool wlcap_on = true;
module_param_named(capture, wlcap_on, bool, 0644);
MODULE_PARM_DESC(capture, "Log PHY/radio/table writes from module load (default on)");
static DEFINE_SPINLOCK(wlcap_lock);

static void wlcap_put(u8 op, u8 width, u16 addr, u32 val)
{
	unsigned long flags;

	if (!wlcap_on || !wlcap_buf)
		return;
	spin_lock_irqsave(&wlcap_lock, flags);
	if (wlcap_n < WLCAP_RECS) {
		struct wlcap_rec *r = &wlcap_buf[wlcap_n++];

		r->op = op;
		r->width = width;
		r->addr = addr;
		r->val = val;
	} else {
		wlcap_dropped++;
	}
	spin_unlock_irqrestore(&wlcap_lock, flags);
}

/* The wrappers pass every argument through unchanged: same registers, same stack slot. */
#define WLCAP_WRAP2(name, op)							\
	extern ul __real_##name(void *pi, ul a, ul v);				\
	ul name(void *pi, ul a, ul v);						\
	ul name(void *pi, ul a, ul v)						\
	{									\
		ul r;								\
		if (!wlcap_depth++)						\
			wlcap_put(op, 0, a, (u16)v);				\
		r = __real_##name(pi, a, v);					\
		wlcap_depth--;							\
		return r;							\
	}

#define WLCAP_WRAP3(name, op)							\
	extern ul __real_##name(void *pi, ul a, ul m, ul v);			\
	ul name(void *pi, ul a, ul m, ul v);					\
	ul name(void *pi, ul a, ul m, ul v)					\
	{									\
		ul r;								\
		if (!wlcap_depth++)						\
			wlcap_put(op, 0, a, ((u32)(u16)m << 16) | (u16)v);	\
		r = __real_##name(pi, a, m, v);					\
		wlcap_depth--;							\
		return r;							\
	}

WLCAP_WRAP2(write_radio_reg, 'R')
WLCAP_WRAP3(mod_radio_reg, 'M')
WLCAP_WRAP2(and_radio_reg, 'A')
WLCAP_WRAP2(or_radio_reg, 'O')
WLCAP_WRAP2(xor_radio_reg, 'X')
WLCAP_WRAP2(phy_reg_write, 'r')
WLCAP_WRAP3(phy_reg_mod, 'm')
WLCAP_WRAP2(phy_reg_and, 'a')
WLCAP_WRAP2(phy_reg_or, 'o')

extern ul __real_wlc_phy_write_table(void *pi, const struct wlcap_tbl *t, ul addr, ul hi, ul lo);
ul wlc_phy_write_table(void *pi, const struct wlcap_tbl *t, ul addr, ul hi, ul lo);
ul wlc_phy_write_table(void *pi, const struct wlcap_tbl *t, ul addr, ul hi, ul lo)
{
	ul r;

	if (!wlcap_depth++) {
		u32 i;

		wlcap_put('T', t->width, t->id, (t->offset << 16) | (t->len & 0xffff));
		for (i = 0; i < t->len; i++) {
			u32 v = t->width == 32 ? ((const u32 *)t->ptr)[i] :
				t->width == 16 ? ((const u16 *)t->ptr)[i] :
				((const u8 *)t->ptr)[i];
			wlcap_put('D', t->width, i, v);
		}
	}
	r = __real_wlc_phy_write_table(pi, t, addr, hi, lo);
	wlcap_depth--;
	return r;
}

static void *wlcap_start(struct seq_file *s, loff_t *pos)
{
	return *pos < wlcap_n ? &wlcap_buf[*pos] : NULL;
}

static void *wlcap_next(struct seq_file *s, void *v, loff_t *pos)
{
	++*pos;
	return wlcap_start(s, pos);
}

static void wlcap_stop(struct seq_file *s, void *v)
{
}

static int wlcap_show(struct seq_file *s, void *v)
{
	const struct wlcap_rec *r = v;

	switch (r->op) {
	case 'M':
	case 'm':
		seq_printf(s, "%c %04x %04x %04x\n", r->op, r->addr, r->val >> 16, r->val & 0xffff);
		break;
	case 'T':
		seq_printf(s, "T id=%u off=%u width=%u len=%u\n", r->addr, r->val >> 16, r->width,
			   r->val & 0xffff);
		break;
	case 'D':
		seq_printf(s, "D %u %0*x\n", r->addr, r->width / 4, r->val);
		break;
	case '#':
		seq_printf(s, "# mark %u\n", r->val);
		break;
	default:
		seq_printf(s, "%c %04x %04x\n", r->op, r->addr, r->val);
	}
	return 0;
}

static const struct seq_operations wlcap_seq_ops = {
	.start = wlcap_start,
	.next = wlcap_next,
	.stop = wlcap_stop,
	.show = wlcap_show,
};

static int wlcap_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &wlcap_seq_ops);
}

static ssize_t wlcap_write(struct file *file, const char __user *ubuf, size_t len, loff_t *off)
{
	char b[32];
	unsigned int n;
	size_t l = min(len, sizeof(b) - 1);

	if (copy_from_user(b, ubuf, l))
		return -EFAULT;
	b[l] = 0;
	if (!strncmp(b, "on", 2)) {
		wlcap_on = true;
	} else if (!strncmp(b, "off", 3)) {
		wlcap_on = false;
	} else if (!strncmp(b, "clear", 5)) {
		wlcap_n = wlcap_dropped = 0;
	} else if (sscanf(b, "mark %u", &n) == 1) {
		bool on = wlcap_on;

		wlcap_on = true;
		wlcap_put('#', 0, 0, n);
		wlcap_on = on;
	} else {
		return -EINVAL;
	}
	return len;
}

static const struct file_operations wlcap_fops = {
	.owner = THIS_MODULE,
	.open = wlcap_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
	.write = wlcap_write,
};

int wl_capture_init(void);
void wl_capture_exit(void);

int wl_capture_init(void)
{
	wlcap_buf = vmalloc(WLCAP_RECS * sizeof(*wlcap_buf));
	if (!wlcap_buf)
		return -ENOMEM;
	if (!proc_create("wlcap", 0600, NULL, &wlcap_fops)) {
		vfree(wlcap_buf);
		wlcap_buf = NULL;
		return -ENOMEM;
	}
	pr_info("wl: register capture on /proc/wlcap (%u records, %s)\n", WLCAP_RECS,
		wlcap_on ? "on" : "off");
	return 0;
}

void wl_capture_exit(void)
{
	remove_proc_entry("wlcap", NULL);
	if (wlcap_dropped)
		pr_info("wl: register capture dropped %u records\n", wlcap_dropped);
	vfree(wlcap_buf);
	wlcap_buf = NULL;
}
