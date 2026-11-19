# Validation — ALTEK Link

## Current owner acceptance — 2026-11-15

- Current source identity is Android 0.9.0 build 1 (`versionCode` 44).
- The owner reports that almost all normal cross-platform behavior is working in practical testing, including large-file transfer, calls, screen sharing, remote control, keyboard input, and recent queue/keyframe improvements.
- This acceptance is not a complete device/API/OEM matrix. Keep the historical reproducible build evidence below until a new formal artifact-validation pass replaces it.
- Build 24 was signed, installed over existing data on a Huawei Y520-U22, and the full existing JVM suite passed. Owner testing confirmed direct H.264 root capture and low-latency persistent root input; 1080p zoom quality remains limited by the device decoder.
- The 0.9.0 build 1 release APK was signed and verified with APK signature schemes v1, v2, and v3 on 2026-11-20; the full existing JVM suite passed.
- Standalone Baseline decode probes returned full 640x360 and 1280x720 output. A 1920x1080 input returned format 1920x540, `height-ratio=2`, stride 1920, slice-height 540, and 1,555,200 output bytes. This is the evidence for the planned Y520-only 720p receive cap.

## Historical artifact validation — 0.1.16

Completed on 2026-10-30:

- Compiled with Android API 34, targetSdk 33, minSdk 16, Java 7 bytecode, and D8 min API 16.
- Verified APK alignment and v1/v2/v3 signatures. The v1 signature preserves API 16 compatibility.
- Passed the core JVM tests for UTF-8/emoji, framing, bounds, persistent identity,
  TLS/certificate pinning, NV21 conversion, and front-camera rotation.
- Passed UI logic tests for geometry, online ordering, and the current 15 fps / 4,000,000 bit/s
  video defaults and bounds.

Current artifact:

- APK SHA-256: `2b00daf2da39b44a555d3061d30bb2aa9173f103e8ab522b48014571b05e28e8`
- Signing certificate SHA-256: `5add42367fd5f284aead720e172a555b942c19e0bcc7582b6ae759ee48b4929d`

The current release APK was signed with the existing local development identity to preserve update
compatibility during validation. The private keystore is not part of the repository. Before the
first public release, decide whether this certificate is the permanent release identity and keep
its private key securely backed up outside source control.

At the time of that artifact validation, still pending were installation and warning behavior on current Android devices, actual settings and
camera enumeration, custom camera/encoder combinations, runtime codec fallback, ringtone/OEM
behavior, and end-to-end interoperability with both current Windows builds. Minimum API support is
not a guarantee that every legacy camera or codec works.
