/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

/* Ordered, never-skipped input message stream. Each packet repeats the last few
 * messages; each message also goes on the reliable channel. */
#ifndef XENIA_KERNEL_NETPLAY_NET_INPUTS_H_
#define XENIA_KERNEL_NETPLAY_NET_INPUTS_H_

#include <stdint.h>

#define NET_INPUT_MSG 1280 /* max message */
#define NET_INPUT_REPEAT 4 /* messages per packet */
#define NET_INPUT_HELD 64  /* out-of-order queue */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t len;
  uint8_t data[NET_INPUT_MSG];
} NetInputMsg;

typedef struct {
  uint32_t next_send; /* next outgoing sequence */
  NetInputMsg sent[NET_INPUT_REPEAT];
  uint32_t next_recv; /* next deliverable sequence */
  uint8_t held_used[NET_INPUT_HELD];
  NetInputMsg held[NET_INPUT_HELD];
  uint32_t bad, repeats;
} NetInputs;

void net_inputs_init(NetInputs* n);
/* Builds fast/reliable packets. Returns length via pointers; 0 done, -1 error. */
int net_inputs_pack(NetInputs* n, const void* msg, int len, uint8_t* fast,
                    int fast_cap, int* fast_len, uint8_t* sure, int sure_cap,
                    int* sure_len);
/* Accept packet from either channel; bad packets counted/ignored. */
void net_inputs_on_packet(NetInputs* n, const uint8_t* packet, int len);
/* Whether packet's newest message fits. */
int net_inputs_fits(const NetInputs* n, const uint8_t* packet, int len);
/* Peek next in-order message. */
int net_inputs_peek(const NetInputs* n, void* buf, int cap);
/* Take next in-order message. */
int net_inputs_next(NetInputs* n, void* buf, int cap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // XENIA_KERNEL_NETPLAY_NET_INPUTS_H_
