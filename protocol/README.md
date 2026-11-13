# Bottie protocol v1

The optional [clock extension](clock.md) carries bounded UTC milliseconds and a local minute offset independently of playback freshness and control/text identity. Invalid optional clock data is ignored by consumers.

The companion owns account access, metadata, controls and artwork conversion. The device owns rendering and touch. No account credentials cross this interface. HTTP is intended for loopback or a trusted local network. Configure device bearer authentication before binding to a non-loopback address.

## Playback

`GET /v1/playback` returns HTTP 200 and `application/json`, at most 8192 bytes, matching [playback.schema.json](playback.schema.json). Every documented field is emitted. Clients tolerate missing context/artwork and ignore unknown fields. UTF-8 strings are truncated at a character boundary: track_id <=128 bytes; title, artist, album and context.name <=256 bytes each; artwork.url <=256 bytes. Duration is 0..86400000 ms, progress is 0..duration, timestamps are nonnegative Unix milliseconds no greater than 9007199254740991. State is playing, paused, idle or unavailable. Repeat is off, context or track.

`updated_at_ms` records when the snapshot was observed, not when it was served. A client may advance progress while playing, capped by duration and a 15-second stale threshold. Stop extrapolating and disable controls when stale or after transport failure. An unavailable response disables all capabilities. An idle response has no active track. Unknown metadata uses an empty string. A missing playlist name falls back to album context if an album is known, otherwise unknown. The device shows playlist above album when present, or album alone.

The fixture starts paused on track `fixture-orbit`, at zero progress. Explicit controls and an injected clock make tests deterministic. A running server advances playing progress with its monotonic clock. `X-Bottie-Status` provides a non-secret diagnostic code. It is `ok` on success; failures use not_authorized, rate_limited, no_device, restricted or upstream_error. Backoff adds `Retry-After` in seconds. An unavailable snapshot keeps the last observation timestamp and disables controls; callers must not interpret an HTTP 200 as proof of fresh upstream playback.

An optional `text_mask` descriptor from the narrow gateway provides [bounded Bengali/Japanese metadata masks](text-mask.md). The original name strings are unchanged. Older clients ignore this additive field. Native punctuation and ASCII names retain the existing pixel font; translated menus are outside this extension.

## Controls

`POST /v1/control` accepts `application/json`, maximum 1024 bytes, one object only, no unknown fields:

| action | value |
| --- | --- |
| play | omitted |
| pause | omitted |
| previous | omitted |
| next | omitted |
| shuffle | boolean, required |
| repeat | off, context or track string, required |
| volume | integer 0..100, required; `control_target` also required |

Snapshots emit `volume_percent` as an integer 0..100 or `null` when unknown. Missing values in older snapshots also mean unknown, never zero. `capabilities.volume` is false when absent; it becomes true only for an explicitly supported, active, nonrestricted player with control permission. A known volume alone does not grant permission. The Spotify endpoint requires Premium and `user-modify-playback-state` authorization. Local codec volume and Spotify player volume are separate controls.

`control_target` is an opaque generation, exactly 32 lowercase hexadecimal characters (`[0-9a-f]{32}`), or an empty string when no controllable target is known. It contains no device ID or account identifier. New device controls must include the current nonempty `control_target` on every action, for example `{"action":"volume","value":35,"control_target":"00000000000000000000000000000001"}`. The six older actions still accept omission for existing device compatibility; explicit empty, null or malformed targets are rejected. Volume requires a target and strict integer notation, without strings, fractions or exponents.

Before a guarded command, the companion checks fresh upstream target/capability state and pins the private device ID internally for that write. A changed target returns HTTP 409; no command is sent. Generations change after a device switch, failed or stale observation, restriction or reauthorization. The device must discard pending buttons and volume values whenever the target changes, disappears or becomes stale. Snapshots without the extension still render, but new physical controls remain disabled. A fresh paused track with `capabilities.play=true` may resume from the idle-world screen; true idle without a target cannot.

Keep only the newest unsent slider value in a separate pending slot. The companion permits at most one volume dispatch per second, honors upstream `Retry-After`, and never replays control writes after a 401, 429 or ambiguous transport result. Wait for observed playback to confirm volume; an accepted command does not update cached volume optimistically. Unknown volume may show an unavailable indicator while a supported absolute control remains usable.

Success is HTTP 200 with `{"ok":true}`. It means the command was accepted, not that the downstream player has already changed. Fetch playback again. Errors are `{"error":{"code":"invalid_control","message":"..."}}`, bounded to 1024 bytes. Codes/statuses: invalid_json or invalid_control (400), unauthorized (401), forbidden or restricted (403), unsupported or target_changed (409), too_large (413), content_type (415), rate_limited (429), internal_error (500), upstream_error (502), not_authorized or no_device (503). Rate limits include Retry-After. The narrow gateway preserves HTTP error status and bounded Retry-After while replacing upstream error text. Never retry controls automatically after ambiguous transport failure.

Use `Authorization: Bearer <device token>` on both endpoints and artwork when configured. The token is local device authentication, never a Spotify token. No query-string token is supported.

## Artwork

`GET artwork.url` returns `application/octet-stream`. Relative URLs resolve against the companion origin. An empty URL means no image. There is no URL-fetch API. Images come only from trusted HTTPS artwork URLs supplied by Spotify playback responses or original local fixtures. Downloads are bounded to 4 MiB; source images to 4096 per dimension and 4,194,304 pixels. Cache holds at most 8 images and 2 MiB. Default target is square 96, configurable from 16 to 240 pixels per side. Artwork is proportionally fitted in the square with black padding, with no crop, distortion or overlays.

The binary format has a **16-byte header**, then row-major RGB565 little-endian pixels:

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `BTAR` |
| 4 | 1 | version = 1 |
| 5 | 1 | format = 1 (RGB565 little-endian) |
| 6 | 2 | width, uint16 LE |
| 8 | 2 | height, uint16 LE |
| 10 | 2 | stride, uint16 LE = width * 2 |
| 12 | 4 | payload length, uint32 LE = width * height * 2 |

Pixel bits are RRRRRGGG GGGBBBBB. No trailing bytes. Validate header, safe dimensions, stride, multiplication and complete payload before rendering. Maximum total transport size is 115216 bytes. Firmware may impose a smaller configured bound and decline larger images safely. Fetch artwork again if a cache URL returns 404. HTTP errors are JSON, never BTAR.

`fixtures/orbit-96.btar` is deterministic original artwork. JSON examples include full, missing optional metadata, stale, idle, unavailable and maximum metadata states. `invalid.json` and `oversized-metadata.json` are deliberately invalid and must be rejected. No hardware dimensions or pins are implied by these fixtures.

The additive fixtures `volume-supported`, `volume-muted`, `volume-unknown`, `volume-unsupported`, `volume-target-changed` and `guarded-paused` exercise capability, null/zero and target-generation handling. Existing fixtures intentionally omit these extensions to check backward-compatible rendering and disabled new physical controls.
