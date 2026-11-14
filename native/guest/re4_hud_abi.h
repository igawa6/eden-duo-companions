// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
// Reserved format-2 mailbox. Guest owns heartbeat/ack; host owns request fields.
typedef struct Re4HudMailbox {
    uint32_t heartbeat, epoch;
    uint32_t seq, request_epoch, deadline, mask;
    uint32_t ack, applied_mask, error;
    uint32_t saved_bits[2], saved_epoch;
    uint64_t saved_manager, saved_nodes[2];
} Re4HudMailbox;
static inline int re4_hud_lease_live(uint32_t now,uint32_t until) {
    return (int32_t)(until-now)>=0 && until-now<=90;
}
