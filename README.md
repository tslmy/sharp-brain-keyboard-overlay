
The Brain has a small, non-standard keyboard where most characters are typed
with the **Shift** and **記号 (Symbol)** modifiers. `keyoverlay` is a tiny,
dependency-free daemon that listens to the keyboard and, while a modifier is
held down, draws the matching key legend layout directly onto the Linux
framebuffer. Release the modifier and the overlay disappears.

| Held key            | Overlay shown          |
| ------------------- | ---------------------- |
| Shift               | Shift layout           |
| 記号 (Symbol)       | Symbol layout          |
| 記号 + Shift        | Symbol + Shift layout  |
| (configurable key)  | Normal layout          |

The layouts are generated programmatically from tables in the source plus an
embedded 8x16 bitmap font — there is no image file to ship or decode at
runtime.

## How it works

- It opens an evdev device (`/dev/input/event*`) **passively** (it never grabs
  the device), so all keystrokes still reach the console and applications.
- It tracks which trigger keys are currently held.
- On a change it snapshots the framebuffer, paints the overlay panel, and
  restores the snapshot when the last modifier is released.
- The framebuffer pixel format (RGB565 / XRGB8888 / …) is detected at runtime
  via `FBIOGET_VSCREENINFO`, so it works on both 16- and 32-bpp displays.

This targets a **TUI / framebuffer console** environment (e.g. the Buildroot
rootfs). It does not require X11.

## Building

```sh
make                 # native build
make CC=arm-linux-gcc # cross build
```

The only requirements are a C compiler and the Linux UAPI headers
(`linux/fb.h`, `linux/input.h`). No external libraries.

## Usage

```
keyoverlay [options]
  -d DEV    input device (default: auto-detect by name)
  -m NAME   device name substring for auto-detect (default: brain-kbd)
  -f FB     framebuffer device (default: /dev/fb0)
  -s CODE   key code emitted while Symbol (記号) is held (default: 186 = KEY_F16)
  -n CODE   key code that triggers the Normal layout (default: 0 = disabled)
  -l        list input devices and exit
  -v        verbose
  -h        help
```

Example:

```sh
keyoverlay -v             # auto-detect device, KEY_F16 as Symbol
keyoverlay -n 187         # also show Normal layout when KEY_F17 is held
```

## Kernel requirement: detecting the Symbol key

On the Brain, the **記号 (Symbol)** key is consumed inside the kernel keyboard
driver to select the symbol keymap; by default it emits no input event, so
userland cannot tell when it is held.

The companion kernel change adds a device-tree property that makes the driver
emit a dedicated, otherwise-inert key event on Symbol press/release:

```dts
&keyboard_gpio {
    symbol-key = <4 3>;
    symbol-event-code = <KEY_F16>;   /* emitted on Symbol press/release */
};
```

`keyoverlay -s 186` (the default) then watches `KEY_F16`. `KEY_F16` is inert on
the console, so it does not interfere with normal symbol input. **Shift** is
already a real modifier (`KEY_LEFTSHIFT`) and needs no kernel change.

## Discovering unused keys (for the Normal-layout trigger)

Some models have physical keys that are not present in the keymap. To find the
matrix coordinates / key code of such a key on a GPIO-keyboard model:

1. Enable discovery in the keyboard driver at runtime:
   ```sh
   echo 1 > /sys/module/brain_kbd_gpio/parameters/discover
   ```
2. Press the mystery key and read the kernel log:
   ```sh
   dmesg | tail
   # brain-kbd-gpio ...: discover: unmapped key pressed at matrix <i j>
   ```
3. Add a keymap entry for `<i j>` in the model's device tree, mapping it to an
   inert code such as `KEY_F17`, rebuild the kernel, and pass that code to
   `keyoverlay -n <code>`.

## License

MIT. The embedded font (`src/font8x16.h`) is the classic IBM VGA 8x16 font,
which is in the public domain.
