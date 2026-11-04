# Validation — ALTEK Link 0.1.16

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

Still pending: installation and warning behavior on current Android devices, actual settings and
camera enumeration, custom camera/encoder combinations, runtime codec fallback, ringtone/OEM
behavior, and end-to-end interoperability with both current Windows builds. Minimum API support is
not a guarantee that every legacy camera or codec works.
