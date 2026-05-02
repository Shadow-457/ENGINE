# GUI & Graphics

> ⚠️ **Status: Not working**
>
> The GUI subsystem compiles and is linked into the kernel, but **does not run correctly** in QEMU. The framebuffer initialises (bochs-display is detected) but the compositor either crashes or produces a blank screen. This is a known issue and is actively being debugged.
>
> Use the VGA text-mode shell (`make run`) in the meantime — it is fully functional.

---

## What It's Supposed To Do

SystrixOS has a software-rendered GUI built in the kernel using a linear framebuffer from QEMU's `bochs-display` device. The code exists and compiles, but doesn't render correctly at runtime.

---

## Framebuffer Device (`kernel/fbdev.c`)

QEMU is launched with:
```
-device bochs-display,xres=1024,yres=768
```

`fbdev.c` scans PCI for the Bochs VBE device, maps its VRAM, and exposes:

```c
u32  fb_get_width(void);
u32  fb_get_height(void);
u32  fb_get_pitch(void);
u32 *fb_get_addr(void);
```

---

## 2D Drawing Primitives (`kernel/gfx.c`)

| Function | Description |
|----------|-------------|
| `gfx_fill_rect(x, y, w, h, color)` | Filled rectangle |
| `gfx_draw_rect(x, y, w, h, color)` | Outline rectangle |
| `gfx_draw_line(x0, y0, x1, y1, color)` | Bresenham line |
| `gfx_draw_char(x, y, ch, fg, bg)` | 8×8 bitmap glyph |
| `gfx_draw_string(x, y, str, fg, bg)` | Null-terminated string |
| `gfx_alpha_blend(dst, src, alpha)` | 8-bit alpha blend |

---

## Window Manager (`kernel/gui.c`)

The largest graphics file (~73 KB). Intended to provide:

- Z-ordered window list with title bars (close/min/max buttons)
- Drag and resize via mouse
- A 56 px dock at screen bottom with animated icon magnification
- Desktop icons that launch ELF binaries on double-click
- Dark "abyss" colour theme

**Not functional at runtime.** The code is present for future work.

---

## Input Layer (`kernel/input.c`)

Coalesces PS/2 and USB HID events into a ring buffer of `input_event_t`. Works correctly in text mode (the shell uses it). The GUI compositor is supposed to read from this queue but never gets far enough to do so.

---

## PNG Viewer (`kernel/pngview.c`)

A dependency-free PNG decoder for the `photo <file>` shell command. Intended to blit decoded images to the framebuffer. **Not usable** until the framebuffer issue is resolved.

---

## Colour Palette (intended)

| Constant | Hex | Use |
|----------|-----|-----|
| `C_BASE` | `#05070A` | Desktop background |
| `C_SURFACE` | `#0F141A` | Window background |
| `C_ACCENT` | `#3584E4` | Focus / buttons (Adwaita blue) |
| `C_TEXT` | `#FFFFFF` | Primary text |
| `C_TEXT_DIM` | `#9A9996` | Secondary text |
