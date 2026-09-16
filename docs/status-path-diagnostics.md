# Gateway status-path diagnostics

The four entries in g_gateway_status_path are CAN receive, UART queue accepted,
UART HAL transmit success, and UART HAL transmit failure, respectively.
Each contains frames, unknown, status, valve_status, actuator_status, last_tick.
last_tick uses ThreadX local ticks, not network time.

The observer uses 488 bytes of static RAM and never changes routing.
It classifies raw packets and full/template compact SDT forms (kinds 1, 2, 4, 5).
Chunks, missing templates, and malformed prefixes count as unknown. Therefore
zero classified statuses alone does not establish a loss if unknown advances.
The two 16-entry diagnostic dictionaries are independent of SEDSNet dictionaries.
CAN is a shared bus: template-ID collisions can make compact source attribution
ambiguous. Use self-describing full-frame evidence to resolve such cases.

UART success only proves HAL completed transmission, not Pico-Fi reception or
GroundStation acknowledgement. Queue acceptance is likewise not delivery.
Compare counter deltas and GroundStation logs during the same test interval.
The observer does not validate CRCs and does not replace the network decoder.

Run ./build.py test to exercise the compiled observer. No simulator mode or
transport configuration override is required to expose these debugger symbols.
