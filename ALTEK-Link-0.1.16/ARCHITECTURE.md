# ALTEK Link v1 transport / code map

One APK, API 16 minimum, target API 33, Java 7 bytecode, compiled against platform API 34. Newer calls remain guarded so the same APK runs on legacy devices. No cloud dependency, no external runtime JAR, no server to install.

## Classes
- `MainActivity`: native contacts/chat/settings, permissions, sharing, voice notes.
- `Store`: SQLite peers/history/outbox. Last 200 messages displayed; full history retained.
- `Identity`: per-install RSA2048 private key and self-signed SHA256withRSA certificate, TLS context, certificate-derived ID.
- `Wire`: bounded length-prefixed UTF-8 / byte frames, hashes and close helpers.
- `LinkService`: foreground network lifecycle, discovery, authorization, message/file/call protocol and retry queue.
- `BootReceiver`: restarts network only when the user's enabled preference is true.
- `MediaActivity`: visible call screen and explicit camera-host mode.
- `MediaEngine`: duplex media framing, audio I/O, bounded queues, keepalive.
- `VideoCapture`: camera1 NV21 -> planar/semiplanar H264, worker-owned resources, actual FPS throttle.
- `VideoDisplay`: H264 decode onto Surface, bounded work, keyframe recovery after overload.
- `AttachmentProvider`: read-only, granted content URIs to external viewers.
- `DebugLog` / `LinkApp`: build-selected diagnostics; release uses a no-op implementation and debug uses a bounded UDP queue.

## Ports
- TCP 45871: TLS1.2. Per-operation control connections; file/media connections are separate.
- UDP 45872: discovery. JSON <= 2048 bytes: app, v, id, name, avatar (empty or SHA256). Period 4s, online threshold 16s.
- UDP 45879: debug-build-only plaintext technical logs. The receiver is taken from the developer's ignored `.local/udphost` at build time; release contains no logging implementation or UI.

Discovery is untrusted. Every TLS operation derives peer ID from the presented certificate, not a claimed JSON field. The key must match the selected contact for outgoing connections. Incoming self-signed certs are signature checked at TLS layer; new contacts are allowed for local messaging by default (schema v2). The legacy approved column now means not blocked. Blocked peers cannot perform operations other than identity introduction. Optional fingerprint comparison is required to authenticate a person beyond the network-discovered key. Certificate validity is deliberately broad for offline clock tolerance; this is local key pinning, not public PKI hostname authentication.

## Control frames
Big-endian unsigned-in-practice signed int32 byte length, bounded to 65536, followed by UTF-8 JSON. Requests contain `v=1`, `type`, `name`. New socket per request is intentional in this beta; pooling is a later optimization.
- hello -> name and ID introduction, no private data.
- text -> UUID id, text <= 8000 Java chars. Persist before ACK. Duplicate id/content ACK without redisplay.
- offer -> UUID, file/voice, filename, size <=100MiB, SHA256, MIME. Receiver gets an offer, not automatic content.
- download -> UUID. Sender authorizes ownership and recipient; OK then exact offered bytes, receiver verifies SHA256 then ACKs. No partial-resume; retry starts over.
- call -> UUID, audio/video/view, 32-byte random token encoded hex. Unblocked contact only; one call slot; 45s setup expiry.
- accept -> UUID/token. Normal calls require UI answer; view requires armed camera mode + per-contact camera authorization.
- media -> UUID/token on separate TLS socket. Must match accepted peer/session. Ownership transferred to MediaEngine.
- hangup -> UUID, matching authenticated peer only.

## Media frames
Length prefix <=2MiB, then one byte type:
- 0 heartbeat (sent every 2 seconds of idle; socket read timeout 15s)
- 1 int32 width, int32 height, remainder AnnexB SPS/PPS configuration
- 2 int64 presentationTimeUs, int32 codec flags, remainder H264 access unit
- 3 PCM signed 16-bit little-endian mono, 16000 Hz, normally 20ms per packet

Video default 320x240 (nearest supported preview), 15fps input throttle, 4,000kbps requested, one-second keyframe interval. Capture drivers can vary bitrate. No RTP, SRTP, adaptive bitrate, jitter-clock resampling or congestion algorithm is claimed. TLS/TCP is the beta transport choice. Media outgoing queue is bounded to 64 frames; audio playback queue to 16; decoder tasks to 12. Overload triggers drop/keyframe wait or explicit failure instead of unbounded memory growth.

## Lifecycle / known boundaries
Network service is foreground, START_STICKY, not bound to Activity. Boot restart follows enabled preference. OS Force Stop/Doze/OEM power rules still apply. Wake/Wi-Fi locks improve readiness with battery cost. Network uses a bounded worker pool. Camera and microphone only begin from an explicit visible MediaActivity; leaving it ends the call. Camera mode is one approved viewer, foreground screen kept on.

## Build and validation
No emulator or physical Android run was performed here. Validation report states exact checks. Real APK tests are required on Y520, A70, J5 2016 and Y3 II, especially TLS provider compatibility, hardware decoder, microphone/AEC, lifecycle and hotspot discovery.


## 0.1.2 profiles and UI
- MainActivity now has a single 56dp toolbar per screen, content-sized contact rows with minimum80dp, separate chat content, icon calls and one composer row. Overflow menus contain settings, camera view and contact controls.
- Icons uses native Canvas paths, not font glyphs; raster launcher resources cover mdpi through xxxhdpi.
- CropActivity samples a max25MiB source to at most1024px per dimension, handles JPEG EXIF orientation, pan/pinch/rotate, and draws a 256px square crop. A circular mask previews avatar display. Profile stores JPEG <=40000 bytes.
- Profile / AvatarView: SHA256 content-addressed private files and memory bitmap cache; cached photos display offline. Unreferenced image files are cleaned up after profile changes.
- Discovery avatar is only a hint. profile control request includes known hash; authenticated pinned TLS response includes hash and optional base64 image. SHA256, byte cap and exact256x256 dimensions are checked before caching. Empty hash deletes the peer association only after TLS response.
- No image data in periodic broadcasts. At most one request per peer, failures/changed hints throttled15s. Same stored hash with an existing file requires no request. Missing files are re-requested. Current metadata is returned even if the discovery hint was stale.
- Profile is available to unblocked LAN contacts. Name/avatar do not authenticate human identity. Block removes camera access. Old database entries migrate to messaging-allowed, preserving camera flags; new contacts default camera=false.
- Profile transfer is additive to protocol v1; old app versions ignore avatar hints and still have their old trust gate. Upgrade both phones for testing.


## 0.1.3 display fixes
- Contact text uses WRAP_CONTENT height rather than the previous56dp container; both lines retain font padding and can grow with system font scale.
- PeerOrder sorts a snapshot of presence time: online first, case-insensitive name, ID tie-break. Row colors reflect presence.
- Header name/avatar launch a cancelable photo dialog; outside image and Back dismiss it.
- MediaActivity owns a TextureView Surface, with explicit hardware acceleration. VideoDisplay reports source dimensions through MediaEngine.Listener. VideoFit maps the stretched texture to an aspect-fit90CW rectangle, with letterboxing and no CPU video re-encoding. Its corner geometry is JVM-tested in portrait, landscape and square viewports.
- Texture surface destruction stops media and releases the Surface. This new display path still requires hardware tests on the legacy devices.
- API reference: https://developer.android.com/reference/android/view/TextureView


## 0.1.4 camera and controls
Front-facing CameraInfo triggers a180-degree half-turn fused into NV21-to-I420/NV12 conversion. Reverse luma samples and chroma-pair positions while preserving U/V channel mapping. No extra per-frame allocation; rear camera conversion and wire protocol unchanged. Combined with receiver90CW, this corrects the user-reported upside-down selfie feed. Not a general orientation/mirroring framework.
Chat ImageButtons explicitly FIT_CENTER rather than relying on intrinsic24-pixel centering. Media controls share a56dp row with32dp icons and actual mute/speaker/torch state indicators. Status is a10sp single line; full text remains accessible on tap.


## 0.1.5 incoming alert lifecycle
IncomingRinger uses the system default Ringtone on the main handler, restarting when needed, with a45-second upper bound and a generation guard against stale starts. It observes ringer mode/STREAM_RING volume, vibrates only in vibrate mode. Pending normal calls start it; answer, clearCall and service destruction stop it. Camera-view mode does not ring. Incoming-call notification ID3 uses a separate silent notification channel; actual ringtone is service-owned. Receiver notification ID1 offers explicit Stop receiver. Persistent CPU/Wi-Fi locks are unchanged.


## 0.1.6 per-camera configuration
VideoOptions stores independent preferences by Camera1 ID. VideoSettingsActivity queries/release each actual camera on a worker, showing preview sizes, FPS ranges, focus modes, and raw formats. Raw formats are informational; NV21 remains the capture input. Settings are rejected during active calls and applied at next capture open (including Flip).
VideoCapture keeps legacy defaults, applies validated requested values, reads back dimensions, and throttles encoder input by monotonic frame period. Nondefault startup failures release camera/codec resources and retry once using defaults, logging VIDEO_FALLBACK without overwriting preferences. Camera and encoder capabilities are different; no claim that every advertised size/range combination works. A conservative heap guard rejects excessive frame allocations. VIDEO_CAPTURE and video_actual preferences retain the last running configuration; bitrate is a request, not measured output.
Build scripts sign into a separate staging output, verify minSdk16 signatures, then promote it. install.bat verifies signatures and requires explicit ADB Success, checking error text and exit status. Final release validation verifies the APK extracted from the ZIP and compares its certificate against0.1.4.
