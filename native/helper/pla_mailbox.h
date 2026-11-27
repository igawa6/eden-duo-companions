// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Shared assembly/C++ offsets. Only request/ack sequences publish payloads.
#define PLA_REQ 0x00
#define PLA_ACK 0x04
#define PLA_OP 0x08
#define PLA_REQ_EPOCH 0x0c
#define PLA_CANCEL 0x10
#define PLA_MODE 0x14
#define PLA_ITEM 0x18
#define PLA_PARTY 0x1c
#define PLA_VIEW 0x20
#define PLA_RESULT 0x28
#define PLA_POST_MODE 0x2c
#define PLA_POST_ITEM 0x30
#define PLA_POST_PARTY 0x34
#define PLA_ITEMS 0x38
#define PLA_PARTIES 0x3c
#define PLA_EPOCH 4092
#define PLA_MAILBOX_SIZE 4096
#define PLA_NEXT 1
#define PLA_PREVIOUS 2
#define PLA_TOGGLE 3
#define PLA_DONE 0
#define PLA_BUSY 1
#define PLA_INVALID 2
