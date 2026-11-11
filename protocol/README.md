# Bottie protocol v1

The companion owns account access, metadata, controls and artwork conversion. The device owns rendering and touch. No account credentials cross this interface. HTTP is intended for loopback or a trusted local network. Configure device bearer authentication before binding to a non-loopback address.

## Playback

`GET /v1/playback` returns HTTP 200 and `application/json`, at most 8192 bytes, matching [playback.schema.json](playback.schema.json). Every documented field is emitted. Clients tolerate missing context/artwork and ignore unknown fields. UTF-8 strings are truncated at a character boundary: track_id <=128 bytes; title, artist, album and context.name <=256 bytes each; artwork.url <=256 bytes. Duration is 0..86400000 ms, progress is 0..duration, timestamps are nonnegative Unix milliseconds no greater than 9007199254740991. State is playing, paused, idle or unavailable. Repeat is off, context or track.

`updated_at_ms` records when the snapshot was observed, not when it was served. A client may advance progress while playing, capped by duration and a 15-second stale threshold. Stop extrapolating and disable controls when stale or after transport failure. An unavailable response disables all capabilities. An idle response has no active track. Unknown metadata uses an empty string. A missing playlist name falls back to album context if an album is known, otherwise unknown. The device shows playlist above album when present, or album alone.

The fixture starts paused on track `fixture-orbit`, at zero progress. Explicit controls and an injected clock make tests deterministic. A running server advances playing progress with its monotonic clock. `X-Bottie-Status` provides a non-secret diagnostic code. It is `ok` on success; failures use not_authorized, rate_limited, no_device, restricted or upstream_error. Backoff adds `Retry-After` in seconds. An unavailable snapshot keeps the last observation timestamp and disables controls; callers must not interpret an HTTP 200 as proof of fresh upstream playback.

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

Success is HTTP 200 with `{"ok":true}`. It means the command was accepted, not that the downstream player has already changed. Fetch playback again. Errors are `{"error":{"code":"invalid_control","message":"..."}}`, bounded to 1024 bytes. Codes/statuses: invalid_json or invalid_control (400), unauthorized (401), forbidden or restricted (403), unsupported (409), too_large (413), content_type (415), rate_limited (429), internal_error (500), upstream_error (502), not_authorized or no_device (503). Rate limits include Retry-After. Never retry next/previous automatically after ambiguous transport failure.

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
