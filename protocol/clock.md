# Optional clock

Playback v1 may contain `clock:{"unix_ms":1794594600000,"utc_offset_min":330}` in any state. The gateway generates this object independently of the Spotify observation timestamp. Original metadata, `updated_at_ms`, `control_target` and the BTXT contract remain unchanged.

`unix_ms` is a JSON integer from 946684800000 through 4102444799999 inclusive (UTC dates in 2000 through 2099). `utc_offset_min` is a signed JSON integer from -840 through 840 inclusive. Strings, fractions, exponent notation, null fields, missing fields and values outside these bounds are invalid. A consumer ignores an absent, null or invalid optional clock without discarding valid playback. Syntactically invalid JSON remains invalid playback.

The host uses `BOTTIE_CLOCK_ZONE`, an IANA timezone name defaulting to `Asia/Kolkata`. Invalid names and the machine-dependent `Local` name fail startup. Go timezone data is embedded; the device needs no timezone database or extra network time request. The offset corresponds to the gateway's timestamp, including applicable daylight-saving rules.

The device anchors a valid clock to monotonic receipt time and draws HH:MM only when the displayed minute changes. It preserves its last valid anchor through absent/invalid clock fields and offline playback. Before the first valid anchor, or after monotonic time reversal, it displays `--:--`. A minute or offset change is cosmetic: it must not rotate the control generation, cancel a gesture, invalidate artwork or change text-mask identity. Clock rendering belongs only to the nonplaying face scene; the playing layout stays unchanged.

Consumers must not use wall-clock corrections to extend playback freshness or control validity. An older firmware can ignore the additive object and continue parsing v1.
