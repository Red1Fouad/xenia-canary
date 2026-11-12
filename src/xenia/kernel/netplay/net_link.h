/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

/* Reliable + unreliable link over any datagram carrier. No sockets or clock.
 * Reliable: once, in order, boundaries kept. Unreliable: at most once.
 * Handshake checks content digest. */
#ifndef XENIA_KERNEL_NETPLAY_NET_LINK_H_
#define XENIA_KERNEL_NETPLAY_NET_LINK_H_

#include <stdint.h>

#define NET_MAX_MSG 1400 /* max message, stays under 1500 MTU */
#define NET_HEADER 24
#define NET_MAX_PACKET (NET_MAX_MSG + NET_HEADER)
#define NET_WINDOW 64 /* reliable in-flight and reorder window */
#define NET_DGRAM_QUEUE 32
#define NET_DIGEST 32
/* Bump on wire format change. */
#define NET_VERSION 2
#define NET_TIMEOUT_MS 8000 /* silence limit, peer gone */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  NET_IDLE,
  NET_LISTENING,  /* host waiting */
  NET_CONNECTING, /* joiner saying hello */
  NET_ESTABLISHED,
  NET_CLOSED, /* peer left */
  NET_TIMED_OUT,
  NET_REFUSED /* see net_link_status */
} NetState;

typedef enum {
  NET_OK = 0,
  NET_REFUSED_VERSION,
  NET_REFUSED_CONTENT /* content mismatch */
} NetStatus;

typedef void (*NetCarrier)(void* ctx, const uint8_t* packet, int len);

typedef struct {
  uint32_t sent, received, retransmits, duplicates, bad, dropped_dgrams;
  uint32_t rtt_ms; /* smoothed RTT, 0 until measured */
} NetStats;

typedef struct {
  uint8_t used, retries, skipped;
  uint16_t seq, len;
  uint32_t sent_ms, first_ms, rto;
  uint8_t data[NET_MAX_MSG];
} NetSlot;

typedef struct {
  uint8_t used;
  uint16_t len;
  uint8_t data[NET_MAX_MSG];
} NetHeld;

typedef struct {
  NetState state;
  NetStatus status;
  NetCarrier carrier;
  void* ctx;
  uint8_t digest[NET_DIGEST];
  uint32_t nonce, peer_nonce, token;
  uint32_t last_rx, last_tx, last_ping, last_hello;
  uint16_t next_rel, read_rel, next_dgram, ping_id;
  uint16_t dgram_top;
  uint64_t dgram_seen;
  int dgram_any;
  uint32_t ping_sent;
  uint32_t srtt, rttvar;
  NetSlot out[NET_WINDOW];
  NetHeld in[NET_WINDOW];
  NetHeld dgrams[NET_DGRAM_QUEUE];
  int dgram_head, dgram_count;
  NetStats stats;
} NetLink;

/* nonce: random per-session value */
void net_link_init(NetLink* l, NetCarrier carrier, void* ctx,
                   const uint8_t digest[NET_DIGEST], uint32_t nonce);
void net_link_listen(NetLink* l, uint32_t now_ms);
void net_link_connect(NetLink* l, uint32_t now_ms);
void net_link_close(NetLink* l); /* notify once, then closed */

/* Packet from carrier. */
void net_link_on_packet(NetLink* l, const uint8_t* packet, int len,
                        uint32_t now_ms);
/* Retries/keep-alives/timeout; call at least every 10 ms. */
void net_link_tick(NetLink* l, uint32_t now_ms);

/* 0 sent/queued; -1 retry later (window full or link down). */
int net_link_send(NetLink* l, int reliable, const void* data, int len,
                  uint32_t now_ms);
/* Next message length, 0 if none; *reliable says channel. */
int net_link_recv(NetLink* l, int* reliable, void* buf, int cap);

NetState net_link_state(const NetLink* l);
NetStatus net_link_status(const NetLink* l);
const NetStats* net_link_stats(const NetLink* l);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // XENIA_KERNEL_NETPLAY_NET_LINK_H_
