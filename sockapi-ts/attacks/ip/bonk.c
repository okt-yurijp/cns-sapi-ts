/* SPDX-License-Identifier: Apache-2.0 */
/* (c) Copyright 2004 - 2022 Xilinx, Inc. All rights reserved. */
/*
 * Test attacks/ip/bonk
 * Fragments with incorrect offsets
 */

/** @page ip-bonk  Fragments with oversized offsets
 *
 * @objective Emulate "bonk" and "Ping of Death" attacks.
 *
 * @reference CERT VU#104823 http://www.kb.cert.org/vuls/id/104823
 *
 * @param env          Testing environment:
 *                     - @ref arg_types_env_peer2peer
 * @param check_frags  Add fragments specification if @c TRUE.
 *
 * @par Scenario
 * -# Create stream and datagram connections between @p pco_iut and
 *    @p pco_tst.
 * -# Start the task on the @p pco_tst, which sends flood of UDP datagrams,
 *    TCP SYN, TCP data packets (corresponding to stream connection created
 *    on the step 1) or ICMP echo requests packets splitted to fragments,
 *    which after reassembling produce packets with length greater than
 *    65535 bytes.
 * -# Check that existing connections may be used to send/receive data.
 *
 * @author Elena Vengerova <Elena.Vengerova@oktetlabs.ru>
 */

#define TE_TEST_NAME  "attacks/ip/bonk"

#include "sockapi-test.h"

#ifdef HAVE_NET_ETHERNET_H
#include <net/ethernet.h>
#endif

#include "tapi_tad.h"
#include "tapi_ip4.h"
#include "tapi_tcp.h"
#include "tapi_cfg_base.h"
#include "tad_common.h"
#include "tapi_cfg.h"
#include "iomux.h"
#include "tapi_route_gw.h"


#define PKT_NUM      (1024 * 256)    /* Number of packets for flooding */
#define FRAG_LEN     512             /* Length of fragment payload */
#define IP_HDR_LEN   20              /* IP header length in bytes */
#define TCP_HDR_LEN  20              /* TCP header length in bytes */

/*
 * Length after reassembling. Here 0x1FFF is maximum value
 * which can be stored in 13bit fragment offset field of
 * IPv4 header and is a number of 8-byte blocks.
 */
#define PKT_LEN     (0x1FFF * 8 / FRAG_LEN + 1) * FRAG_LEN

/** Types of the payload */
typedef enum {
    PKT_UDP,
    PKT_TCP_SYN,
    PKT_TCP_DATA,
    PKT_ICMP
} pkt_type;

static const struct sockaddr *iut_addr;
static const struct sockaddr *tst_addr;
static const struct sockaddr *tst_fake_addr;
static tapi_tcp_handler_t     tcp_conn;

/**
 * Create template for packet with specified payload.
 *
 * @param type          payload type
 *
 * @return Template which may be sent to CSAP
 */
static asn_value *
create_template(pkt_type type, rcf_rpc_server *src, rcf_rpc_server *dst)
{
    char buf[64] = { 0, };
    int  n, i;
    int  rc;

    asn_value *result = NULL;

    tapi_ip_frag_spec *frags;
    tapi_ip_frag_spec *tmp;

    n = PKT_LEN / FRAG_LEN + 1 - !(PKT_LEN % FRAG_LEN);
    frags = (tapi_ip_frag_spec *)calloc(n, sizeof(*tmp));
    if (frags == NULL)
        TEST_FAIL("Out of memory");
    tapi_ip_frag_specs_init(frags, n);

    tmp = frags;

    switch (type)
    {
        case PKT_TCP_SYN:
        {
            uint16_t dst_port, src_port;

            CHECK_RC(tapi_allocate_port_htons(dst, &dst_port));
            CHECK_RC(tapi_allocate_port_htons(src, &src_port));

            CHECK_RC(tapi_tcp_make_msg(src_port, dst_port,
                                       0, 0, TRUE, FALSE,
                                       (uint8_t *)buf));
            break;
        }

        case PKT_TCP_DATA:
            CHECK_RC(tapi_tcp_make_msg(SIN(tst_fake_addr)->sin_port,
                                       SIN(iut_addr)->sin_port,
                                       tapi_tcp_next_seqn(tcp_conn),
                                       tapi_tcp_next_ackn(tcp_conn),
                                       FALSE, FALSE,
                                       (uint8_t *)buf));
             break;

        case PKT_UDP:
            *(uint16_t *)buf = SIN(tst_addr)->sin_port;
            *(uint16_t *)(buf + 2) = SIN(iut_addr)->sin_port;
            buf[4] = (PKT_LEN >> 8) & 0xFF;
            buf[5] = PKT_LEN & 0xFF;
            break;

        case PKT_ICMP:
            memset(buf, 0, sizeof(buf));
            buf[0] = 8; /* Echo */
            for (i = 4; i < (int)sizeof(buf); i++)
                buf[i] = i;
            *(uint16_t *)(buf + 2) = ~calculate_checksum(buf, sizeof(buf));
            break;
    }

    for (i = 0; i < n; i++)
    {
        tmp->hdr_offset = tmp->real_offset = i * FRAG_LEN;
        tmp->real_length =
            (i < n - 1) ? FRAG_LEN :
            (PKT_LEN % FRAG_LEN == 0) ? FRAG_LEN : PKT_LEN % FRAG_LEN;
        tmp->hdr_length = tmp->real_length + IP_HDR_LEN;

        tmp->more_frags = i < n - 1;
        tmp->dont_frag = FALSE;

        tmp++;
    }

    rc = tapi_ip4_template(frags, n, 1,
                           type == PKT_UDP ? IPPROTO_UDP :
                           type == PKT_ICMP ? IPPROTO_ICMP :
                                              IPPROTO_TCP,
                           (uint8_t *)buf, sizeof(buf), &result);
    free(frags);

    if (rc != 0)
        TEST_FAIL("tapi_ip4_template() failed; rc %r", rc);

    rc = tapi_tad_add_iterator_for(result, 1, PKT_NUM, 1);
    if (rc != 0)
        TEST_FAIL("tapi_tad_add_iterator_for() failed; rc %r", rc);

    return result;
}

int
main(int argc, char *argv[])
{
    rcf_rpc_server *pco_iut = NULL;
    rcf_rpc_server *pco_tst = NULL;

    const struct if_nameindex *iut_if;
    const struct if_nameindex *tst_if;

    struct sockaddr_in iut_addr1;
    struct sockaddr_in tst_addr1;

    const struct sockaddr *alien_link_addr = NULL;

    int iut_s_tcp = -1;
    int tst_s_tcp = -1;
    int iut_s_udp = -1;
    int tst_s_udp = -1;
    int iut_srv = -1;
    int iut_acc = -1;
    int num;
    int retries;

    uint8_t mac_iut[ETHER_ADDR_LEN];
    uint8_t mac_tst[ETHER_ADDR_LEN];

    char oid[RCF_MAX_ID];
    char tx_buf[FRAG_LEN * 2 - TCP_HDR_LEN];
    char rx_buf[FRAG_LEN * 2 - TCP_HDR_LEN];

    te_bool        check_frags;
    te_bool        again = FALSE;
    tapi_tcp_pos_t seqn;

    tapi_ip_frag_spec frags[] = {
        { 0, 0, FRAG_LEN + IP_HDR_LEN, FRAG_LEN, TRUE, FALSE, -1 },
        { FRAG_LEN, FRAG_LEN, FRAG_LEN + IP_HDR_LEN, FRAG_LEN,
          FALSE, FALSE, -1}
    };

    csap_handle_t csap_udp = CSAP_INVALID_HANDLE;
    csap_handle_t csap_tcp_syn = CSAP_INVALID_HANDLE;
    csap_handle_t csap_tcp_data = CSAP_INVALID_HANDLE;
    csap_handle_t csap_icmp = CSAP_INVALID_HANDLE;

    int sid_udp, sid_tcp_syn, sid_tcp_data, sid_icmp;

    asn_value *udp_pkt = NULL;
    asn_value *tcp_syn_pkt = NULL;
    asn_value *tcp_data_pkt = NULL;
    asn_value *icmp_pkt = NULL;

    TEST_START;

    TEST_GET_PCO(pco_iut);
    TEST_GET_PCO(pco_tst);
    TEST_GET_ADDR(pco_iut, iut_addr);
    TEST_GET_ADDR(pco_tst, tst_addr);
    TEST_GET_ADDR(pco_tst, tst_fake_addr);
    TEST_GET_IF(iut_if);
    TEST_GET_IF(tst_if);
    TEST_GET_BOOL_PARAM(check_frags);
    TEST_GET_LINK_ADDR(alien_link_addr);

    CHECK_RC(tapi_cfg_del_neigh_dynamic(pco_iut->ta, iut_if->if_name));

    CHECK_RC(tapi_update_arp(pco_iut->ta, iut_if->if_name, NULL, NULL,
                             tst_fake_addr, CVT_HW_ADDR(alien_link_addr),
                             TRUE));
    CFG_WAIT_CHANGES;

    sprintf(oid, "/agent:%s/interface:%s", pco_iut->ta, iut_if->if_name);
    if (tapi_cfg_base_if_get_mac(oid, mac_iut) != 0)
        TEST_STOP;

    sprintf(oid, "/agent:%s/interface:%s", pco_tst->ta, tst_if->if_name);
    if (tapi_cfg_base_if_get_mac(oid, mac_tst) != 0)
        TEST_STOP;

    iut_addr1 = *(struct sockaddr_in *)iut_addr;
    tst_addr1 = *(struct sockaddr_in *)tst_addr;
    TAPI_SET_NEW_PORT(pco_iut, &iut_addr1);
    TAPI_SET_NEW_PORT(pco_tst, &tst_addr1);

    GEN_CONNECTION(pco_iut, pco_tst, RPC_SOCK_DGRAM, RPC_IPPROTO_UDP,
                   iut_addr, tst_addr, &iut_s_udp, &tst_s_udp);

    GEN_CONNECTION(pco_iut, pco_tst, RPC_SOCK_STREAM, RPC_IPPROTO_TCP,
                   (struct sockaddr *)&iut_addr1,
                   (struct sockaddr *)&tst_addr1,
                   &iut_s_tcp, &tst_s_tcp);

    if (rcf_ta_create_session(pco_tst->ta, &sid_udp) != 0 ||
        rcf_ta_create_session(pco_tst->ta, &sid_tcp_syn) != 0 ||
        rcf_ta_create_session(pco_tst->ta, &sid_tcp_data) != 0 ||
        rcf_ta_create_session(pco_tst->ta, &sid_icmp) != 0)
    {
        TEST_FAIL("Failed to allocate RCF session");
    }

    /* Establish TCP connection */
    iut_srv = rpc_socket(pco_iut, RPC_PF_INET, RPC_SOCK_STREAM,
                         RPC_PROTO_DEF);
    rpc_bind(pco_iut, iut_srv, iut_addr);
    rpc_listen(pco_iut, iut_srv, SOCKTS_BACKLOG_DEF);
    CHECK_RC(tapi_tcp_init_connection(pco_tst->ta, TAPI_TCP_CLIENT,
                                      (struct sockaddr *)tst_fake_addr,
                                      (struct sockaddr *)iut_addr,
                                      tst_if->if_name,
                                      (const uint8_t *)alien_link_addr->sa_data,
                                      mac_iut,
                                      0, &tcp_conn));
    CHECK_RC(tapi_tcp_wait_open(tcp_conn, 10000));
    iut_acc = rpc_accept(pco_iut, iut_srv, NULL, NULL);

    /* Create CSAP for sending huge UDP packets */
    CHECK_RC(tapi_ip4_eth_csap_create(pco_tst->ta, sid_udp, tst_if->if_name,
                                      (TAD_ETH_RECV_DEF &
                                       ~TAD_ETH_RECV_OTHER) |
                                      TAD_ETH_RECV_NO_PROMISC,
                                      mac_tst, mac_iut,
                                      SIN(tst_addr)->sin_addr.s_addr,
                                      SIN(iut_addr)->sin_addr.s_addr,
                                      IPPROTO_UDP, &csap_udp));

    /* Create CSAP for sending huge TCP data packets */
    CHECK_RC(tapi_ip4_eth_csap_create(pco_tst->ta, sid_tcp_data,
                                      tst_if->if_name,
                                      (TAD_ETH_RECV_DEF &
                                       ~TAD_ETH_RECV_OTHER) |
                                      TAD_ETH_RECV_NO_PROMISC,
                                      (const uint8_t *)alien_link_addr->sa_data,
                                      mac_iut,
                                      SIN(tst_fake_addr)->sin_addr.s_addr,
                                      SIN(iut_addr)->sin_addr.s_addr,
                                      IPPROTO_TCP, &csap_tcp_data));

    /* Create CSAP for sending huge TCP SYN packets */
    CHECK_RC(tapi_ip4_eth_csap_create(pco_tst->ta, sid_tcp_syn,
                                      tst_if->if_name,
                                      (TAD_ETH_RECV_DEF &
                                       ~TAD_ETH_RECV_OTHER) |
                                      TAD_ETH_RECV_NO_PROMISC,
                                      mac_tst, mac_iut,
                                      SIN(tst_addr)->sin_addr.s_addr,
                                      SIN(iut_addr)->sin_addr.s_addr,
                                      IPPROTO_TCP, &csap_tcp_syn));

    /* Create CSAP for sending huge ICMP Echo packets */
    CHECK_RC(tapi_ip4_eth_csap_create(pco_tst->ta, sid_icmp,
                                      tst_if->if_name,
                                      (TAD_ETH_RECV_DEF &
                                       ~TAD_ETH_RECV_OTHER) |
                                      TAD_ETH_RECV_NO_PROMISC,
                                      mac_tst, mac_iut,
                                      SIN(tst_addr)->sin_addr.s_addr,
                                      SIN(iut_addr)->sin_addr.s_addr,
                                      IPPROTO_ICMP, &csap_icmp));

    udp_pkt = create_template(PKT_UDP, NULL, NULL);
    tcp_syn_pkt = create_template(PKT_TCP_SYN, pco_tst, pco_iut);
    tcp_data_pkt = create_template(PKT_TCP_DATA, NULL, NULL);
    icmp_pkt = create_template(PKT_ICMP, NULL, NULL);

    /* Start flooding on CSAPs */
    CHECK_RC(tapi_tad_trsend_start(pco_tst->ta, sid_udp, csap_udp, udp_pkt,
                                   RCF_MODE_NONBLOCKING));
    CHECK_RC(tapi_tad_trsend_start(pco_tst->ta, sid_tcp_syn,
                                   csap_tcp_syn, tcp_syn_pkt,
                                   RCF_MODE_NONBLOCKING));
    CHECK_RC(tapi_tad_trsend_start(pco_tst->ta, sid_tcp_data,
                                   csap_tcp_data, tcp_data_pkt,
                                   RCF_MODE_NONBLOCKING));
    CHECK_RC(tapi_tad_trsend_start(pco_tst->ta, sid_icmp,
                                   csap_icmp, icmp_pkt,
                                   RCF_MODE_NONBLOCKING));

    SLEEP(10);

    sockts_test_connection(pco_iut, iut_s_tcp, pco_tst, tst_s_tcp);
    sockts_test_connection(pco_iut, iut_s_udp, pco_tst, tst_s_udp);

    te_fill_buf(tx_buf, sizeof(tx_buf));
    seqn = tapi_tcp_next_seqn(tcp_conn);

    again:
    retries = 3;
    do {
        int iomux_rc;

        CHECK_RC(tapi_tcp_send_msg(tcp_conn, (uint8_t *)tx_buf,
                                   sizeof(tx_buf),
                                   TAPI_TCP_EXPLICIT, seqn,
                                   TAPI_TCP_AUTO, 0,
                                   check_frags ? frags : NULL,
                                   check_frags ? 2 : 0));
        memset(rx_buf, 0, sizeof(rx_buf));

        iomux_rc = iomux_call_default_simple(pco_iut, iut_acc, EVT_RD,
                                             NULL, 100);
        if (iomux_rc > 0)
            num = rpc_recv(pco_iut, iut_acc, rx_buf, sizeof(rx_buf), 0);
        else if (iomux_rc == 0)
            num = -1;
        else
            TEST_FAIL("iomux_call() failed");

        retries--;
    } while (num < 0 && retries > 0);

    if (retries == 0 && num < 0)
    {
        if (!check_frags || again)
            TEST_FAIL("retries to send message are over");

        WARN("Fragmented data are not received during bonk attack");
        /* Flush reassembling queue and try again */
        rcf_ta_trsend_stop(pco_tst->ta, sid_udp, csap_udp, &num);
        rcf_ta_trsend_stop(pco_tst->ta, sid_tcp_data, csap_tcp_data, &num);
        rcf_ta_trsend_stop(pco_tst->ta, sid_tcp_syn, csap_tcp_syn, &num);
        rcf_ta_trsend_stop(pco_tst->ta, sid_icmp, csap_icmp, &num);
        SLEEP(40);
        again = TRUE;
        goto again;
    }

    /* data was sent and acked. */
    tapi_tcp_update_sent_seq(tcp_conn, sizeof(tx_buf));

    if (num != sizeof(tx_buf))
        TEST_FAIL("Unexpected number of bytes is received");
    if (memcmp(rx_buf, tx_buf, sizeof(tx_buf)) != 0)
        TEST_FAIL("Data passed via flooded connection are corrupted");

    TEST_SUCCESS;

cleanup:
    CLEANUP_CHECK_RC(tapi_tad_csap_destroy(pco_tst->ta, 0, csap_udp));
    CLEANUP_CHECK_RC(tapi_tad_csap_destroy(pco_tst->ta, 0,
                                         csap_tcp_data));
    CLEANUP_CHECK_RC(tapi_tad_csap_destroy(pco_tst->ta, 0, csap_tcp_syn));
    CLEANUP_CHECK_RC(tapi_tad_csap_destroy(pco_tst->ta, 0, csap_icmp));

    asn_free_value(udp_pkt);
    asn_free_value(tcp_syn_pkt);
    asn_free_value(tcp_data_pkt);
    asn_free_value(icmp_pkt);

    if (tcp_conn != 0)
    {
        CLEANUP_CHECK_RC(tapi_tcp_send_fin(tcp_conn, 5000));
        CLEANUP_CHECK_RC(tapi_tcp_destroy_connection(tcp_conn));
    }

    CLEANUP_RPC_CLOSE(pco_iut, iut_s_udp);
    CLEANUP_RPC_CLOSE(pco_tst, tst_s_udp);
    CLEANUP_RPC_CLOSE(pco_iut, iut_s_tcp);
    CLEANUP_RPC_CLOSE(pco_tst, tst_s_tcp);
    CLEANUP_RPC_CLOSE(pco_iut, iut_srv);
    CLEANUP_RPC_CLOSE(pco_iut, iut_acc);

    CLEANUP_CHECK_RC(tapi_update_arp(pco_iut->ta, iut_if->if_name,
                                     pco_tst->ta, tst_if->if_name,
                                     tst_fake_addr, NULL, FALSE));
    CFG_WAIT_CHANGES;

    if (pco_iut != NULL && iut_if != NULL)
        CLEANUP_CHECK_RC(tapi_cfg_del_neigh_dynamic(pco_iut->ta,
                                                    iut_if->if_name));

    TEST_END;
}
