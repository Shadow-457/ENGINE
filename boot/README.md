# boot/

MBR bootloader source.

| File | What it does |
|------|-------------|
| `boot.S` | 512-byte x86 MBR bootloader. Reads the kernel from disk, sets up A20 + paging, enters 64-bit long mode, jumps to `kernel/entry.S`. |

See [`../docs/boot.md`](../docs/boot.md) for the full boot sequence.

The build process strips `.note.gnu.property` from the linked ELF before extracting the flat binary — without this, binutils would produce 1064 bytes instead of 512.
