# Shaders with LWM

LWM is a window manager, not a compositor. It arranges windows and publishes
state; pixel composition (and therefore shaders) is the compositor's job. The
recommended setup is **LWM + picom**, where picom does GLSL fragment shading on
windows and keys its rules off the EWMH state LWM already maintains.

This document is a recipe, not a feature of LWM itself.

## What LWM gives the compositor

Picom rules can match on standard X/EWMH state. LWM keeps the following
authoritative and current, which is enough to drive most visual rules:

- `_NET_ACTIVE_WINDOW` — the focused window
- `_NET_CURRENT_DESKTOP` — current workspace on the focused monitor
- `_NET_WM_STATE` — including `FULLSCREEN`, `HIDDEN`, `FOCUSED`, `STICKY`,
  `ABOVE`, `BELOW`, `DEMANDS_ATTENTION`
- `_NET_WM_WINDOW_TYPE` — `DIALOG`, `DOCK`, `DESKTOP`, `UTILITY`, etc.
- `_NET_CLIENT_LIST_STACKING` — Z order
- `WM_CLASS`, `WM_NAME` — for per-application targeting

What LWM does **not** currently publish, and that picom therefore cannot key
off of: tiled-vs-floating, layout role (master/stack), focused monitor, or
workspace transitions. If you want shaders that react to those, that is the
job of a future LWM-specific property extension; see the end of this doc.

## Install

Arch:

```bash
sudo pacman -S picom
```

Debian/Ubuntu:

```bash
sudo apt install picom
```

Use a recent build (the `yshui/picom` lineage). Older `compton`-era forks have
a different shader API and are not covered here.

## Wiring picom into your session

Start picom after LWM, e.g. from `.xinitrc`:

```sh
exec lwm &
picom --backend glx --config ~/.config/picom/picom.conf &
wait
```

The `glx` backend is required for fragment shaders.

## A minimal `picom.conf`

```ini
backend = "glx";
vsync = true;
use-damage = true;

# Per-window fragment shader, plus per-rule overrides.
window-shader-fg = "~/.config/picom/shaders/passthrough.frag";
window-shader-fg-rule = [
  "~/.config/picom/shaders/desaturate.frag:!focused && !class_g = 'polybar' && window_type != 'dock' && window_type != 'desktop'",
  "~/.config/picom/shaders/scanlines.frag:class_g = 'Alacritty' || class_g = 'XTerm'",
  "~/.config/picom/shaders/vignette.frag:window_type = 'dialog'",
  "none:class_g = 'polybar' || window_type = 'dock' || window_type = 'desktop'",
];
```

Rules are evaluated top-to-bottom; the first match wins. The trailing `none`
rule keeps panels and the desktop layer untouched.

## Shader API (picom, modern)

Every fragment shader must define `vec4 window_shader()` and may call
`default_post_processing` to apply picom's opacity/dim/invert pipeline.

```glsl
#version 330
in vec2 texcoord;
uniform sampler2D tex;
uniform float opacity;
vec4 default_post_processing(vec4 c);

vec4 window_shader() {
    vec4 c = texelFetch(tex, ivec2(texcoord), 0);
    // modify c here
    return default_post_processing(c);
}
```

Save shaders under `~/.config/picom/shaders/`.

## Example shaders

### `passthrough.frag` — no-op default

```glsl
#version 330
in vec2 texcoord;
uniform sampler2D tex;
vec4 default_post_processing(vec4 c);
vec4 window_shader() {
    return default_post_processing(texelFetch(tex, ivec2(texcoord), 0));
}
```

### `desaturate.frag` — drain color from unfocused windows

Subtle "focus follows attention" cue. LWM publishes `_NET_WM_STATE_FOCUSED`,
so picom's `focused` predicate is reliable.

```glsl
#version 330
in vec2 texcoord;
uniform sampler2D tex;
vec4 default_post_processing(vec4 c);

const float AMOUNT = 0.65;  // 0 = full color, 1 = grayscale

vec4 window_shader() {
    vec4 c = texelFetch(tex, ivec2(texcoord), 0);
    float l = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
    c.rgb = mix(c.rgb, vec3(l), AMOUNT);
    return default_post_processing(c);
}
```

### `scanlines.frag` — CRT lines on terminals

Targeted by `class_g = 'Alacritty'` in the picom rule above. Cheap, very
recognizable, pairs well with retro color schemes.

```glsl
#version 330
in vec2 texcoord;
uniform sampler2D tex;
vec4 default_post_processing(vec4 c);

void main() {}  // unused; picom uses window_shader()

vec4 window_shader() {
    vec4 c = texelFetch(tex, ivec2(texcoord), 0);
    float line = mod(texcoord.y, 2.0);
    float dim = line < 1.0 ? 0.88 : 1.0;
    c.rgb *= dim;
    return default_post_processing(c);
}
```

### `vignette.frag` — soft edge darkening for dialogs

Useful for transient/dialog windows so they read as overlaid, not embedded.

```glsl
#version 330
in vec2 texcoord;
uniform sampler2D tex;
uniform float texture_width;
uniform float texture_height;
vec4 default_post_processing(vec4 c);

vec4 window_shader() {
    vec4 c = texelFetch(tex, ivec2(texcoord), 0);
    vec2 uv = texcoord / vec2(texture_width, texture_height);
    float d = distance(uv, vec2(0.5));
    float v = smoothstep(0.75, 0.45, d);
    c.rgb *= mix(0.7, 1.0, v);
    return default_post_processing(c);
}
```

### Other ideas worth trying

- **Frosted glass on floating windows**: picom's built-in `blur-background`
  plus a tint pass. Matches well with semi-transparent terminal themes.
- **Sepia on `class_g = 'zathura'`**: a reading-mode look for PDFs.
- **Red-shift on `_NET_WM_STATE_DEMANDS_ATTENTION`**: visual urgency cue
  that costs nothing in LWM.
- **Subtle gamma lift on the focused window**: the inverse of `desaturate` —
  push focused content slightly brighter rather than dimming the rest.
- **Color-blind simulation overlays**: bind a key in LWM to start/stop picom
  with an alternate config that loads a deuteranopia/protanopia LUT shader.
  Useful for accessibility QA without changing the WM.

## Limits of this approach

Shaders here see X state, not LWM intent. So the following are **not**
expressible with picom alone:

- "Dim every window on the non-focused monitor" — picom does not know which
  monitor LWM considers focused.
- "Tint master vs stack differently in master/stack layout" — layout role is
  internal to LWM.
- "Run a wipe animation between workspaces" — workspace switch is not an
  event picom subscribes to in a useful way.

Adding any of these is a separate piece of work: LWM would expose extra
`_LWM_*` properties (e.g. `_LWM_LAYOUT_ROLE`, `_LWM_FOCUSED_MONITOR`,
`_LWM_TRANSITION`) and picom rules or a small companion daemon would read
them. That extension is not implemented today; this document covers only
what works against current LWM.
