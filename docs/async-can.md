# Asynchronous gateway CAN transmit

`can_bus_send_large()` copies one complete SEDSnet side-transport frame (up to
128 bytes) into an owned queue and returns immediately. `HAL_OK` means accepted
locally, not CAN-acknowledged or executed remotely. Existing SEDSnet command
acknowledgements remain the end-to-end confirmation.

The queue is capped at 6 KiB including conservative per-node allocation charges,
and 48 pending packets. Foreground allocation is fallible and preserves at least
4 KiB of router/control headroom. Full queues refuse the new packet with
`HAL_BUSY`; the SEDSnet callback reports transport failure, as it did for failed
synchronous sends. Best-effort data may be lost on refusal; this is not a new
promise of reliable delivery for every data type.

The TX-FIFO-empty interrupt feeds up to three hardware slots per invocation.
Packets retain their fragment sequence and offset across FIFO-full/HAL-error
conditions. No allocation, free, spin-wait, router call or subscriber callback
occurs in this interrupt. Foreground RX servicing reaps retired buffers and also
pumps TX, so a missed empty notification cannot leave the queue stuck. HAL copies
submitted frames into controller message RAM before a source buffer is retired.

Packets older than one second are discarded and counted, including incomplete
fragments; they are not replayed late as stale commands. A bus-off recovery can
still lose frames already accepted into controller RAM, and remote delivery
still requires command ACKs/retries. Reinitialization discards the local queue.

Counters distinguish pending packets, charged bytes, high-water occupancy,
refused submissions, expired packets and packets fully submitted to hardware.
`g_can_tx_submitted` is not a wire ACK or command-success counter.

Tests cover source-buffer reuse, fragment contents/order, HAL busy/error retries,
queue saturation without leaks, ISR allocation/free prohibition, age expiry and
timer wrap, reset, and sequence wrap. The same wire header is retained. Hardware
soak testing must still verify TX interrupt routing, bus-off behavior, loadcell
loss rates and command ACK latency.
