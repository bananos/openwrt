/*
 * Minimal wl ioctl helpers for wlc (replaces Broadcom's libshared wlutils).
 *
 * Named variables ("iovars") travel through WLC_GET_VAR/WLC_SET_VAR as "name\0" followed by the
 * value; per-BSS variables as "bsscfg:name\0" + int32 bss index + value. A GET returns the value
 * at the start of the same buffer.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License version 2 as published by the Free Software Foundation.
 */
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/sockios.h>

#include "wlutils.h"

int wl_ioctl(char *name, int cmd, void *buf, int len)
{
	struct ifreq ifr;
	wl_ioctl_t ioc;
	int s, ret;

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return -1;
	memset(&ioc, 0, sizeof(ioc));
	ioc.cmd = cmd;
	ioc.buf = buf;
	ioc.len = len;
	/* ioc.set is optional: the core decides get/set by cmd (the glue only uses it for a
	 * CAP_NET_ADMIN check), and wlc runs as root */
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	ifr.ifr_data = (caddr_t)&ioc;
	ret = ioctl(s, SIOCDEVPRIVATE, &ifr);
	close(s);
	return ret;
}

int wl_probe(char *name)
{
	int val;

	if (wl_ioctl(name, WLC_GET_MAGIC, &val, sizeof(val)) < 0 || val != WLC_IOCTL_MAGIC)
		return -1;
	if (wl_ioctl(name, WLC_GET_VERSION, &val, sizeof(val)) < 0 || val > WLC_IOCTL_VERSION)
		return -1;
	return 0;
}

static int iovar_mkbuf(const char *prefix, const char *iovar, int bssidx, int with_idx,
		       const void *param, int paramlen, char *buf, int buflen)
{
	int plen = strlen(prefix), nlen = strlen(iovar) + 1, len = plen + nlen;

	if (with_idx)
		len += sizeof(int32);
	if (len + paramlen > buflen) {
		errno = ENOMEM;
		return -1;
	}
	memcpy(buf, prefix, plen);
	memcpy(buf + plen, iovar, nlen);
	if (with_idx)
		memcpy(buf + plen + nlen, &bssidx, sizeof(int32));
	if (paramlen)
		memcpy(buf + len, param, paramlen);
	return len + paramlen;
}

int wl_iovar_get(char *ifname, char *iovar, void *bufptr, int buflen)
{
	char tmp[WLC_IOCTL_SMLEN];
	char *buf = buflen > (int)sizeof(tmp) ? bufptr : tmp;
	int size = buflen > (int)sizeof(tmp) ? buflen : (int)sizeof(tmp);
	int ret;

	if (iovar_mkbuf("", iovar, 0, 0, NULL, 0, buf, size) < 0)
		return -1;
	ret = wl_ioctl(ifname, WLC_GET_VAR, buf, size);
	if (!ret && buf != bufptr)
		memcpy(bufptr, buf, buflen);
	return ret;
}

int wl_iovar_set(char *ifname, char *iovar, void *param, int paramlen)
{
	char buf[WLC_IOCTL_MAXLEN];
	int len = iovar_mkbuf("", iovar, 0, 0, param, paramlen, buf, sizeof(buf));

	return len < 0 ? -1 : wl_ioctl(ifname, WLC_SET_VAR, buf, len);
}

int wl_iovar_getint(char *ifname, char *iovar, int *val)
{
	return wl_iovar_get(ifname, iovar, val, sizeof(*val));
}

int wl_iovar_setint(char *ifname, char *iovar, int val)
{
	return wl_iovar_set(ifname, iovar, &val, sizeof(val));
}

int wl_bssiovar_get(char *ifname, char *iovar, int bssidx, void *outbuf, int len)
{
	char buf[WLC_IOCTL_MAXLEN];
	int ret;

	if (len > (int)sizeof(buf) ||
	    iovar_mkbuf("bsscfg:", iovar, bssidx, 1, NULL, 0, buf, sizeof(buf)) < 0)
		return -1;
	ret = wl_ioctl(ifname, WLC_GET_VAR, buf, sizeof(buf));
	if (!ret)
		memcpy(outbuf, buf, len);
	return ret;
}

int wl_bssiovar_set(char *ifname, char *iovar, int bssidx, void *param, int paramlen)
{
	char buf[WLC_IOCTL_MAXLEN];
	int len = iovar_mkbuf("bsscfg:", iovar, bssidx, 1, param, paramlen, buf, sizeof(buf));

	return len < 0 ? -1 : wl_ioctl(ifname, WLC_SET_VAR, buf, len);
}

int wl_bssiovar_setint(char *ifname, char *iovar, int bssidx, int val)
{
	return wl_bssiovar_set(ifname, iovar, bssidx, &val, sizeof(val));
}
