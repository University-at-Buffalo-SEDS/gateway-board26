#pragma once
#include "tx_api.h"

/* ------ Telemetry Thread ------ */
extern TX_THREAD telemetry_thread;
extern TX_THREAD router_test_thread;

void telemetry_thread_entry(ULONG initial_input);
UINT create_telemetry_thread(TX_BYTE_POOL *byte_pool);
void router_test_thread_entry(ULONG initial_input);
UINT create_router_test_thread(TX_BYTE_POOL *byte_pool);
/* ------ Telemetry Thread ------ */

/* ------ Data Acquisition Thread ------ */
extern TX_THREAD data_acq_thread;

void data_acq_thread_entry(ULONG initial_input);
UINT create_data_acq_thread(TX_BYTE_POOL *byte_pool);
/* ------ Data Acquisition Thread ------ */
