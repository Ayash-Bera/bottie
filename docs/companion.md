# Local companion

Go 1.26 or later is required. The service runs on the computer that hosts the Spotify connection. The device receives bounded playback data, control capabilities and converted artwork. Audio remains on the active Spotify playback device, including Alexa when Spotify exposes it as controllable.

## Offline start

From the repository root:

```fish
cd companion
go run ./cmd/server
```

Open `http://127.0.0.1:8787/` for status. `GET /v1/playback` starts paused on Orbital Sunrise. `POST /v1/control` with `{"action":"play"}` starts fixture progress. The three original sample tracks are Orbital Sunrise (Small Worlds), Quiet Tides (Moonwater), and Distant Signal (no album/context/artwork), all credited to the fictional Bottie Ensemble. Artwork is procedural original material, not a downloaded commercial cover. Quiet Tides uses an album-only heading; Orbital Sunrise includes the Desk Orbits playlist. The fixture plays no audio.

All six actions work without an account. Shuffle selects a fixed alternative order for reproducibility; repeat cycles off/context/track. Fixture progress uses elapsed time, remains within duration, and stops at the last track when repeat is off. Start each integration run with a fresh process for the same initial state.

## Configuration

The config package reads the environment once at startup, with optional `.env` loading from the current directory. Process environment takes precedence. Keep credentials out of committed files. The application never writes secrets, account tokens or refresh tokens to disk.

| Variable | Default | Constraints |
| --- | --- | --- |
| BOTTIE_MODE | fixture | fixture or spotify |
| BOTTIE_BIND | 127.0.0.1:8787 | IP literal and port; non-loopback requires device token |
| BOTTIE_DEVICE_TOKEN | unset | 32..256 printable ASCII characters without spaces; required for LAN |
| BOTTIE_ARTWORK_SIZE | 96 | square side 16..240 |
| BOTTIE_POLL_SECONDS | 3 | 2..10 seconds |
| SPOTIFY_CLIENT_ID | unset | Required only in spotify mode; 32 hexadecimal characters from app registration |
| SPOTIFY_REDIRECT_URI | http://127.0.0.1:8787/auth/callback | HTTP loopback literal, callback path and bound port must match |

The redirect default follows a changed bind port. `localhost` is deliberately rejected. A Spotify listener must accept the configured loopback callback. For IPv6 use `[::1]` consistently. Admin/status/OAuth routes require a loopback peer and loopback Host. Forwarded headers are not trusted. To preserve an existing loopback Spotify session when connecting a device, use the narrow device gateway described below.

To set a device secret without printing it, start an interactive fish session and paste a random value of at least 32 characters from a password manager into the hidden prompt:

```fish
read --silent --prompt-str 'Device token: ' bottie_secret
set -gx BOTTIE_DEVICE_TOKEN $bottie_secret
set -e bottie_secret
set -gx BOTTIE_BIND 0.0.0.0:8787
```

Run the server from that same session. Set the same token in the eventual device provisioning step; do not place it in command arguments, URLs, screenshots or shell history. All `/v1` requests, including artwork, require `Authorization: Bearer <device token>` when configured. Use a trusted private network; the built-in listener is HTTP, so bearer authentication does not encrypt the transport. Public exposure and device provisioning are outside this milestone. Keep the default loopback bind until the device is configured.

The opt-in [ESP transport](../firmware/network/README.md) accepts an IPv4 literal origin and a separate bearer token containing letters, digits or `-._~+/=`. Choose at least 32 characters to satisfy both ends. Keep `BOTTIE_ARTWORK_SIZE=96` for its bounded artwork cache. Plain HTTP also requires the firmware's explicit LAN opt-in. Private Wi-Fi and device credentials stay in an ignored local configuration file and configured firmware images must remain private. The default firmware has no credentials; compiled networking has not been exercised on hardware.

## Device gateway preserving a signed-in companion

`cmd/device-bridge` is a separate, bounded HTTP adapter to the fixed upstream `http://127.0.0.1:8787`. It preserves the companion process and its in-memory Spotify session. It has no Spotify configuration or account tokens, reads no `.env`, and forwards no caller cookies, bearer, origin or forwarded headers. The existing companion must remain on loopback, with its device-token setting unset; this adapter authenticates the device before forwarding. Restarting the gateway does not restart the companion.

| Variable | Default | Constraints |
| --- | --- | --- |
| BOTTIE_BRIDGE_BIND | 127.0.0.1:8788 | Specific private or loopback IPv4 literal and port; no wildcard, public IP or port 8787 |
| BOTTIE_BRIDGE_TOKEN_FILE | required | Absolute path to a regular 0600 file, no symlink; 32..256 letters, digits or `-._~+/=`, optional final newline |

Every route requires the separate device bearer. Only these method/path pairs are forwarded:

- `GET /v1/playback`
- `POST /v1/control`
- `GET /v1/artwork/<id>`, with 1..128 letters, digits, underscores or hyphens

The gateway exposes no home, status, login or callback route. It rejects query strings, encoded paths, arbitrary origins and all browser Origin/Fetch-Metadata requests, including `Origin: null` with spoofed same-origin metadata. Host must match the configured numeric address and port. There is no CORS allowance. Caller headers cannot select another upstream. Redirects are rejected and control requests are never replayed by the gateway, including after ambiguous connection failure. The companion's existing Spotify refresh policy remains unchanged.

Requests have an 8-second upstream deadline and eight concurrent slots. Control bodies are capped at 1024 bytes, playback/control JSON at 8192 bytes and artwork at 18,448 bytes. Playback is decoded into protocol fields, so unknown response fields cannot leak through. BTAR headers, stride, payload length and dimensions up to 96 x 96 are checked before delivery. Keep the companion's artwork size at 96. Retry-After delta seconds are capped at 300. Only selected status headers are returned; cookies, redirect locations and raw upstream error bodies are discarded. Header/read/write/idle limits match the companion.

Prepare a separate device token in a private temporary directory without printing its value:

```fish
set -l bridge_private (mktemp -d /tmp/bottie-device-bridge-XXXXXXXX)
chmod 700 $bridge_private
python3 -c 'import os,secrets,sys; fd=os.open(sys.argv[1],os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600); os.write(fd,secrets.token_urlsafe(48).encode()); os.close(fd)' $bridge_private/device-token
```

From `companion/`, build once, then start a loopback-only instance when desired:

```fish
go build -o /tmp/bottie-device-bridge ./cmd/device-bridge
env BOTTIE_BRIDGE_TOKEN_FILE=$bridge_private/device-token /tmp/bottie-device-bridge
```

For commissioning on the observed host, the proposed destination is **`http://192.168.1.148:8788`**, on wired interface `enp5s0`. This is a proposed activation, not a listener started by preparation. The other observed address, `192.168.1.211` on `wlan0`, is not selected. Recheck the chosen address before activation because DHCP can change it. After explicit LAN authorization, the activation command is:

```fish
env BOTTIE_BRIDGE_BIND=192.168.1.148:8788 BOTTIE_BRIDGE_TOKEN_FILE=$bridge_private/device-token /tmp/bottie-device-bridge
```

Provision the same device token and exact origin into the firmware's private configuration only after that handoff is authorized. Wi-Fi credentials must be supplied for provisioning; this setup does not read saved host Wi-Fi passwords. No firewall changes are part of the proposal. HTTP does not encrypt metadata, artwork, control commands or the device bearer, so this option requires a trusted private LAN. Router isolation and actual device reachability remain unverified. A USB alternative would require a new firmware/host transport and serial ownership arrangement; the completed device client uses HTTP.

Stopping only the gateway removes this listener while the companion and Spotify session continue running. Temporary token/configuration files disappear when removed or when the temporary directory is cleared. Loss of that separate token requires reprovisioning the device. Never include its value in command arguments, reports, screenshots, URLs or committed files.

## Spotify connection

1. Register an application and the exact redirect URI in the Spotify developer dashboard. Set SPOTIFY_CLIENT_ID and BOTTIE_MODE=spotify in the server environment. No client secret is used.
2. Start Spotify playback on the intended device. Open the companion loopback page on the same computer and select Connect Spotify. Complete consent in that browser within ten minutes.
3. Check `/status`. A restart clears tokens, so connect again after restarting. The service requests only playback read and modify permissions.

Account access, app eligibility and player controls depend on Spotify's current account/app rules. Player control endpoints require Premium. Automated tests use fake upstreams. User-completed login and read-only live playback metadata and 96 x 96 artwork were verified on 2026-11-12; live controls and Alexa behavior remain unverified. See the [player reference](https://developer.spotify.com/documentation/web-api/reference/start-a-users-playback) and [development mode changes](https://developer.spotify.com/documentation/web-api/references/changes/february-2026).

OAuth uses a random state and S256 PKCE verifier, a browser-bound HttpOnly SameSite=Lax cookie, ten-minute expiry and one-use state consumption before code exchange. Authorization data is never included in playback JSON or logs. The loopback HTTP cookie intentionally has no Secure flag. Redirects from token/API/artwork upstreams are refused. Refresh preserves a previous refresh token if Spotify omits a replacement and clears invalid grants so the user can reconnect. See [PKCE](https://developer.spotify.com/documentation/web-api/tutorials/code-pkce-flow), [refresh](https://developer.spotify.com/documentation/web-api/tutorials/refreshing-tokens), and [redirect requirements](https://developer.spotify.com/documentation/web-api/concepts/redirect_uri).

Polling caches playback independently of device GET requests. It uses `/v1/me/player`, optionally resolves playlist names with `fields=name`, and caches names for five minutes (maximum 32). Inaccessible playlist names fall back to the album. Episodes and ads produce unavailable state; this milestone renders music tracks only. Inactive/restricted devices and denied actions disable controls. Both flat action restrictions and deployed `actions.disallows` wrappers are supported; repeat is conservatively disabled if either repeat mode is forbidden.

Rate limits honor `Retry-After` seconds or HTTP dates. Missing/invalid retry values and transient errors use exponential delays from 2 to 60 seconds. Delay applies across token refresh, polls, playlist reads and controls. Very large retry values are bounded at ten years. No automatic replay follows a network error for next/previous. A 401 permits one refresh and retry. A 204 playback response becomes idle. Requests are canceled at shutdown. See [rate limits](https://developer.spotify.com/documentation/web-api/concepts/rate-limits).

## Limits and status

The server has 3-second header, 10-second request-read, 15-second response-write and 30-second idle timeouts, 16 KiB headers and 32 concurrent application requests. Control bodies are at most 1024 bytes. API upstream responses are at most 256 KiB with an 8-second HTTP timeout. Artwork permits 4 MiB encoded downloads, at most 4096 pixels per dimension and 4,194,304 source pixels; the converted cache contains at most eight images and 2 MiB. Only `https://i.scdn.co/image/...` supplied by a playback response can be fetched. A failed image fetch is deferred for a minute. No general URL-fetch endpoint exists.

Artwork is fitted proportionally, without cropping or overlays, with black padding where necessary. The exact RGB565 transport and JSON limits are in [protocol v1](../protocol/README.md).

| Status | Action |
| --- | --- |
| not_authorized | Connect or reconnect through the local page |
| restricted | Check Premium, app access, scopes and active device restrictions |
| no_device | Start playback on a Spotify device |
| rate_limited | Respect Retry-After; avoid repeated controls |
| upstream_error | Check connectivity and wait for the next permitted poll |

`GET /v1/playback` remains HTTP 200 when the upstream is unavailable and sets state=unavailable with all capabilities false. The header `X-Bottie-Status` gives the diagnostic code. Stale snapshots become unavailable after 15 seconds. The last observation time is preserved so the device can reject duplicate stale data. There is no claim that a successful control response confirms audible playback; the next poll supplies actual state.

## Verification

```fish
cd companion
go test ./...
go build ./...
go vet ./...
go test -race ./...
```

Tests use original fixtures, fake clocks, local HTTP upstreams and in-memory transports. They exercise authentication, one-use/expired OAuth state, PKCE exchange, refresh retention/rotation, restrictions, backoff, bounded metadata, JSON validation, trusted image origins, aspect fit and cache limits. For combined device-core verification see [integration](integration.md). No live credentials are required.
