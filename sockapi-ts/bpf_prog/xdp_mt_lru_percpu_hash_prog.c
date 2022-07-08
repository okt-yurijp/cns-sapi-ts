/* SPDX-License-Identifier: Apache-2.0 */
/* (c) Copyright 2004 - 2022 Xilinx, Inc. All rights reserved. */
/*
 * Socket API Test Suite
 * XDP programs
 *
 * @author Artemii Morozov <Artemii.Morozov@oktetlabs.ru>
 */

#include <linux/bpf.h>
#include "bpf_helpers.h"

struct bpf_map SEC("maps") map_lru_percpu_h = {
    .type = BPF_MAP_TYPE_LRU_PERCPU_HASH,
    .key_size = sizeof(__u32),
    .value_size = sizeof(__u32),
    .max_entries = 1
};

SEC("prog")
int xdp_prog(struct xdp_md *ctx)
{
    return XDP_PASS;
}
