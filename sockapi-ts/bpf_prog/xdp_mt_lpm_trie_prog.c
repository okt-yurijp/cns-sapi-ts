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

struct bpf_map SEC("maps") map_lpm_trie = {
    .type = BPF_MAP_TYPE_LPM_TRIE,
    .key_size = sizeof(__u64),
    .value_size = sizeof(__u32),
    .max_entries = 1,
    .map_flags = 1U << 0
};

SEC("prog")
int xdp_prog(struct xdp_md *ctx)
{
    return XDP_PASS;
}
