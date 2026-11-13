# Daily-use additions to protocol v1

The schema version, 8192-byte complete playback limit, 1024-byte control limit,
BTAR and BTXT v1 remain unchanged. Firmware 167 may ignore these additive fields.
Unknown or malformed optional `details` and `offline_reason` must not discard
otherwise valid playback. Omit details if serializing them would exceed 8192
bytes or 256 parser tokens. Count each object, array, key and value as one token;
closing delimiters do not add tokens. Receivers independently enforce safe
parser bounds even for hostile input.

## Guarded seek and feedback

`capabilities.seek` defaults false. `control_track` is an opaque, nonsecret,
32-character lowercase hexadecimal generation. It is valid only alongside a
fresh `control_target`, identified track and known duration. The host retains it
across fresh observations of the same player, track and duration. It changes
after an observed player/track/duration change, authorization loss or freshness
discontinuity. Clock, progress, playlist resolution and optional assets do not
change it. Spotify player IDs remain private to the host.

```json
{"action":"seek","value":25000,"control_target":"0123456789abcdef0123456789abcdef","control_track":"abcdef0123456789abcdef0123456789"}
```

`value` is a strict JSON integer from 0 through 86400000 milliseconds. Both
guards are required for seek. Other actions reject `control_track`; existing
legacy control bodies remain valid. Before one upstream write the host obtains
fresh playback, checks the same internal player, track and positive duration,
checks restrictions and scope, and clamps to `min(value,duration_ms-1)`. It pins
the private player ID. Unknown or changed duration disables/rejects the seek.
There is at most one seek write attempt per second. An unsent device seek
expires 1000 ms after release; a newer unsent release may replace it. A drag
previews locally and dispatches at most once on release. Dispatched commands,
including ambiguous transport failures and 429 responses, are never replayed.

The host cannot atomically lock Spotify between its fresh GET and subsequent
PUT. An external controller can still change tracks in that interval. Spotify
requires Premium and the existing `user-modify-playback-state` scope for seek.
See the [official endpoint](https://developer.spotify.com/documentation/web-api/reference/seek-to-position-in-currently-playing-track).

Successful replies retain `ok:true` and add `result:"accepted"`. Seek also
returns the effective integer `position_ms`. Acceptance is not observed playback
confirmation. Device feedback uses one in-flight callback, a local monotonic
request sequence and the original action/target/track context. An older reply
must not clear a newer pending action, even for identical actions and guards.
Pending and error displays expire; a transport failure remains an unknown
outcome. No optimistic success or automatic retry is implied.

## Availability reasons

Optional `offline_reason` is one of `login_required`, `no_active_player`,
`restricted`, `rate_limited`, `upstream_unavailable`, `wallet_locked`,
`wallet_unavailable`, or `stale`. Unknown, null and malformed values are ignored.
The host supplies application facts; the device owns `wifi_disconnected` and
`gateway_unreachable` from its own connection evidence. A reason never replaces
the playback state or invalidates the last valid monotonic clock anchor.

## Detail descriptor and identity

The gateway advertises bounded, shaped names separately from the closed-screen
BTXT bundle. Original metadata remains unchanged and is not translated. ASCII
uppercase letters are lowered for display, including mixed-script names.

```json
{"details":{"layout":1,"id":"<64 lowercase hex>","title":{"pages":["<body SHA256>"],"flags":0}}}
```

The four optional field keys are `title`, `artist`, `album`, and `context`.
Each is `{pages:[SHA256...],flags:0..3}`, with zero through four pages. There
are at most sixteen references. Empty fields are omitted; an empty pages array
with flags zero is also valid. Context means an actual named playlist; an
album-only primary header opens album detail. No field fabricates another name
as a fallback. Missing assets display unavailable text without reusing an old
page.

`id` hashes the exact bounded raw track ID, title, artist, album, context type
and name, pinned font digests, shaping-library identity, layout and tail policy.
Clock, progress and control generations are excluded. The descriptor changes
when that identity changes. Each page URL is `/v1/detail/<SHA256 exact body>`.
The authenticated gateway accepts only that canonical path, without query,
redirect, arbitrary text, remote URL or external font lookup.

## BTDT layout 1

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `BTDT` |
| 4 | 1 | Version 1 |
| 5 | 1 | Format 1: A4 alpha, high nibble is the left pixel |
| 6 | 1 | Layout 1 |
| 7 | 1 | Field: 0 title, 1 artist, 2 album, 3 context |
| 8 | 1 | Zero-based page index |
| 9 | 1 | Total pages in this field, 1 through 4 |
| 10 | 1 | Flags: bit 0 source-tail uncertainty, bit 1 page-limit truncation |
| 11 | 1 | Reserved, zero |
| 12 | 4 | Little-endian payload length, exactly 10368 |

Layout 1 fixes width 216, height 96 and stride 108 bytes. The payload is row
major with no color, overlays or variable dimensions. Total size is exactly
10384 bytes. All other flag bits, mismatched count/index/field/flags, wrong SHA,
wrong dimensions implied by layout, and truncated/extra payload are rejected.
Flags describe the entire field consistently across its pages.

Names wrap only at complete extended grapheme and shaping-cluster boundaries.
Measure complete ink and reshape retained text. A raw input of 253 through 256
UTF-8 bytes sets source-tail uncertainty before normalization: the old producer
may have clipped a final mark. Discard the final complete cluster and show an
ellipsis. This reports uncertainty, not recovered source or proven truncation.
Overflow beyond four pages sets page-limit truncation and uses a safe final
ellipsis. The device shows a bounded partial-name indication for either flag.

## Lifetime and resource bounds

Metadata and controls never wait for detail shaping. One cancellable worker and
one replaceable pending identity prepare pages. A descriptor is published only
with complete retained field pages. Cancel outdated work and suppress late
publication. Cache at most four identities for five minutes, at most sixteen
pages each. Page bytes are at most 664576 cached, 166144 for the current job,
and 83072 for eight bounded response copies: 913792 total. Additional explicit
scratch includes one four-plane field buffer (41472 bytes), one 216x96 Cairo A8
surface (20736 bytes), normalized/candidate text (1025/1028 bytes), and fixed
cluster/cut arrays. Each job retains at most four 256-byte labels; each cached
descriptor has at most sixteen 64-byte digests. Native font/glyph caches and
Pango object overhead are separate from these payload bounds, with only the two
pinned fonts loaded. Eviction must not create an unbounded
list of retained responses. Shutdown cancels and joins the worker.

The device fetches only the selected page, with no prefetch. Network, shared and
displayed copies cost at most 31152 page bytes; repeated descriptor copies are
counted separately. No page-sized stack allocation. Bind a fetch and response
to captured raw metadata, track ID, descriptor ID, field, page, flags and body
digest. Closing the card, selecting another page, changing identity or issuing
a control cancels optional work; late results cannot publish. Freeze the
displayed page across all render bands. Existing optional-request cancellation,
metadata/control priority and no-replay rules still apply.

The native shaping call finishes its bounded current field before cancellation
can take effect. It runs only in the dedicated worker and cannot hold metadata
or control requests. Cancellation is checked between fields and before
publication. It is not a hard real-time CPU deadline.

Generate fictional detail fixtures with `go run ./cmd/detail-fixtures OUTPUT`
from `companion`. JSON descriptors reference the exact generated BTDT hashes;
the names include Bengali, Japanese, mixed Latin, album-only, four-page and
source-uncertainty cases. No account or live playback is required.
