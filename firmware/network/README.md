# Opt-in companion transport

This component supplies Wi-Fi station setup and bounded HTTP/HTTPS requests for the playback protocol. It is disabled by default. Including the component or calling a disabled build cannot join Wi-Fi. No verification command connects to a network or accesses the device.

The application selects live mode separately and enables `CONFIG_BOTTIE_NETWORK_ENABLED=y`. Live mode must disable fixture inputs and synthetic controls. Only a deliberate input accepted by `Model::intent` should call `Client::control`.

## Integration contract

Add this directory to `EXTRA_COMPONENT_DIRS`, and require component `network`. Include `bottie/network.hpp`.

- One background task owns `Client::start`, `playback`, `fetch_asset`, `control`, `artwork`, `text_mask`, `status` and `stop`. `start` initiates asynchronous station connection; successful return means setup started, not connected playback. Poll `playback` approximately once per second. Never call these blocking methods from the render loop.
- `playback(output, capacity, length)` accepts a buffer of 8193 bytes for the full 8192-byte JSON bound plus terminator. Success returns parsed, structurally valid JSON before any optional asset request. An unavailable snapshot remains a successful transport response. The core model decides freshness and capabilities. On false, call `Model::disconnected`; ignore buffer contents and old artwork.
- `artwork()` returns a borrowed view, valid only until the next `playback`, `fetch_asset` or `stop`. Copy its `stride * height` bytes in the worker before another request. Publish a typed snapshot and copied artwork under a short handoff lock; no lock spans a download. The renderer owns its own copy. A missing image is normal and must clear the renderer's previous cover.
- `control` accepts the JSON produced by `encode_intent`, up to 1024 bytes. It sends one POST only. HTTP 200 plus `{"ok":true}` means accepted. Poll playback after success. A timeout may follow a command the host accepted, so never automatically replay it. This client does not generate controls.
- `cancel()` and `interrupt_assets()` may be called concurrently. Cancellation is sticky until `start`; the worker exits its request at the next nonblocking I/O/deadline check. Join or acknowledge the worker before `stop` or destruction. `status()` is owner-task-only and exposes a secret-free enum.

Use a 24 KiB network worker stack initially: the existing snapshot parser has nested fixed token/snapshot storage. The renderer must continue local animation while the worker reconnects or times out.

## Private configuration

There are no credentials in Kconfig or committed defaults. Create `firmware/network/network.local.hpp` locally with mode 0600. Its ignore rule is component-local. Do not add it to Git. Define the function below with your private values; the blank example remains disconnected:

```cpp
#pragma once
namespace bottie::network {
inline Config local_config() {
    Config config;
    config.ssid = "";
    config.password = "";
    config.origin = "";
    config.device_token = "";
    config.allow_insecure_http = false;
    return config;
}
}
```

SSID is 1..32 bytes and the WPA2/WPA3 password is 8..63 bytes. Open/WEP networks are rejected. Origin is an exact `http://IPv4[:port]` or `https://IPv4[:port]`, without path, userinfo, query, or fragment. Numeric unicast IPv4 avoids DNS waits; hostnames, mDNS and IPv6 are deliberately unsupported in this version. Reserve the companion's LAN address through the router if needed.

HTTP requires explicit `allow_insecure_http=true` and is suitable only for a trusted isolated LAN: its bearer token and playback data are unencrypted in transit. HTTPS requires `server_certificate_pem` pointing to a long-lived PEM trust certificate. The server certificate must match the configured IP address, and certificate validity needs an accurate system clock. Verification is never disabled. This component does not set time or provision certificates.

The device token must be a separate 16..256-byte bearer token using letters, digits, `-._~+/=`. It authenticates all device endpoints; it is never a Spotify access or refresh token. Spotify credentials remain on the companion. Wi-Fi configuration uses RAM storage with Wi-Fi NVS disabled. Credentials are still embedded in any locally built firmware image, so keep configured build outputs private and ignored. Do not enable verbose third-party networking logs when using private credentials. This component emits no credentials, request headers, or response bodies to logs.

The component exclusively owns Wi-Fi station setup and refuses an already initialized Wi-Fi driver/default station interface. Integrate shared Wi-Fi ownership explicitly before combining it with another station manager. Configuration strings are copied except the certificate, which must outlive the client.

Numeric commissioning logs distinguish station start, association, address acquisition, driver disconnect reasons, connect return codes and client timeouts. They include event counts, channel, authentication mode and disconnect RSSI, without network names, hardware addresses or credentials. The end event's timeout flag identifies a client timeout. It prints after the disconnect call; callbacks can arrive before or after that line.

## Bounds and failures

Each connection, headers and body share a 6-second monotonic deadline. ESP-TLS uses nonblocking sockets and 20 ms connection polling. Cancellation does not close a socket from another task. A playback call fetches metadata only, capped at6 seconds. `fetch_asset(snapshot, ticket)` is a separate optional request capped at6 seconds. Publish valid metadata before calling it. No redirects, auth retries, compression, or automatic POST retries are supported.

The HTTP reader accepts Content-Length or chunked framing, rejects conflicting lengths, limits total headers to 4096 bytes, a line to 511 characters, chunks to 1024 and trailers to 1024 bytes. Errors are handled by status without exposing error bodies. Retry-After delta seconds is clamped to 300; HTTP-date values are ignored. Transport failures back off 1,2,4,8,16,32 seconds. Valid playback clears transport failure history. Wi-Fi attempts last up to 15 seconds before the same bounded backoff. Continuing worker polling is required to drive reconnects.

Artwork paths must be `/v1/artwork/<id>` or that exact path under the configured origin. IDs allow letters, digits, underscore and hyphen. Foreign origins, traversal, encoded paths and query strings are rejected before sending the token. The single cache stores at most 18448 bytes, including the 16-byte BTAR header, and accepts dimensions no greater than 96 x 96 matching the snapshot. It checks version, format, stride, payload length and exact body size. Larger host artwork is declined safely; configure the companion's default 96-pixel artwork. URL/dimension changes discard the previous cover. Failed fetches clear the view and retry with bounded backoff.

## Reproduce verification

Run from repository root with the existing compiler and pinned SDK:

```fish
/usr/bin/fish --no-config firmware/network/tools/check-native.fish
/usr/bin/fish --no-config firmware/network/tools/check-idf.fish
python3 firmware/network/tools/check-scope.py
```

Native tests use in-memory HTTP streams and address/undefined sanitizers. The IDF check generates a private temporary project in `/tmp/bottie-network-idf-*`; it compiles the active component for S3 with a 240 x 296 framebuffer, two artwork handoff/render buffers and no PSRAM. An optional `BOTTIE_NETWORK_CHECK_DIR` must name a private 0700 directory with that prefix. ESP-IDF 6.1 must be installed under `$HOME/.local/share/bottie/esp-idf-v6.1` at commit `fff9895c82d744c7237be8847347bdd1b07c6643`. Tools remain under `$HOME/.local/share/bottie/idf-tools-v6.1`.

The temporary 2 MiB flash configuration only checks linkage. It is not the device's measured flash size or a deployment image. The helper does not run generated upload commands. No flashing or live Wi-Fi is part of these checks.

Linked size does not establish runtime heap headroom. Account separately for the network cache, task stacks, Wi-Fi packet buffers and TLS handshake allocations. The complete integrated application's size, free internal heap, largest block and stack high-water marks still need measurement during a separately authorized runtime test. No external PSRAM is assumed.

References: [ESP-IDF 6.1 ESP-TLS source](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/esp-tls/esp_tls.h), [Wi-Fi API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/network/esp_wifi.html), [network interface API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/network/esp_netif.html). The companion's `protocol/README.md` defines payload semantics.

## Metadata text masks

`text_mask(snapshot)` returns a borrowed parsed view only when its exact track/title/artist/album/context/descriptor identity matches that snapshot. Copy the full encoded body before the next playback or asset call, as with artwork. The HTTP body is capped at8494 bytes and parsed as fixed BTXT v1 A4 planes; the URL SHA256 is verified with the SDK PSA crypto implementation. Native verification uses OpenSSL. No font files or shaping code run on the device.

Text accepts only the exact relative `/v1/text/<64 lowercase hex>` path. No absolute URL, query, fragment, percent encoding or path traversal is accepted. Invalid descriptor, unavailable bundle, corrupt digest, wrong geometry or unsupported layout clears the mask without rejecting otherwise valid playback. Rendering uses an ellipsis placeholder for missing non-native lines. Partial planes are accepted only for names requiring shaping, preserving native ASCII and punctuation pixels.

Text has a separate bounded1..32-second failure backoff (Retry-After at most300 seconds). Metadata is still polled and controls still processed when text fails. Each optional service opportunity fetches at most one asset; text gets the first opportunity and artwork the next when both need data. Alternating opportunities prevent failed text from starving the cover. No frame or band starts downloads. A successful cached mask needs no request until metadata/descriptor changes. Identity changes invalidate the old mask before any fetch, and late commit for an obsolete snapshot is rejected.

## Priority and cancellation

The worker checks the guarded FIFO/coalesced-volume mailbox before polling metadata and before optional assets. A successful enqueue increments an atomic asset-interrupt generation and wakes the worker. It captures the asset ticket before inspecting the mailbox, so an enqueue between the check and starting I/O cannot be lost. The exact `RequestBudget`/`read_until` policy checks that generation before every nonblocking read, including waits before any header or body byte arrives. An interrupted optional response is discarded without marking playback offline or imposing a failure backoff. Only the owner task closes the connection. Optional results publish only while the unchanged worker snapshot remains fresh.

At100Hz the configured retry sleep is one10ms tick; the explicit TCP writable select waits at most10ms, and nonblocking ESP-TLS connection setup uses20ms timeout. These are configured polling bounds, not measured target end-to-end cancellation latency; SDK cleanup, TLS and scheduling still need physical timing. Required metadata and an already submitted control request retain their6s deadline and are not preempted or replayed. A control can therefore still expire behind a slow required metadata response. The unchanged2s control TTL, target-generation validation, four-button FIFO and1s latest-volume rate bound remain authoritative.

Metadata is published before assets; a new copied mask/cover publishes separately. Short handoff locks never span network I/O. The render task's owned snapshot and buffers are changed only before the next pass. This preserves language masks across row bands while preventing an obsolete cover from surviving an identity change.
