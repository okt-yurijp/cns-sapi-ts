/* SPDX-License-Identifier: Apache-2.0 */
/* (c) Copyright 2004 - 2022 Xilinx, Inc. All rights reserved. */
/*
 * Socket API Test Suite
 * Bad Parameters and Boundary Values
 */

/** @page bnbvalue-func_pipe_sock_recv Using socket receiving data functions with pipe
 *
 * @objective Check that it is not possible to use socket receiving functions
 *            with pipe
 *
 * @type conformance, robustness
 *
 * @param pco_iut       PCO on IUT
 * @param func          Receiving function to be tested
 *
 * @par Scenario:
 *  -# Create a pipe.
 *  -# Write some data to the write end of pipe
 *  -# Call @p func on its read end.
 *  -# If @p func successeed, check that received data
 *     is correct.
 *
 * @author Dmitry Izbitsky <Dmitry.Izbitsky@oktetlabs.ru>
 */

#define TE_TEST_NAME  "bnbvalue/pipe_sock_recv"

#include "sockapi-test.h"

#define BUF_SIZE 1024

int
main(int argc, char *argv[])
{
    rcf_rpc_server *pco_iut = NULL;

    int         pipefds[2] = { -1, -1 };
    rpc_recv_f  func;
    char       *tx_buf;
    char       *rx_buf;
    int         sent;
    int         received;
    te_bool     is_failed = FALSE;

    TEST_START;
    TEST_GET_PCO(pco_iut);
    TEST_GET_RECV_FUNC(func);

    CHECK_NOT_NULL(tx_buf = te_make_buf_by_len(BUF_SIZE));
    CHECK_NOT_NULL(rx_buf = te_make_buf_by_len(BUF_SIZE));

    rpc_pipe(pco_iut, pipefds);

    sent = rpc_write(pco_iut, pipefds[1], tx_buf, BUF_SIZE);
    if (sent != BUF_SIZE)
        TEST_VERDICT("Failed to write all the data to a pipe");

    pco_iut->op = RCF_RPC_CALL;
    func(pco_iut, pipefds[0], rx_buf, BUF_SIZE, 0);

    SLEEP(1);
    if (!rcf_rpc_server_is_alive(pco_iut))
    {
        pipefds[0] = -1;
        pipefds[1] = -1;
        rcf_rpc_server_restart(pco_iut);
        TEST_VERDICT("RPC server is dead as a result of %s() "
                     "call on the read end of pipe",
                     rpc_recv_func_name(func));
    }

    pco_iut->op = RCF_RPC_WAIT;
    RPC_AWAIT_ERROR(pco_iut);
    received = func(pco_iut, pipefds[0], rx_buf, BUF_SIZE, 0);
    if (received >= 0)
    {
        ERROR_VERDICT("%s() successeed on the read end of pipe",
                     rpc_recv_func_name(func));
        is_failed = TRUE;
        if (received == 0)
            RING_VERDICT("No data was read");
        else if (received != BUF_SIZE)
            TEST_VERDICT("%s than expected was read",
                         sent < BUF_SIZE ? "less" : "greater");
        else if (memcmp(tx_buf, rx_buf, BUF_SIZE) != 0)
            TEST_VERDICT("Incorrect data was read");
    }
    else if (RPC_ERRNO(pco_iut) != RPC_ENOTSOCK)
    {
        RING_VERDICT("%s() on the read end of pipe failed "
                     "with unexpected errno " RPC_ERROR_FMT,
                     rpc_recv_func_name(func),
                     RPC_ERROR_ARGS(pco_iut));
    }

    if (is_failed)
        TEST_STOP;
    TEST_SUCCESS;

cleanup:

    CLEANUP_RPC_CLOSE(pco_iut, pipefds[0]);
    CLEANUP_RPC_CLOSE(pco_iut, pipefds[1]);
    free(rx_buf);
    free(tx_buf);

    TEST_END;
}
