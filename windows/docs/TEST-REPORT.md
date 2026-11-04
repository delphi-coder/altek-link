# Verification report — Windows 0.1.7

## Current-source follow-up — 2026-10-30

- Current artifact identity is version 0.1.7, build 7. `include/build_number.h` is the build-number
  source; the value is shown in the main title, Settings, and the `START` log.
- `build-vs2010.bat x86` completed successfully and produced `bin\x86\AltekLink.exe` after the full-screen, quality-mode screen encoder, INI setting, and disabled raw H.264 dump changes.
- `build-tdm.bat x64` completed successfully and produced `bin\x64\AltekLink.exe` from the same source.
- Both release and debug executables expose file/product version `0.1.7.7` in their Windows version resources.
- Current release hashes are:

  | Target | EXE size | SHA-256 |
  | --- | ---: | --- |
  | release x86 | 237,568 bytes | `359578d492b4212fba25f672815398f9c735fa5da14e4043b404d52b54b14c67` |
  | release x64 | 244,736 bytes | `83334e0b714fe41f19afe24a1f26db9500ea707be08474d38758d7643839633e` |
  | debug x86 | 266,752 bytes | `cd4032724b1f9491db8310760dea0c24de9fdd967961aaf002a347b3d9c0315e` |
  | debug x64 | 923,280 bytes | `52cf66d352e2e574adb812007e29684ea6a59619c896c34e736844d140dd1063` |
- Build 3 adds the themed default avatar, revised telephone icon, and the default-enabled
  `minimize_screen_share` option. Startup was verified to persist `minimize_screen_share=1` in the
  live INI and log `Windows client 0.1.7 (build 3)`. The visual result and minimize-before-capture
  ordering still require confirmation in a real incoming screen-share session.
- Build 4 refines the default avatar to match the supplied simple head-and-shoulders reference and
  redraws the audio-call icon as a diagonal handset with three sound-wave arcs. Both official x86
  and x64 build scripts completed successfully; exact visual fit still requires confirmation in
  the running UI at the configured Windows scaling.
- Use these batch files as the build entry points. A running `AltekLink.exe` locks its output path; if linking reports that the output cannot be opened or replaced, close the running application and retry the same batch command.
- VS2010 emitted an existing const-qualifier warning in `tls.c`, plus `LNK4068` defaulting the target to x86. TDM-GCC x64 linked successfully. No warning from the new build-number or call-window flicker changes was reported.
- The call window now uses `WS_CLIPCHILDREN`, changes control visibility only when call state changes,
  and excludes the bottom control strip from its fixed-timer repaint. Visual flicker still requires
  confirmation during a real remote-desktop session.
- No runtime screen-sharing, quality/bitrate behavior, INI reload, full-screen keyboard-hook, corner-exit overlay, Windows hardware, or Android interoperability test was performed in this follow-up.
- Both official x86 and x64 builds complete successfully. The emitted discovery packet was captured with a certificate whose DER SHA-256 exactly matches the advertised ID. Detailed logging localized the real two-PC failure to server handshake step 2 while processing the client's certificate/key proof. A controlled loopback used the BABA-PC discovery certificate in the server trust store but a known-good local client identity; Schannel completed mutual TLS and reached the expected application fingerprint rejection. This proves the framing/server path and local certificate proof are valid. The corrected build therefore excludes the server's own same-subject leaf from the client-auth store and verifies/repairs certificate-to-private-key mismatches caused by identity migration. End-to-end calling requires deploying this corrected executable to both Windows peers.

The remainder of this report records the older Windows 0.1.0 verification and is retained as historical evidence.

## Completed

- Full x86 and x64 compile/resource/link with MinGW-w64 GCC 13.2 (cross compiler). Both final builds completed without diagnostics.
- Compiled all application C with gnu90 and `-Werror=declaration-after-statement`, enforcing declaration placement compatible with older C compilers. This is not a claim of running MSVC.
- Examined PE machine type and imports. OpenH264 and mfreadwrite are dynamically loaded, not mandatory import-table dependencies. No GCC runtime or C++ runtime DLL is imported. Windows system DLLs are still required.
- Uniscribe imports resolve through USP10.dll, avoiding accidental binding to newer GDI32 exports.
- Production JSON parser: Persian/emoji Unicode escapes, surrogate-pair handling, short-output rejection, malformed JSON, nested keys, and 30,000 escaping/decoding roundtrips. AddressSanitizer and UndefinedBehaviorSanitizer enabled. LeakSanitizer could not run in the container and was explicitly disabled.
- Production codec adapter: OpenH264 2.4.1 C interface, SPS/PPS generation, 20 encode/decode roundtrips, dimensions and keyframe check. Used the Linux binary through a small test-only loader adapter; this is not a Windows DLL execution test.
- Android baseline comparison: 23 Java source files are byte-identical to ALTEK-Link-0.1.6.zip.

## Not executed here

- Windows UI rendering/interaction and physical webcam/microphone/speaker tests.
- Schannel handshake and certificate-selection interoperability with Android, in either direction.
- Real Windows file/profile exchange and repeated call teardown.
- Windows 7 RTM/SP1 or Windows 10/11 runtime acceptance.
- Exact TDM-GCC 9.2 or VS2010 compilation. Build scripts are provided for these; current cross-builds do not substitute for testing them.

A local Wine execution attempt was blocked by the environment (`wineserver: socket: Operation not permitted`). No permission bypass was attempted. Hardware and operating-system compatibility are therefore explicitly pending user-side tests.

## Historical built executables (Windows 0.1.0)

| Target | PE machine | EXE size | SHA-256 |
| --- | --- | --- | --- |
| x86 | 0x014c | 164,878 bytes | d05bd34b068f15e6df602621c40ff1e081f01b1f7d103705717aa8eac4ee7643 |
| x64 | 0x8664 | 146,944 bytes | 747dff4c97e2f4b6c4a295e7ce01c827245a147fa6d5e7548ebd429a31be118f |

The binary sizes exclude the adjacent OpenH264 DLL.
