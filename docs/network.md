# Networking

SystrixOS implements a full network stack from scratch in `kernel/net.c` and `kernel/tcpip.c`. No external network libraries are used.

---

## Stack Layers

```
HTTP GET
   ↕
TCP (reliable stream, SYN/FIN handshake, retransmit)
   ↕
UDP (unreliable datagrams — used by DHCP)
   ↕
ICMP (ping / error messages)
   ↕
IPv4 (routing, checksum)
   ↕
ARP (IP ↔ MAC resolution)
   ↕
Ethernet II (framing)
   ↕
e1000 NIC driver (Intel 8254x, MMIO)
   ↕
QEMU virtual NIC (MAC 52:54:00:12:34:56)
```

---

## e1000 Driver (`kernel/e1000.c` + `kernel/net.c`)

The Intel 8254x (e1000) is the NIC exposed by QEMU by default. The driver:

1. **PCI discovery** — scans the PCI bus for vendor `0x8086` / device `0x100E`
2. **MMIO mapping** — maps the BAR0 register space (default `0xFEB80000` in QEMU)
3. **TX ring** — 16 descriptors, each pointing to a 2 KB packet buffer in physical memory
4. **RX ring** — 16 descriptors, each pre-loaded with a 2 KB buffer; the NIC writes received frames in and sets the DD (Descriptor Done) bit
5. **MAC read** — reads RAL0/RAH0 after a hardware reset to get the burned-in MAC

**Sending a packet:**
```c
net_send(buf, len);   // writes to next TX descriptor, bumps TDT
```

**Receiving a packet:**
```c
net_poll();           // checks RX ring for DD bit, dispatches to eth_recv()
```

**QEMU invocation (from Makefile):**
```
-netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56
```

---

## ARP

- Maintains a small ARP cache (`arp_cache[]`, 16 entries)
- `arp_request(ip)` — broadcasts a Who-has frame and blocks until a reply arrives
- `arp_reply()` — responds to incoming Who-has requests for our own IP

---

## IPv4 / ICMP

- `ip_send(proto, dst_ip, payload, len)` — fills IP header (TTL=64, no fragmentation), computes checksum
- ICMP echo request/reply implemented for `ping` shell command

---

## UDP

- Thin layer over IP; no port multiplexing beyond matching `dst_port` against a single registered listener
- Used only internally by DHCP

---

## DHCP

- Sends DHCPDISCOVER (broadcast), waits for DHCPOFFER, sends DHCPREQUEST, waits for DHCPACK
- Extracts: `yiaddr` (our IP), subnet mask, default gateway, DNS server
- Shell command: `dhcp`

---

## TCP (`kernel/tcpip.c`)

Implements enough TCP for a single outbound HTTP connection:

- Three-way handshake: SYN → SYN-ACK → ACK
- Data transfer with ACK tracking
- Connection teardown: FIN/ACK exchange
- Simple retransmit on timeout (no SACK, no window scaling)

---

## HTTP GET

`http_get(host, path, out_buf, max_len)` — opens a TCP connection to port 80, sends a raw HTTP/1.0 GET request, reads the response body into a buffer. Used by SystrixLynx.

---

## Shell Commands

| Command | What it does |
|---------|-------------|
| `dhcp` | Run DHCP to obtain an IP address |
| `ping <ip>` | Send ICMP echo request, print RTT |
| `http <url>` | Fetch a URL and print the body |

---

## Known Limitations

- Single NIC, single IP address, no routing table
- No IPv6
- TCP: no congestion control, no SACK, no window scaling
- DNS: resolver in `net.c` but may fail depending on QEMU's DNS proxy — see current debugging in progress
