/**
 * net_uds_3ds.c — local wireless (UDS) transport and the socket bring-up for
 * the 3DS. See net_uds_3ds.h.
 *
 * Mirrors net/net_transport.c's model: one host (slot 0) and up to three
 * clients; the host learns a client's identity from its first packet and
 * hands it the next free slot; discovery is "find the host" — here a beacon
 * scan instead of a UDP broadcast. Network node IDs are UDS's: 1 is the
 * host, 2.. the clients, 0xFFFF broadcast. The game's 512-byte packets fit
 * a single UDS data frame.
 */

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_uds_3ds.h"
#include "net_transport.h"
#include "endian_util.h"

int g_netLocalMode = 0;

/* --- SOC (online) --------------------------------------------------------- */
#define SOC_BUFSIZE 0x100000
static u32 *s_socBuf = NULL;
static int  s_socUp = 0;

/* --- UDS (local) ---------------------------------------------------------- */
#define UDS_WLANCOMMID   0x00536F52u          /* "SoR" */
#define UDS_ID8          0x52
#define UDS_CHANNEL      1
#define UDS_SHAREDMEM    0x3000
static const char s_passphrase[] = "SonicR-3D";

static int  s_udsUp = 0;
static int  s_active = 0;
static int  s_isHost = 0;
static int  s_localSlot = 0;
static udsBindContext s_bind;
static u16  s_slotNode[NET_MAX_PLAYERS];    /* node id per slot, 0 = free */
static udsNetworkStruct s_foundNet;
static int  s_haveFound = 0;
static u8  *s_scanBuf = NULL;
#define SCAN_BUFSIZE 0x4000

int net3ds_wireless_enabled(void)
{
    /* Both services refuse to start with the wireless switch off; report
     * "enabled" and let the caller see the failure. */
    return 1;
}

static void uds_down(void)
{
    if (!s_udsUp) return;
    if (s_active) {
        udsUnbind(&s_bind);
        if (s_isHost) udsDestroyNetwork(); else udsDisconnectNetwork();
    }
    udsExit();
    s_udsUp = 0;
    s_active = 0;
    s_isHost = 0;
}

static void soc_down(void)
{
    if (!s_socUp) return;
    socExit();
    s_socUp = 0;
    free(s_socBuf);
    s_socBuf = NULL;
}

int net3ds_online_prepare(void)
{
    if (s_socUp) return 0;
    uds_down();
    s_socBuf = (u32 *)memalign(0x1000, SOC_BUFSIZE);
    if (s_socBuf == NULL) return -1;
    Result rc = socInit(s_socBuf, SOC_BUFSIZE);
    if (R_FAILED(rc)) {
        fprintf(stderr, "net: socInit failed (%08lX) — no Wi-Fi connection?\n", (unsigned long)rc);
        free(s_socBuf);
        s_socBuf = NULL;
        return -1;
    }
    s_socUp = 1;
    fprintf(stderr, "net: sockets up\n");
    return 0;
}

static int uds_up(void)
{
    if (s_udsUp) return 0;
    soc_down();
    Result rc = udsInit(UDS_SHAREDMEM, NULL);
    if (R_FAILED(rc)) {
        fprintf(stderr, "net: udsInit failed (%08lX) — wireless off?\n", (unsigned long)rc);
        return -1;
    }
    if (s_scanBuf == NULL) {
        s_scanBuf = (u8 *)malloc(SCAN_BUFSIZE);
    }
    s_udsUp = 1;
    fprintf(stderr, "net: local wireless up\n");
    return 0;
}

void net3ds_shutdown(void)
{
    uds_down();
    soc_down();
}

static int slot_of_node(u16 node)
{
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (s_slotNode[i] == node) return i;
    }
    return -1;
}

/* --- transport ------------------------------------------------------------ */

int net_uds_host_start(int port)
{
    (void)port;
    if (s_active && s_isHost) return 0;
    if (s_active) net_uds_close();
    if (uds_up() < 0) return -1;

    udsNetworkStruct net;
    udsGenerateDefaultNetworkStruct(&net, UDS_WLANCOMMID, UDS_ID8, NET_MAX_PLAYERS);
    Result rc = udsCreateNetwork(&net, s_passphrase, sizeof(s_passphrase), &s_bind,
                                 UDS_CHANNEL, UDS_DEFAULT_RECVBUFSIZE);
    if (R_FAILED(rc)) {
        fprintf(stderr, "net: udsCreateNetwork failed (%08lX)\n", (unsigned long)rc);
        return -1;
    }
    memset(s_slotNode, 0, sizeof(s_slotNode));
    s_slotNode[0] = UDS_HOST_NETWORKNODEID;
    s_isHost = 1;
    s_active = 1;
    s_localSlot = 0;
    fprintf(stderr, "net: hosting a local wireless game\n");
    return 0;
}

int net_uds_client_connect(const char *host_ip, int port)
{
    (void)host_ip; (void)port;
    if (s_active) net_uds_close();
    if (uds_up() < 0) return -1;
    if (!s_haveFound) {
        /* Direct join without a prior scan: look once now. */
        if (net_uds_discover_send(port) < 0) return -1;
        if (!s_haveFound) {
            fprintf(stderr, "net: no local host found\n");
            return -1;
        }
    }
    Result rc = udsConnectNetwork(&s_foundNet, s_passphrase, sizeof(s_passphrase), &s_bind,
                                  UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client,
                                  UDS_CHANNEL, UDS_DEFAULT_RECVBUFSIZE);
    if (R_FAILED(rc)) {
        fprintf(stderr, "net: udsConnectNetwork failed (%08lX)\n", (unsigned long)rc);
        return -1;
    }
    memset(s_slotNode, 0, sizeof(s_slotNode));
    s_slotNode[0] = UDS_HOST_NETWORKNODEID;
    s_isHost = 0;
    s_active = 1;
    s_localSlot = -1;   /* the host assigns it */
    fprintf(stderr, "net: joined a local wireless game\n");
    return 0;
}

static int uds_send(u16 node, const void *data, int len)
{
    if (!s_active) return -1;
    if (len > (int)UDS_DATAFRAME_MAXSIZE) len = (int)UDS_DATAFRAME_MAXSIZE;
    Result rc = udsSendTo(node, UDS_CHANNEL, UDS_SENDFLAG_Default, data, (size_t)len);
    if (R_FAILED(rc)) return -1;
    return len;
}

int net_uds_send_to_host(const void *data, int len)
{
    return uds_send(UDS_HOST_NETWORKNODEID, data, len);
}

int net_uds_broadcast(const void *data, int len)
{
    if (!s_active) return -1;
    if (s_isHost) {
        return uds_send(UDS_BROADCAST_NETWORKNODEID, data, len) < 0 ? -1 : 0;
    }
    return uds_send(UDS_HOST_NETWORKNODEID, data, len) < 0 ? -1 : 0;
}

int net_uds_send_to(int player_slot, const void *data, int len)
{
    if (player_slot < 0 || player_slot >= NET_MAX_PLAYERS) return -1;
    if (s_slotNode[player_slot] == 0) return -1;
    return uds_send(s_slotNode[player_slot], data, len);
}

int net_uds_recv(void *buf, int maxlen, int *from_slot)
{
    if (!s_active) return 0;
    size_t actual = 0;
    u16 src = 0;
    Result rc = udsPullPacket(&s_bind, buf, (size_t)maxlen, &actual, &src);
    if (R_FAILED(rc) || actual == 0) {
        return 0;
    }
    int slot = slot_of_node(src);
    if (s_isHost && slot < 0) {
        for (int i = 1; i < NET_MAX_PLAYERS; i++) {
            if (s_slotNode[i] == 0) {
                s_slotNode[i] = src;
                slot = i;
                fprintf(stderr, "net: player %d joined (node %u)\n", i, (unsigned)src);
                break;
            }
        }
    }
    if (from_slot) *from_slot = slot;
    return (int)actual;
}

int net_uds_register_client(int slot)
{
    if (slot < 0 || slot >= NET_MAX_PLAYERS) return -1;
    return slot;   /* assigned in net_uds_recv, as the socket transport does */
}

/* Discovery = scanning for our beacon. Blocking for a fraction of a second;
 * the lobby calls it every 64 frames while looking for a host. */
int net_uds_discover_send(int port)
{
    (void)port;
    if (uds_up() < 0) return -1;
    if (s_scanBuf == NULL) return -1;
    udsNetworkScanInfo *networks = NULL;
    size_t total = 0;
    Result rc = udsScanBeacons(s_scanBuf, SCAN_BUFSIZE, &networks, &total, UDS_WLANCOMMID, UDS_ID8, NULL, false);
    if (R_FAILED(rc)) {
        fprintf(stderr, "net: udsScanBeacons failed (%08lX)\n", (unsigned long)rc);
        return -1;
    }
    if (total > 0 && networks != NULL) {
        s_foundNet = networks[0].network;
        s_haveFound = 1;
        fprintf(stderr, "net: found %u local host(s)\n", (unsigned)total);
    }
    free(networks);
    return 0;
}

int net_uds_discover_check(char *host_ip, int host_ip_len)
{
    if (!s_haveFound) return 0;
    if (host_ip && host_ip_len > 0) {
        strncpy(host_ip, "local", (size_t)host_ip_len - 1);
        host_ip[host_ip_len - 1] = '\0';
    }
    return 1;
}

int net_uds_is_active(void)    { return s_active; }
int net_uds_is_host(void)      { return s_isHost; }
int net_uds_local_slot(void)   { return s_localSlot; }
void net_uds_set_local_slot(int slot) { s_localSlot = slot; }

void net_uds_close(void)
{
    if (s_udsUp && s_active) {
        udsUnbind(&s_bind);
        if (s_isHost) udsDestroyNetwork(); else udsDisconnectNetwork();
    }
    memset(s_slotNode, 0, sizeof(s_slotNode));
    s_haveFound = 0;
    s_active = 0;
    s_isHost = 0;
    s_localSlot = 0;
    fprintf(stderr, "net: local closed\n");
}

void net_uds_unregister_slot(int slot)
{
    if (slot <= 0 || slot >= NET_MAX_PLAYERS) return;
    if (s_isHost && s_slotNode[slot] != 0) {
        udsEjectClient(s_slotNode[slot]);
    }
    s_slotNode[slot] = 0;
}

int net_uds_slot_is_connected(int slot)
{
    if (slot < 0 || slot >= NET_MAX_PLAYERS) return 0;
    if (!s_active) return 0;
    if (slot == 0) return 1;
    if (s_slotNode[slot] == 0) return 0;
    udsConnectionStatus st;
    if (R_FAILED(udsGetConnectionStatus(&st))) return 1;
    return (st.node_bitmask & (1u << (s_slotNode[slot] - 1))) ? 1 : 0;
}
