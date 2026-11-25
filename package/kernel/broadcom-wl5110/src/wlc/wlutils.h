/*
 * Minimal wl ioctl helpers for wlc (replaces Broadcom's libshared wlutils).
 * Written for the broadcom-wl5110 port; talks to wl.ko through SIOCDEVPRIVATE + wl_ioctl_t.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License version 2 as published by the Free Software Foundation.
 */
#ifndef _WLUTILS_H_
#define _WLUTILS_H_

#include <typedefs.h>
#include <wlioctl.h>

int wl_ioctl(char *name, int cmd, void *buf, int len);
int wl_probe(char *name);

int wl_iovar_get(char *ifname, char *iovar, void *bufptr, int buflen);
int wl_iovar_set(char *ifname, char *iovar, void *param, int paramlen);
int wl_iovar_getint(char *ifname, char *iovar, int *val);
int wl_iovar_setint(char *ifname, char *iovar, int val);

int wl_bssiovar_get(char *ifname, char *iovar, int bssidx, void *outbuf, int len);
int wl_bssiovar_set(char *ifname, char *iovar, int bssidx, void *param, int paramlen);
int wl_bssiovar_setint(char *ifname, char *iovar, int bssidx, int val);

#endif
