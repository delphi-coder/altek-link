# ALTEK Link protocol v1

This is the current shared interoperability contract, derived from both active source trees on 2026-10-25. Links point to implementation evidence. “Implemented” means code exists, not that real-device interoperability was executed in this pass.

## Discovery and presence

Implemented by [`network.c`](../windows/src/network.c) (`discovery_loop`, `announce`) and [`LinkService.java`](../android/src/ir/altek/link/LinkService.java) (`discoverLoop`, `announce`).

- UDP port 45872; receivers accept at most 2048 bytes and decode UTF-8 JSON.
- Packet: `{"app":"ALTEK_LINK","v":1,"id":"<64 lowercase hex>","name":"...","avatar":"<empty or 64 lowercase hex>","cert":"<optional base64 DER leaf>"}`. Current Windows peers include `cert`; older peers omit it.
- `id` is a discovery hint only. The authenticated peer ID is recalculated from the TLS leaf certificate.
- Before using a discovery certificate as a Schannel client-auth issuer, Windows verifies that it is self-signed RSA >=2048 bits and that SHA-256 of its DER bytes exactly equals the advertised `id`. The certificate only allows the TLS exchange to complete; `verify_peer` still authenticates and pins the certificate received inside TLS.
- Both sides announce every 4 seconds to global/interface broadcast, known unblocked peers, and discovered gateway targets. A peer is online for 16 seconds after its last sighting.
- Manual probe uses an IPv4 address, TLS, and a `hello` request; `hello` returns `ok`, `name`, `id`, and `avatar`.

## TLS identity, connection, and authorization

Implemented by [`tls.c`](../windows/src/tls.c), [`Identity.java`](../android/src/ir/altek/link/Identity.java), and each platform's request dispatcher.

- TCP port 45871, TLS 1.2 only, mutual certificate presentation, one control operation per connection. Media uses a separate TLS connection.
- Each install owns a self-signed RSA certificate/key (minimum 2048-bit RSA). Peer ID is lowercase SHA-256 of the DER leaf certificate.
- Both implementations verify the leaf is self-signed and RSA >=2048 bits. Outgoing connections additionally require the certificate ID to match the selected stored contact. Certificate subject/hostname and discovery `id` are not trust anchors.
- A newly seen key becomes a contact allowed for messaging unless locally blocked. Android's legacy `approved` column now means “not blocked”; camera-view permission is separate. Windows persists `blocked` only.
- Fingerprint comparison is the available human identity check; names and avatars are not authentication.
- Windows keeps the CryptoAPI key container and certificate metadata outside the normal app-data directory when possible; Android attempts an external-storage backup and otherwise uses private app storage. Persistence across uninstall is therefore platform/device-policy dependent.

## Framing, encoding, and request envelope

Implemented by [`tls.c`](../windows/src/tls.c) (`wire_send`, `wire_read`), [`Wire.java`](../android/src/ir/altek/link/Wire.java), [`network.c`](../windows/src/network.c), and `LinkService.request`/`handle`.

- Every JSON or media record is `length:u32 big-endian` followed by exactly that many bytes. Implementations use signed 32-bit storage but reject negative lengths.
- JSON maximum: 65,536 bytes. Media/screen record maximum: 2 MiB. Text encoding is UTF-8; binary numeric fields described below are big-endian unless stated otherwise.
- Control request envelope: `v:1`, `type`, `name`, plus operation fields. Response is framed JSON with `ok:boolean` and usually `error:string`; some successful responses add fields.
- Unknown protocol versions and operations are rejected. There is no negotiation beyond exact `v=1`; additive fields are ignored by tolerant JSON readers.

| Type | Required/meaningful fields | Behavior |
| --- | --- | --- |
| `hello` | envelope only | Introduces identity metadata; allowed even when blocked. |
| `text` | `id` UUID, `text` <=8000 UTF-16 code units | Persist before positive response; duplicate UUID from the same peer is acknowledged without redisplay. |
| `profile` | `known` avatar hash or empty | Returns current `hash` and optional base64 `image`. |
| `offer` | UUID `id`, `kind`=`file`/`voice`, `filename`, `size`, SHA-256 `hash`, `mime` | Persists an offer; never downloads automatically. |
| `download` | UUID `id` | Positive JSON, exact raw file bytes (not length-framed), then framed receiver ACK. |
| `call` | UUID `id`, `kind`, 64-hex `token` | Reserves the single call slot. |
| `accept` | matching `id`, `token` | Callee accepts; caller opens media connection. |
| `media` | matching `id`, `token` | After positive response, the socket switches to media or screen records. |
| `hangup` | matching `id` | Ends the matching authenticated peer/session. |

## Messaging acknowledgment and retry

Implemented by `network.c` (`flush`, `send_pending`, `handle`) and `LinkService` (`text`, `flush`, `handle`) plus each store.

- Sender creates a UUID and persists state `queued` before transmission.
- Receiver validates and persists the message before replying `ok:true`. A duplicate UUID/content pair is ACKed without another notification; Android explicitly rejects an identity collision with different content. Windows deduplicates by UUID+peer and does not compare duplicate content.
- A successful response changes outgoing state to `delivered`. Failure leaves it queued.
- Both sides retry queued items from the periodic 4-second tick only while the unblocked peer is online; retries are ordered per peer. There is no explicit retry count, expiry, or backoff beyond presence/tick gating.

## File and profile exchange

Implemented by [`files.c`](../windows/src/files.c), [`image.c`](../windows/src/image.c), `LinkService.offer`/`download`, and [`Profile.java`](../android/src/ir/altek/link/Profile.java).

- Files are staged before `offer`, capped at 100 MiB, owned by UUID+recipient, and SHA-256 verified after exact-size download. A failed/retried download restarts from byte zero; no resume protocol exists.
- The final receiver ACK lets the sender mark `transferred`. An offer ACK marks only `offered`, not downloaded.
- Windows currently sends `mime:"application/octet-stream"` for every outgoing offer and does not persist incoming MIME; Android resolves and preserves MIME. This is compatible but loses metadata in Windows-originated transfers.
- Avatars are JPEG, exactly 256x256, <=40,000 bytes, named/content-addressed by SHA-256. Discovery carries only the hash. `profile` returns `hash` and includes base64 `image` only when the request's `known` differs.
- Profile sync is limited to one active request per peer and retried no sooner than 15 seconds. An empty authenticated hash clears the association.

## Calls and lifecycle

Implemented by [`call.c`](../windows/src/call.c), `LinkService.dial`/`handle`, [`MediaActivity.java`](../android/src/ir/altek/link/MediaActivity.java), and [`IncomingCallActivity.java`](../android/src/ir/altek/link/IncomingCallActivity.java).

- One active call slot per app. `id` is a UUID and `token` is 32 random bytes encoded as 64 lowercase hex characters.
- Kinds shared by both sides: `audio`, `video`, `screen`. Android also implements `view`, an auto-accepted remote-camera mode gated by an armed UI state and per-contact camera permission.
- Windows explicitly rejects every incoming `view` call with `CAMERA_NOT_ARMED`; Windows therefore cannot serve as an Android-style armed camera endpoint.
- Normal calls require explicit acceptance. After `accept`, the caller opens `media`; both session ID/token and TLS peer ID must match. Setup without a media socket expires after 45 seconds. Either side can send `hangup`.
- Android owns incoming ringing for at most 45 seconds. Leaving ordinary `MediaActivity` stops that media session; Android screen sharing is service-owned and may continue outside its activity.

## Ordinary audio/video records

Implemented by [`media.c`](../windows/src/media.c), [`camera.c`](../windows/src/camera.c), [`codec.c`](../windows/src/codec.c), [`MediaEngine.java`](../android/src/ir/altek/link/MediaEngine.java), `VideoCapture`, and `VideoDisplay`.

| Kind byte | Payload after kind |
| --- | --- |
| 0 | Keepalive; no fields. |
| 1 | `width:u32`, `height:u32`, then H.264 Annex-B SPS/PPS configuration. |
| 2 | `presentationTimeUs:u64`, `codecFlags:u32`, then H.264 Annex-B access unit. Keyframe flag is bit/value 1. |
| 3 | Signed PCM16 little-endian, mono, 16,000 Hz; payload is even-sized and <=4096 bytes. Normal capture chunk is 640 bytes (20 ms). |

- Idle writer keepalive is every 2 seconds; media socket read timeout is 15 seconds.
- Android defaults to nearest 320x240 camera preview, 15 fps target, 4,000,000 bit/s request, one-second keyframe interval; settings are per camera and driver/codec results may differ. Windows defaults are 320x240, 15 fps, 4,000,000 bit/s, one-second intra period.
- Windows requires dynamically loaded OpenH264 2.4.1 for video. Android uses MediaCodec. This is TLS/TCP media, not RTP/SRTP; there is no adaptive bitrate, retransmission layer beyond TCP, or jitter-clock resampling.

## Screen records and controls

Implemented by [`screen.c`](../windows/src/screen.c), [`ScreenEngine.java`](../android/src/ir/altek/link/ScreenEngine.java), `ScreenCapture`, `RemoteControl`, and `RemoteControlActivity`. `screen` reuses the outer length framing but not the ordinary media payload layout.

| Kind | Payload |
| --- | --- |
| 0 | Keepalive. |
| 1 | `h264:u8`, `width:u32`, `height:u32`, then SPS/PPS when H.264; JPEG mode has no config. |
| 2 | `keyframe:u8`, `presentationTimeUs:u64`, then frame bytes (H.264 AU or JPEG according to format). |
| 3 | `op:u8`, then operation-specific fields below. |

Control numbers and big-endian fields:

- 1 tap: `x:f32,y:f32`; 2 swipe: `x1,y1,x2,y2:f32,durationMs:i32`; 3 navigation key: `action:i32` (1 back, 2 home, 3 recents).
- 4 pinch: `centerX,centerY,scale:f32`; 5/6/7 left mouse down/move/up and 8/9 right down/up: `x,y:f32`; 13 keyboard: `virtualKey:i32,down:i32`.
- Windows additionally sends/handles 10 vertical wheel and 11 horizontal wheel: `x,y:f32,delta:i32`. Android `ScreenEngine.readLoop` does not handle 10/11, so wheel input from a Windows controller is currently ignored by Android. Both sides handle 1-9 and 13 as receivers, although not every platform UI emits every operation.
- Coordinates are normalized 0..1. Screen frame queues deliberately drop old frames under backpressure; control queues are bounded and treated as higher priority.
- Windows desktop sharing uses OpenH264 `SCREEN_CONTENT_REAL_TIME`; `screen_quality=1` selects quality rate control (`0` selects bitrate control). `%LOCALAPPDATA%\ALTEK-Link\settings.ini` stores `screen_width`, `screen_height`, `screen_fps`, `screen_bitrate`, `screen_quality`, `screen_iframe` (frame count), `screen_threads`, and `screen_frame_skip`; the defaults are native desktop size, 4 fps, 4,000,000 bit/s target, quality control, a 4-frame intra period, one encoder thread, and frame skipping enabled. These are local encoder policy, not wire fields, and changes take effect after restart. The optional raw `screen.h264` diagnostic dump is compiled out by default. A commented reference file is checked in as `windows/settings.example.ini`.

## Versioning, compatibility, and unresolved evidence

- The wire version remains 1 across Android and Windows; profile and screen-control additions are additive under that version. Older peers may ignore unknown JSON fields or control operations.
- Windows 0.1.7 corrected its outgoing screen format/frame layout to match Android. The historical [`CHANGES-0.1.7.md`](../windows/CHANGES-0.1.7.md) says it had not been rebuilt at that time; the current source subsequently built successfully with `build-vs2010.bat x86` and `build-tdm.bat x64` on 2026-10-30. Runtime interoperability remains pending.
- Android release identity is 0.1.16 throughout the active source. This does not alter wire `v=1`.
- Existing architecture/test reports describe intended and earlier validated behavior. Where they disagree with active source (for example older 10 fps/250 kbps video defaults or a shorter screen-control table), this document follows the active source.
