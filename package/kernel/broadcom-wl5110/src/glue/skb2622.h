/* SPDX-License-Identifier: ISC */
/* Copyright (c) 2026 Andrew Druchenko */
/*
 * Shadow of struct sk_buff as compiled into the closed wlc core: ASUS/Broadcom Linux 2.6.22.19,
 * MIPS o32 little endian, HNDCTF on, PKTC off.
 * Verified offsets from the binary: data@144 head@140 len@88 (wlc_ether_8023hdr inline skb_push),
 * ctf_mac_len@96 & sp@36 (PKTCTFMAP in wlc_dofrag_tkip). Full census: out/skb_audit.txt.
 *
 * Fields the core reads/writes inline: next, cb[48] (PKTTAG), len, data_len (must stay 0),
 * ctf_mac_len (must stay 0 => CTF paths skipped), priority, cloned bit (PKTSHARED), tail, end, head, data.
 * prev (PKTLINK) is used by open bcmutils pktq code we compile ourselves.
 */
#ifndef _SKB2622_H_
#define _SKB2622_H_
#include <linux/types.h>
#include <linux/build_bug.h>

struct sk_buff;

struct skb2622 {
	struct skb2622	*next;		/*   0 */
	struct skb2622	*prev;		/*   4  PKTLINK */
	void		*sk;		/*   8 */
	u32		__pad0;		/*  12 */
	u64		tstamp;		/*  16  ktime_t, 8-aligned on o32 */
	void		*dev;		/*  24 */
	int		iif;		/*  28 */
	void		*dst;		/*  32 */
	void		*sp;		/*  36  CTFMAPPTR, must stay NULL */
	char		cb[48];		/*  40  PKTTAG (wlc_pkttag_t) */
	unsigned int	len;		/*  88 */
	unsigned int	data_len;	/*  92  read by inline skb_pull/__skb_trim checks: keep 0 */
	unsigned int	ctf_mac_len;	/*  96  FASTBUF 0x10 / CTFBUF 0x20: keep 0 */
	u32		csum;		/* 100 */
	u32		priority;	/* 104  PKTPRIO */
	u8		flags1;		/* 108  bit1 = cloned (PKTSHARED), bits2-3 ip_summed */
	u8		flags2;		/* 109 */
	u16		protocol;	/* 110 */
	void		*destructor;	/* 112 */
	u32		mark;		/* 116 */
	unsigned char	*transport_header, *network_header, *mac_header;	/* 120..128 */
	unsigned char	*tail;		/* 132 */
	unsigned char	*end;		/* 136 */
	unsigned char	*head;		/* 140 */
	unsigned char	*data;		/* 144 */
	unsigned int	truesize;	/* 148 */
	int		users;		/* 152 */
	unsigned char	wl_idx;		/* 156 */
	/* 160..191 in the ASUS kernel: nfct, nfct_reasm, nfcache, mac_len, hdr_len, tc_index/verd, imq.
	 * Not touched by the core; reused here for shadow bookkeeping. */
	struct sk_buff	*native;	/* 160  backing 4.14 skb whose buffer head/data/tail/end point into */
	u32		__spare[7];	/* 164..191 */
};

#define SKB2622_CHECK(f, off) BUILD_BUG_ON(offsetof(struct skb2622, f) != (off))
static inline void skb2622_layout_check(void)
{
	SKB2622_CHECK(next, 0);   SKB2622_CHECK(prev, 4);   SKB2622_CHECK(tstamp, 16);
	SKB2622_CHECK(sp, 36);    SKB2622_CHECK(cb, 40);    SKB2622_CHECK(len, 88);
	SKB2622_CHECK(data_len, 92); SKB2622_CHECK(ctf_mac_len, 96); SKB2622_CHECK(priority, 104);
	SKB2622_CHECK(flags1, 108); SKB2622_CHECK(tail, 132); SKB2622_CHECK(end, 136);
	SKB2622_CHECK(head, 140); SKB2622_CHECK(data, 144); SKB2622_CHECK(wl_idx, 156);
	SKB2622_CHECK(native, 160); BUILD_BUG_ON(sizeof(struct skb2622) != 192);
}
#endif

/* ---- 2.6.22 helper semantics, identical to what the core has inlined (data_len is always 0) ---- */
#ifndef _SKB2622_HELPERS_
#define _SKB2622_HELPERS_
#define SKB2622_CLONED(s)	(((s)->flags1 >> 1) & 1)
#define SKB2622_IP_SUMMED(s)	(((s)->flags1 >> 2) & 3)
#define SKB2622_SET_IP_SUMMED(s, v) ((s)->flags1 = ((s)->flags1 & ~0x0c) | (((v) & 3) << 2))
void skb_under_panic(void *skb, int len, void *here);	/* provided by the glue; called by inline push */
static inline unsigned char *skb2622_push(struct skb2622 *s, unsigned int n)
{
	s->data -= n; s->len += n;
	if (unlikely(s->data < s->head))
		skb_under_panic(s, n, __builtin_return_address(0));
	return s->data;
}
static inline unsigned char *skb2622_pull(struct skb2622 *s, unsigned int n)
{
	if (unlikely(n > s->len))
		return NULL;
	s->len -= n;
	return s->data += n;
}
static inline void skb2622_trim(struct skb2622 *s, unsigned int len)
{
	s->len = len; s->tail = s->data + len;
}
#endif
