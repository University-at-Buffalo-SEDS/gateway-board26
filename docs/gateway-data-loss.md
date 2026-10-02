# Gateway receive-loss investigation (2026-10-02)

No hardware was available for this change. The saved 2026-10-01 capture is
baseline evidence, not a measurement of the updated firmware.

Over 601 seconds the previous capture recorded 293,290 incoming CAN frames and
21,907 receive-ring drops (7.47%). Only 45 of those drops were the deliberate
loadcell bulk-admission policy. Allocator failures and panics remained zero.
The maximum recorded CAN service slice was 59 ms despite a 2 ms target; a
single synchronous callback can overrun that target. UART processing reached
30 ms. These observations point to servicing stalls/bursts, not just a small
receive ring. The capture does not isolate each stall's cause.

## Changes

- TLSF admission no longer walks all heap blocks with interrupts disabled for
  ordinary packets. It uses conservative constant-time occupancy accounting,
  preserves the existing ACK/dispatch reserve, and checks the requested largest
  allocation with a temporary aligned TLSF allocation immediately released.
  Heap snapshots remain diagnostic/startup/failure operations. The new snapshot
  counter allows the next soak to verify that they are not on the hot path.
- UART receive servicing has a 512-byte / 1-ms budget per call. A partially
  consumed DMA chunk remains owned by the RX ring and resumes at its saved byte
  offset. UART TX flushing and RX restart still run after the budget expires.
  This prevents continuous UART traffic from keeping the thread away from CAN.
  A single frame handler can still exceed the time budget.
- Small UART sends no longer clear an entire 1028-byte slot with IRQs masked;
  only the initialized header and actual payload are transmitted.

## Verification and remaining limits

The gateway host suite includes sanitizer-backed allocator churn (100,000
allocation/free iterations with admission checks), partial-chunk/timer-wrap
UART tests, permanent-ingress fairness, full-ring behavior, and existing UART
DMA ownership/retry tests. Build the firmware with TLSF enabled to exercise the
allocator change; UART changes apply to both allocator choices.

The synchronous CAN transmit-slot wait remains a possible source of stalls.
Returning failure early is not a safe substitute: the current callback error
path does not guarantee retention of all best-effort packets, and partial
multi-frame sends can increase loss. That path needs transport-level retry
validation before changing its behavior.

For the next hardware test, compare deltas of CAN received/ring-dropped/hardware
lost counters, UART ring drops/queue refusals, allocator failures/admission
refusals/snapshot count, and service maxima under the same traffic. Verify
command acknowledgements alongside loadcell sample counts. No zero-loss claim
is made from host tests alone. The UART baud setting must still match the Pico.
