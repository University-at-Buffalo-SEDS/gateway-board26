import json
import unittest
from pathlib import Path

import build


class QualificationContractTests(unittest.TestCase):
    def test_gateway_transports_v4_topology_packets_and_recovers_can(self):
        root = Path(build.__file__).resolve().parent
        uart_h = (root / "Core" / "Inc" / "telemetry_uart.h").read_text(
            encoding="utf-8"
        )
        uart_c = (root / "Core" / "Src" / "telemetry_uart.c").read_text(
            encoding="utf-8"
        )
        can = (root / "Core" / "Src" / "can_bus.c").read_text(encoding="utf-8")

        self.assertIn("#define TELEMETRY_UART_MAX_PAYLOAD 1024U", uart_h)
        self.assertIn("#define TELEMETRY_UART_QUEUE_DEPTH 8U", uart_c)
        self.assertIn("#define TELEMETRY_UART_RX_RING_DEPTH 8U", uart_c)
        self.assertIn("can_bus_recover_if_bus_off", can)
        self.assertIn("can_bus_wait_for_tx_slot", can)
        self.assertIn("HAL_FDCAN_AbortTxRequest", can)

    def test_full_runner_profiles_memory_and_linked_network(self):
        root = Path(build.__file__).resolve().parent
        runner = (root / "sim" / "run_full.py").read_text(encoding="utf-8")
        script = (root / "build.py").read_text(encoding="utf-8")

        self.assertIn('"profile"', runner)
        self.assertIn('"--sample-count", "20"', runner)
        self.assertIn('"--traffic-iterations", "1000000"', runner)
        self.assertIn('"bay"', runner)
        self.assertIn('"tx_probe": "fdcan_tx_ok"', runner)
        self.assertIn('"rx_probe": "fdcan_rx"', runner)
        self.assertIn('"host_nodes"', runner)
        self.assertIn('"groundstation"', runner)
        self.assertIn('"rocket_radio"', runner)
        self.assertIn('"fill_pico"', runner)
        self.assertIn('"GS_SIM_VALIDATE_VALVE_ROUNDTRIP": "1"', runner)
        self.assertIn('"probe": "valve_commands_received", "minimum": 1', runner)
        self.assertIn("forwarded status ACK to GroundStation", runner)
        self.assertIn('simulation_env["SEDS_FIRMWARE_SIM_TEST"] = "1"', runner)
        self.assertIn('run_live(command, "firmware simulation")', runner)
        self.assertIn('running ({int(now - started)}s elapsed)', runner)
        self.assertIn("Long-duration memory profile", script)
        self.assertIn("Network discovery and time sync", script)

    def test_layout_exposes_network_convergence(self):
        root = Path(build.__file__).resolve().parent
        layout = json.loads((root / "sim" / "board.json").read_text(encoding="utf-8"))
        self.assertLess(layout["execution"].get("memory_probe_warmup_samples", 0), layout["execution"]["sample_count"])
        probes = {
            probe["name"]: probe["symbol"]
            for probe in layout["execution"]["memory_probes"]
        }
        self.assertEqual(probes["network_ready"], "g_telemetry_network_ready")
        self.assertEqual(probes["discovery_seen"], "g_telemetry_discovery_seen")
        self.assertEqual(probes["timesync_valid"], "g_telemetry_timesync_valid")

        telemetry = (root / "Core" / "Src" / "telemetry.c").read_text(encoding="utf-8")
        for symbol in (
            "g_telemetry_network_ready",
            "g_telemetry_discovery_seen",
            "g_telemetry_timesync_valid",
        ):
            self.assertIn(symbol, telemetry)

    def test_shared_can_avoids_hop_retry_storms(self):
        root = Path(build.__file__).resolve().parent
        telemetry = (root / "Core" / "Src" / "telemetry.c").read_text(encoding="utf-8")
        cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('seds_router_add_side_packed(r, "can", 3U, tx_send, NULL, false)', telemetry)
        self.assertIn('SEDSNET_MAX_QUEUE_BUDGET "8192"', cmake)

    def test_physical_bridge_preserves_the_packed_wire_image(self):
        root = Path(build.__file__).resolve().parent
        telemetry = (root / "Core" / "Src" / "telemetry.c").read_text(
            encoding="utf-8"
        )

        uart_ingress = telemetry.index("void telemetry_uart_handle_data")
        uart_ingest = telemetry.index(
            "seds_router_receive_packed_from_side(", uart_ingress
        )
        self.assertGreater(uart_ingest, uart_ingress)

        can_ingress = telemetry.index("static void telemetry_can_rx")
        local_can_ingest = telemetry.index("rx_asynchronous(data, len);", can_ingress)
        self.assertGreater(local_can_ingest, can_ingress)

        # Source-side routing preserves the packed reliability envelope while
        # preventing a frame from being reflected to its ingress transport.
        bridge = telemetry[uart_ingress: telemetry.index("static uint32_t telemetry_timesync_role")]
        self.assertNotIn("seds_pkt_pack", bridge)
        self.assertIn("seds_router_new(Seds_RM_Relay", telemetry)
        self.assertIn('seds_router_add_side_packed(r, "can"', telemetry)
        self.assertIn('seds_router_add_side_packed(r, "uart"', telemetry)
        self.assertNotIn("tx_send(payload, len, NULL)", telemetry)
        self.assertNotIn("telemetry_uart_tx_send(data, len, NULL)", telemetry)
        self.assertNotIn("BridgeEchoMarker", telemetry)


    def test_periodic_health_check_does_not_serialize_topology(self):
        root = Path(build.__file__).resolve().parent
        telemetry = (root / "Core" / "Src" / "telemetry.c").read_text(encoding="utf-8")
        self.assertNotIn("seds_router_export_topology_len", telemetry)
        self.assertIn("g_telemetry_discovery_seen = 1U", telemetry)

if __name__ == "__main__":
    unittest.main()
