# Protocol compatibility baseline

Read against the unchanged `ir.altek.link` Android 0.1.6 source: Wire, Identity,
LinkService, MediaEngine, VideoCapture, VideoDisplay, Profile and Store.

## Transport

- TCP 45871, TLS 1.2 with RSA self-signed per-install identities on both sides.
- Contact ID: lower-case SHA-256 of the peer DER leaf certificate, 64 hex digits.
- Client checks expected ID; both roles verify RSA >=2048 bits and the certificate's self-signature. Schannel supplies TLS proof of private-key possession. Certificate names are not identity keys.
- Current Windows discovery includes the public DER identity certificate as base64. A receiver verifies its self-signature, RSA strength, and SHA-256 match to the advertised ID before adding it to Schannel's in-memory client-auth issuer store, then reacquires the inbound credential so Schannel sees the new issuer. The server's own same-subject leaf is deliberately excluded from that store. Each active handshake keeps the credential generation it started with. On startup Windows also verifies that the saved certificate public key matches its CryptoAPI private key and repairs a stale migrated certificate when necessary. The certificate received inside TLS is still validated and fingerprint-pinned by the application. Both Windows peers must run the corrected build.
- UDP 45872: UTF-8 JSON discovery, app=ALTEK_LINK, v=1, id, name, avatar hash.
- UDP 45879: debug-build-only diagnostics. Release compiles logging as a no-op; debug embeds the receiver selected through the ignored `.local/udphost` build input.

## Application records

Every JSON request/response and media packet uses a 4-byte unsigned big-endian
length followed by that many bytes. JSON max 65536; media max 2 MiB. TLS reads
must handle split/coalesced TCP segments and additional TLS record bytes.
No assumptions about one recv() returning one packet are made.

Requests include v=1, type, name, and operation fields:

| Type | Fields / meaning |
| --- | --- |
| hello | Return name, id and avatar hash |
| text | UUID id, Unicode text (max 8000 UTF-16 code units) |
| profile | known hash; reply hash and optional JPEG base64 image |
| offer | UUID id, kind=file/voice, filename, size, SHA-256 hash, mime |
| download | UUID id; successful JSON reply, exactly size raw TLS bytes, then framed JSON acknowledgement |
| call | UUID id, kind=audio/video/view, 32 random-byte token encoded as 64 hex characters |
| accept | Matching id/token; initiator then connects a dedicated media socket |
| media | Matching id/token and certificate identity; existing socket handed to media engine |
| hangup | Matching id and certificate identity |

The receiver does not silently download offered files. Files are staged before
advertising, authorized by peer ID and offer UUID, limited to 100 MiB, and
verified by SHA-256 before becoming completed downloads.

## Media payloads

After the media JSON acknowledgement, each frame's first byte is its kind:

| Kind | Rest of payload |
| --- | --- |
| 0 | Keepalive, no further fields |
| 1 | width:u32 BE, height:u32 BE, H.264 Annex-B SPS/PPS bytes |
| 2 | pts:u64 BE in microseconds, flags:u32 BE, H.264 access-unit bytes; keyframe flag=1 |
| 3 | PCM signed 16-bit little-endian, mono, 16000 Hz, up to 4096 bytes |

Windows serializes media writes through a per-TLS write lock; microphone,
camera and keepalive threads cannot interleave record headers/bodies. Socket
send timeout bounds a blocked producer. Decoder and playback buffers are
bounded. This beta retains TCP reliability and its head-of-line-blocking tradeoff.

## Screen-call media (kind "screen")

"screen" calls do NOT use the MediaEngine frame table above. The media socket
keeps the same 4-byte-length record framing but its payload starts with a
private kind byte: 0 = keepalive, 1 = format (h264 flag u8, width u32 BE,
height u32 BE, then SPS/PPS), 2 = frame (key flag u8, pts u64 BE us, then an
Access Unit), 3 = control (op u8: 1 tap(f32 x, f32 y), 2 swipe(f32 x1,y1,x2,y2,
int32 ms), 3 key(int32 action) all big-endian). The dialing side is the
controller: it sends control packets and decodes/displays frames. The answering
side streams its display (desktop) and applies the incoming control packets.

## Profile synchronization

Discovery advertises only the photo hash. A receiver requests `profile` when
its cached hash differs or the file is missing, with a per-peer 15-second retry
limit. A JPEG is sent only if the request's known hash differs. Stored photos
are 256x256 and <=40000 bytes; incoming content is hash-verified and dimension
checked. Clearing a photo advertises an empty hash. No image broadcast timer.

## Local storage

`%LOCALAPPDATA%\ALTEK-Link`: settings.ini, peers.jsonl, identity.der, messages,
sent, received, and avatar_<hash>.jpg. Debug builds also create debug.log. Messages have independent JSON
files and creation timestamps; enumeration is sorted by timestamp on load.
Settings and data are outside the source/build directory and survive replacing
the EXE. Files use a temporary-write/rename path for complete records.
