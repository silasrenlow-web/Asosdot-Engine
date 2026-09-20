# Lens flare textures

Greyscale flare elements used by the built-in `LensFlareElement` types, embedded
into the binary by `servers/rendering/renderer_rd/effects/SCsub`.

- Upstream: <https://opengameart.org/content/lens-flares-and-particles>
- Author: hc (hackcraft.de)
- Version: `flare_20120324`
- License: CC0 1.0 Universal (public domain dedication), see `LICENSE`.

Only the nine textures the default flare uses are vendored; the upstream pack
has thirty six. They are 8 bit greyscale PNGs where the single channel is
coverage, which the renderer uploads as `R8_UNORM` and swizzled to
`(r, r, r, 1)`.

## Local changes

Each texture has had any uniform floor subtracted, everything outside its
inscribed ellipse masked off with a smooth edge, and the result renormalised so
its peak is still 255.

Upstream, `nova.png` carries a floor of 1/255 across the whole image, and
`pearl.png` and `flaredouble.png` reach 10/255 at their borders and corners.
Drawn additively that is enough to show the element's own quad as a faint
rectangle, which is the one thing a lens flare must never do. Every element is
centred, so nothing belongs in the corners of its quad anyway.

CC0 permits the modification; nothing else was touched.
