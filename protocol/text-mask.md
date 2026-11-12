# Bengali and Japanese metadata masks

This additive v1 extension renders original song, artist, album and playlist names. It does not translate names or menus. Existing ASCII labels keep their exact device pixel renderer. The gateway shapes other supported text with local Pango/HarfBuzz and pinned Noto fonts; the Spotify companion can keep its existing signed-in process.

An optional playback field advertises a cached bundle:

```json
"text_mask": {"url": "/v1/text/<64 lowercase SHA-256 hex characters>", "layout": 1}
```

The digest is SHA-256 of the complete binary response. GET accepts only this exact same-origin path, no query or caller-provided text, font, dimensions or URL. It uses the same device bearer, origin restrictions and bounded concurrency as other device endpoints. Missing, expired or evicted bundles return 404. No arbitrary font or external URL fetching is available.

The binary response is at most 8,494 bytes, including this 16-byte header. All multibyte integers are little-endian.

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `BTXT` |
| 4 | 1 | Version 1 |
| 5 | 1 | Format 1, packed 4-bit alpha |
| 6 | 1 | Line flags, only bits 0..3 allowed, at least one set |
| 7 | 1 | Layout 1 |
| 8 | 2 | Frame width 240 |
| 10 | 2 | Frame height 296 |
| 12 | 4 | Exact payload length |

Only flagged planes follow, in increasing bit order. Rows are tightly packed. The high nibble is the left pixel; alpha 0 is transparent and 15 is opaque. There is no RGB color in a mask. The device blends against its normal background using each label's existing ink/accent color. Planes have fixed geometry:

| Bit | Label | X | Y | Width | Height | Stride | Bytes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | Playlist, or album when no playlist exists | 14 | 6 | 212 | 18 | 106 | 1908 |
| 1 | Album below a playlist | 19 | 27 | 202 | 18 | 101 | 1818 |
| 2 | Song title | 12 | 184 | 216 | 26 | 108 | 2808 |
| 3 | Artist | 12 | 212 | 216 | 18 | 108 | 1944 |

The largest payload is 8,478 bytes. Unknown layout/format, excess flags, wrong frame geometry, inconsistent size, truncation, trailing bytes or digest mismatch invalidate the complete bundle. A clear flag keeps that label's native ASCII path. No artwork, control or progress geometry changes.

The native predicate accepts all codepoints below 128 plus U+00A0, U+2010..U+2014, U+2018, U+2019, U+201C and U+201D, matching existing punctuation normalization. A line needs shaping if it contains any other codepoint. A flag may be set only for a line that needs shaping and exists under the playlist/album fallback rule. A line with unsupported glyphs has no flag; successful other planes remain usable. The payload length is the sum of only flagged plane lengths. All-ASCII/native-punctuation snapshots omit the descriptor entirely.

The gateway cache key includes the exact bounded track ID, title, artist, album, context type/name, layout, font digests and shaping-library versions. It holds at most eight bundles (67,952 response bytes). Shaping is serialized and performed only on a changed cache key, never per device frame or band. The device retains the exact source metadata alongside the descriptor. It invalidates masks on any relevant metadata/track/layout change and rejects a late fetch for an older snapshot. A control target is not a text identity. Body hashing verifies transport bytes, while exact snapshot comparison prevents stale labels.

Names remain unchanged in JSON. The existing companion caps each name at 256 UTF-8 bytes on a codepoint boundary. This does not guarantee an intact final grapheme, and a gateway cannot recover bytes already discarded. For fields of 253..256 bytes, the gateway conservatively drops the final present grapheme/shaping cluster and adds an ellipsis before layout. Width clipping also selects complete safe clusters and reshapes the retained prefix with the ellipsis. Unsupported glyphs do not produce a misleading successful mask. ASCII letters in shaped mixed-script text use the device's existing lowercase display convention.

Pinned fonts used by this layout:

| Font | Installed file | SHA-256 |
| --- | --- | --- |
| Noto Sans Bengali Regular 3.011 | `/usr/share/fonts/noto/NotoSansBengali-Regular.ttf` | `b55c62ee531e3214da6c0701daecea89a52ba42db7d8206b92e6b51f397a3193` |
| Noto Sans CJK JP Regular 2.004, collection face 0 | `/usr/share/fonts/NotoSansCJK/NotoSansCJK-Regular.ttc` | `b76b0433203017ca80401b2ee0dd69350349871c4b19d504c34dbdd80541690a` |

Both files' embedded license records identify SIL Open Font License 1.1. Primary license sources: [Noto Bengali](https://github.com/notofonts/bengali/blob/main/OFL.txt), [Noto CJK Sans](https://github.com/notofonts/noto-cjk/blob/main/Sans/LICENSE). The fonts stay on the host; firmware contains only the bounded mask decoder. [Pango](https://docs.gtk.org/Pango/) is LGPL-2.1-or-later, and [HarfBuzz](https://github.com/harfbuzz/harfbuzz/blob/main/COPYING) uses its permissive Old MIT license. Runtime font replacement requires updating and re-verifying the pinned hashes and layout fixtures.
