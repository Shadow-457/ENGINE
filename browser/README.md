# browser/

SystrixLynx — a text-mode HTTP browser for SystrixOS.

> ⚠️ **Status: Not working**
>
> SystrixLynx compiles but **does not run** inside SystrixOS. The `elf LYNX` command loads the binary but it fails before rendering any output. The root cause is likely tied to the unresolved DNS / TCP issues in `kernel/net.c` — the browser depends on a working network stack for HTTP fetches, and DNS resolution is currently broken.
>
> See [`../docs/network.md`](../docs/network.md) for the current networking status.

---

## Files

| File | What it does |
|------|-------------|
| `lynx.c` | HTTP GET client, minimal HTML-to-text renderer, interactive pager |

---

## Build

The binary still builds cleanly:

```bash
make lynx        # compiles browser/lynx.c → LYNX
make addlynx     # embeds LYNX into the disk image
make run
# inside the shell — will load but not work:
elf LYNX
```

---

## What It's Supposed To Do

- Accept a URL as argument: `elf LYNX http://example.com/`
- Open a TCP connection via the kernel network stack
- Send an HTTP/1.0 GET request
- Strip HTML tags and reflow text to terminal width
- Let you scroll and follow links

Once the network stack is fixed (DNS + TCP), this should work without further changes to `lynx.c`.
