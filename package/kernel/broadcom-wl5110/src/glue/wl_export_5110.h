/* SPDX-License-Identifier: ISC */
/* Copyright (c) 2026 Andrew Druchenko */
/*
 * Interface the closed wlc core (wlc_core.o, from stock wl.ko 5.110.27.20012) expects from the Linux glue.
 * Recovered 2026-11-25 from: OpenWrt broadcom-wl 5.10.56 wl_export.h + disassembly of the stock 5.110 glue
 * (scripts/argcount.py). The core treats wl_info / wl_if / wl_timer as opaque pointers.
 */
#ifndef _WL_EXPORT_5110_H_
#define _WL_EXPORT_5110_H_

struct wl_info; struct wl_if; struct wl_timer; struct wlc_if; struct ether_addr;
struct bcmstrbuf; struct wlc_event; struct wl_rxsts;

/* lifecycle */
void   wl_init(struct wl_info *wl);                          /* stock: wlc_init() under lock */
uint   wl_reset(struct wl_info *wl);
int    wl_up(struct wl_info *wl);
void   wl_down(struct wl_info *wl);
void   wl_dump_ver(struct wl_info *wl, struct bcmstrbuf *b);

/* interrupts: stock wraps wlc_intrs*(wl->wlc) in local irq disable */
void   wl_intrson(struct wl_info *wl);
uint32 wl_intrsoff(struct wl_info *wl);
void   wl_intrsrestore(struct wl_info *wl, uint32 macintmask);

/* timers */
struct wl_timer *wl_init_timer(struct wl_info *wl, void (*fn)(void *arg), void *arg, const char *name);
void   wl_free_timer(struct wl_info *wl, struct wl_timer *t);
void   wl_add_timer(struct wl_info *wl, struct wl_timer *t, uint ms, int periodic);
bool   wl_del_timer(struct wl_info *wl, struct wl_timer *t);

/* data path. p is a SHADOW packet (struct skb2622 *), numpkt = chain length */
void   wl_sendup(struct wl_info *wl, struct wl_if *wlif, void *p, int numpkt);
/* 5.110: 4 args (5.10.56 had 3). wlif may be NULL -> primary netdev. prio is ignored by stock glue */
void   wl_txflowcontrol(struct wl_info *wl, struct wl_if *wlif, bool state, int prio);

/* events */
void   wl_event(struct wl_info *wl, char *ifname, struct wlc_event *e);
void   wl_event_sync(struct wl_info *wl, char *ifname, struct wlc_event *e);  /* stock: empty */

/* virtual interfaces (WDS, MBSS) */
struct wl_if *wl_add_if(struct wl_info *wl, struct wlc_if *wlcif, uint unit, struct ether_addr *remote);
void   wl_del_if(struct wl_info *wl, struct wl_if *wlif);
char  *wl_ifname(struct wl_info *wl, struct wl_if *wlif);

/* monitor mode: stock implementations are empty */
void   wl_monitor(struct wl_info *wl, struct wl_rxsts *rxsts, void *p);
void   wl_set_monitor(struct wl_info *wl, int val);

/* stock: returns TRUE */
bool   wl_alloc_dma_resources(struct wl_info *wl, uint dmaddrwidth);

/* Kernel/arch data symbols the core reads directly (variables, NOT functions):
 *   jiffies (u32), g_assert_type (int, from bcmutils/osl), bcm_ctype (uint8[256], bcmutils),
 *   _dma_cache_inv (function POINTER variable: void (*)(unsigned long start, unsigned long size)),
 *   wl_msg_level (defined inside the core itself). Functions: dump_stack, skb_under_panic. */
#endif
