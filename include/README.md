# include/

Shared kernel headers. Every kernel `.c` file includes `kernel.h`.

| File | Contents |
|------|---------|
| `kernel.h` | All kernel type definitions, struct declarations, and function prototypes. The single source of truth for the kernel ABI. |
| `font8x8.h` | 8×8 pixel bitmap font data (256 glyphs, one `uint8_t[8]` per character). Used by `gfx.c` to draw text on the framebuffer. |
