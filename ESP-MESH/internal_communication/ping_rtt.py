"""
ping_rtt.py
-----------
Measures Round-Trip Time (RTT) across the ESP-MESH network.

PC sends PING over UART to a non-root node.
Node forwards PING to Root over mesh.
Root replies PONG back to the node over mesh.
Node writes PONG + mesh RTT back to PC over UART.
PC measures total wall-clock RTT.

Protocol (PC -> Node, UART):
    PING:<seq_id>\n

Protocol (Node -> PC, UART):
    PONG:<seq_id>:<mesh_rtt_ms>\n   -- success
    PONG_TIMEOUT:<seq_id>\n         -- root did not reply in time

Output:
    Seq   Total RTT   Mesh RTT   UART overhead
    ---   ---------   --------   -------------
      1     312 ms     298 ms          14 ms
      2     301 ms     289 ms          12 ms
    ...

    --- Summary (10 pings) ---
    Min   total: 295ms   mesh: 281ms
    Max   total: 412ms   mesh: 398ms
    Avg   total: 318ms   mesh: 304ms
    Loss  : 0/10

Firmware additions required in mesh_main.c:
    See comments at bottom of this file.

Usage:
    python ping_rtt.py <COM_PORT> [count] [interval_s]
    python ping_rtt.py COM4               # 10 pings, 2s interval
    python ping_rtt.py COM4 20            # 20 pings, 2s interval
    python ping_rtt.py COM4 20 1          # 20 pings, 1s interval
"""

import sys
import time
import serial
import statistics

BAUD_RATE       = 115200
PING_TIMEOUT_S  = 10       # per-ping timeout (root reply + UART back)
DEFAULT_COUNT   = 10
DEFAULT_INTERVAL= 2.0      # seconds between pings
WAIT_SECS       = 15       # stabilisation wait before first ping

# ----------------------------------------------------------------
#  Colour helpers (Windows + Unix compatible)
# ----------------------------------------------------------------
try:
    import colorama
    colorama.init()
    GREEN  = "\033[92m"
    YELLOW = "\033[93m"
    RED    = "\033[91m"
    CYAN   = "\033[96m"
    RESET  = "\033[0m"
    BOLD   = "\033[1m"
except ImportError:
    GREEN = YELLOW = RED = CYAN = RESET = BOLD = ""


def colour_rtt(rtt_ms):
    """Colour-code RTT value: green < 500ms, yellow < 1000ms, red >= 1000ms."""
    if rtt_ms < 500:
        return f"{GREEN}{rtt_ms:>6} ms{RESET}"
    elif rtt_ms < 1000:
        return f"{YELLOW}{rtt_ms:>6} ms{RESET}"
    else:
        return f"{RED}{rtt_ms:>6} ms{RESET}"


def ping_rtt(port, count=DEFAULT_COUNT, interval=DEFAULT_INTERVAL):
    print(f"\n{BOLD}ESP-MESH RTT Ping{RESET}")
    print(f"  Port     : {port}")
    print(f"  Count    : {count}")
    print(f"  Interval : {interval}s")
    print(f"  Timeout  : {PING_TIMEOUT_S}s per ping\n")

    ser = serial.Serial()
    ser.port     = port
    ser.baudrate = BAUD_RATE
    ser.timeout  = 1        # readline timeout
    ser.open()
    ser.dtr = False
    ser.rts = False

    # Flush any stale bytes (boot logs, previous output)
    time.sleep(0.5)
    ser.reset_input_buffer()

    # Wait for ESP32 mesh to stabilise before sending first ping
    print(f"Waiting {WAIT_SECS}s for mesh to stabilise...")
    for i in range(WAIT_SECS, 0, -1):
        print(f"  {i}s...", end='\r')
        time.sleep(1)
    print("Ready.          ")
    ser.reset_input_buffer()   # flush any output received during wait

    total_rtts = []     # wall-clock RTT (ms)
    mesh_rtts  = []     # mesh-only RTT (ms) reported by node firmware
    lost       = 0

    print(f"{'Seq':>4}  {'Total RTT':>10}  {'Mesh RTT':>10}  {'UART overhead':>14}")
    print(f"{'---':>4}  {'---------':>10}  {'--------':>10}  {'-------------':>14}")

    try:
        for seq in range(1, count + 1):
            ping_line = f"PING:{seq}\n"

            # Timestamp right before write
            t_send = time.time()
            ser.write(ping_line.encode('utf-8'))
            ser.flush()

            # Wait for PONG:<seq>:<mesh_rtt_ms> or PONG_TIMEOUT:<seq>
            deadline = time.time() + PING_TIMEOUT_S
            got_reply = False

            while time.time() < deadline:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                if not line:
                    continue

                if line.startswith(f"PONG:{seq}:"):
                    t_recv = time.time()
                    total_rtt_ms = int((t_recv - t_send) * 1000)

                    # Parse mesh RTT from node firmware
                    parts = line.split(':')
                    try:
                        mesh_rtt_ms = int(parts[2])
                    except (IndexError, ValueError):
                        mesh_rtt_ms = -1

                    uart_overhead = total_rtt_ms - mesh_rtt_ms if mesh_rtt_ms >= 0 else -1

                    total_rtts.append(total_rtt_ms)
                    if mesh_rtt_ms >= 0:
                        mesh_rtts.append(mesh_rtt_ms)

                    overhead_str = (f"{uart_overhead:>12} ms"
                                    if uart_overhead >= 0 else f"{'N/A':>12}")

                    print(f"{seq:>4}  {colour_rtt(total_rtt_ms)}  "
                          f"{colour_rtt(mesh_rtt_ms) if mesh_rtt_ms >= 0 else 'N/A':>10}  "
                          f"{overhead_str}")
                    got_reply = True
                    break

                elif line.startswith(f"PONG_TIMEOUT:{seq}"):
                    print(f"{seq:>4}  {RED}  TIMEOUT — root did not reply{RESET}")
                    lost += 1
                    got_reply = True
                    break

                # Print any other unsolicited node output (ESP_LOG lines, etc.)
                # but only if it doesn't look like internal noise
                elif not line.startswith("I (") and not line.startswith("W ("):
                    print(f"       {CYAN}[node]{RESET} {line}")

            if not got_reply:
                print(f"{seq:>4}  {RED}  NO RESPONSE — UART timeout ({PING_TIMEOUT_S}s){RESET}")
                lost += 1

            # Wait remainder of interval before next ping
            elapsed = time.time() - t_send
            wait = interval - elapsed
            if wait > 0 and seq < count:
                time.sleep(wait)

    except serial.SerialException as e:
        print(f"\n{RED}Serial error: {e}{RESET}")
        print("Ensure idf.py monitor is not open on this port.")
    except KeyboardInterrupt:
        print(f"\n{YELLOW}Interrupted by user.{RESET}")
    finally:
        ser.close()

    # ---- Summary ----
    received = len(total_rtts)
    print(f"\n{BOLD}--- Summary ({count} pings) ---{RESET}")

    if received > 0:
        print(f"  {'Min':<6} total: {min(total_rtts):>5}ms"
              + (f"   mesh: {min(mesh_rtts):>5}ms" if mesh_rtts else ""))
        print(f"  {'Max':<6} total: {max(total_rtts):>5}ms"
              + (f"   mesh: {max(mesh_rtts):>5}ms" if mesh_rtts else ""))
        avg_total = int(statistics.mean(total_rtts))
        print(f"  {'Avg':<6} total: {avg_total:>5}ms"
              + (f"   mesh: {int(statistics.mean(mesh_rtts)):>5}ms" if mesh_rtts else ""))
        if received > 1:
            jitter = int(statistics.stdev(total_rtts))
            print(f"  {'Jitter':<6}        {jitter:>5}ms  (std dev)")
    else:
        print(f"  {RED}No replies received.{RESET}")

    loss_pct = (lost / count) * 100
    loss_colour = GREEN if lost == 0 else (YELLOW if lost < count // 2 else RED)
    print(f"  Loss   : {loss_colour}{lost}/{count} ({loss_pct:.0f}%){RESET}\n")


# ================================================================
#  Entry point
# ================================================================
if __name__ == '__main__':
    if len(sys.argv) not in (2, 3, 4):
        print("Usage: python ping_rtt.py <COM_PORT> [count] [interval_s]")
        print("  python ping_rtt.py COM4")
        print("  python ping_rtt.py COM4 20")
        print("  python ping_rtt.py COM4 20 1")
        sys.exit(1)

    port_arg     = sys.argv[1]
    count_arg    = int(sys.argv[2])   if len(sys.argv) >= 3 else DEFAULT_COUNT
    interval_arg = float(sys.argv[3]) if len(sys.argv) == 4 else DEFAULT_INTERVAL

    ping_rtt(port_arg, count_arg, interval_arg)


# ================================================================
#  FIRMWARE ADDITIONS REQUIRED  (mesh_main.c)
# ================================================================
#
# 1. Add prefix defines alongside existing ones:
#
#    #define PING_MSG_PREFIX  "PING_MSG:"
#    #define PONG_MSG_PREFIX  "PONG_MSG:"
#
# ----------------------------------------------------------------
# 2. In uart_recv_task — detect PING:<seq> from PC and forward
#    to root, then listen for PONG_MSG reply and write back:
#
#    } else if (strncmp(line, "PING:", 5) == 0) {
#        int seq = atoi(line + 5);
#        char ping_mesh[32];
#        snprintf(ping_mesh, sizeof(ping_mesh), "PING_MSG:%d", seq);
#
#        TickType_t t_send = xTaskGetTickCount();
#        mesh_send_to_root(ping_mesh);
#
#        // Wait up to 8s for PONG_MSG back from root
#        static uint8_t pong_buf[64];
#        mesh_addr_t    pong_from;
#        mesh_data_t    pong_data;
#        int            pong_flag = 0;
#        pong_data.data = pong_buf;
#        pong_data.size = sizeof(pong_buf) - 1;
#
#        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(8000);
#        bool got_pong = false;
#        while (xTaskGetTickCount() < deadline) {
#            TickType_t remaining = deadline - xTaskGetTickCount();
#            if (esp_mesh_recv(&pong_from, &pong_data, remaining,
#                              &pong_flag, NULL, 0) != ESP_OK) continue;
#            pong_buf[pong_data.size] = '\0';
#            char expected[32];
#            snprintf(expected, sizeof(expected), "PONG_MSG:%d", seq);
#            if (strncmp((char *)pong_buf, expected, strlen(expected)) == 0) {
#                uint32_t mesh_rtt_ms =
#                    (xTaskGetTickCount() - t_send) * portTICK_PERIOD_MS;
#                char uart_reply[48];
#                int rlen = snprintf(uart_reply, sizeof(uart_reply),
#                                    "PONG:%d:%"PRIu32"\n", seq, mesh_rtt_ms);
#                uart_write_bytes(UART_NUM_0, uart_reply, rlen);
#                got_pong = true;
#                break;
#            }
#        }
#        if (!got_pong) {
#            char timeout_reply[32];
#            int rlen = snprintf(timeout_reply, sizeof(timeout_reply),
#                                "PONG_TIMEOUT:%d\n", seq);
#            uart_write_bytes(UART_NUM_0, timeout_reply, rlen);
#        }
#    }
#
# ----------------------------------------------------------------
# 3. In mesh_recv_task on ROOT — detect PING_MSG and reply PONG_MSG:
#
#    } else if (strncmp(msg, PING_MSG_PREFIX, strlen(PING_MSG_PREFIX)) == 0) {
#        const char *seq_str = msg + strlen(PING_MSG_PREFIX);
#        char pong[32];
#        snprintf(pong, sizeof(pong), "PONG_MSG:%s", seq_str);
#        mesh_send_to_addr(&from, pong);
#        ESP_LOGI(TAG, "PING from " MACSTR " seq=%s → PONG sent",
#                 MAC2STR(from.addr), seq_str);
#    }
#
# ================================================================