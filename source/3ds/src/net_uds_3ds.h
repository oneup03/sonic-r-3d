/**
 * net_uds_3ds.h — 3DS network transports.
 *
 * ONLINE  = the shared UDP transport (net/net_transport.c) over libctru's
 *           BSD sockets (socInit), for LAN / Internet play with other builds.
 * LOCAL   = UDS local wireless (no access point), 3DS to 3DS only.
 *
 * The two can't share the radio: UDS and SOC are brought up lazily by
 * whichever one the lobby picks, and the other is torn down first.
 */
#ifndef NET_UDS_3DS_H
#define NET_UDS_3DS_H

#include <stdint.h>

/* 1 = LOCAL (UDS), 0 = ONLINE (sockets). Toggled in the lobby. */
extern int g_netLocalMode;

/* Bring up sockets for ONLINE mode (tearing down UDS if it is up).
 * 0 on success, -1 if there is no network (no Wi-Fi / no access point). */
int  net3ds_online_prepare(void);
/* Tear everything down (HOME/exit). */
void net3ds_shutdown(void);
/* 1 if the wireless is switched on at all. */
int  net3ds_wireless_enabled(void);

/* The LOCAL implementation of net_transport.h. */
int  net_uds_host_start(int port);
int  net_uds_client_connect(const char *host_ip, int port);
int  net_uds_send_to_host(const void *data, int len);
int  net_uds_broadcast(const void *data, int len);
int  net_uds_send_to(int player_slot, const void *data, int len);
int  net_uds_recv(void *buf, int maxlen, int *from_slot);
int  net_uds_register_client(int slot);
int  net_uds_discover_send(int port);
int  net_uds_discover_check(char *host_ip, int host_ip_len);
int  net_uds_is_active(void);
int  net_uds_is_host(void);
int  net_uds_local_slot(void);
void net_uds_set_local_slot(int slot);
void net_uds_close(void);
void net_uds_unregister_slot(int slot);
int  net_uds_slot_is_connected(int slot);

#endif
