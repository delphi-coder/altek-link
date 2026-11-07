# Changelog

Notable user-facing changes are recorded here for each GitHub update. Older Windows-only notes are in [windows/docs/CHANGES.md](windows/docs/CHANGES.md).

## Android 0.1.17 and Windows 0.1.7 build 18 ? 2026-11-07

### Android
- Add a movable self-view during video calls, with tap visibility and double-tap swapping.
- Allow a trusted contact to start unattended remote control when root access has been granted. Root availability is checked before enabling and using this permission; otherwise normal call acceptance remains available.
- Add screen-sharing settings for resolution, frame rate, bitrate and keyframe interval. The root screenrecord path supports size and bitrate; its other settings are controlled by Android.
- Bound the screen transmission queue and recover H.264 with a keyframe after backpressure.
- Improve remote double-tap and add keyboard text input from Windows using the Windows keyboard layout when Android Accessibility is enabled. Improve handling of the Samsung Messages recipient search hint on older Android versions.

### Windows
- Add a setting to disable remotely requested pinch zoom everywhere or only on the desktop.
- Improve settings layout and add a button to open settings.ini.
- Improve remote keyboard focus and forwarding of system keys; send Unicode text to Android peers that advertise support.

### Validation and limitations
- Android release APK signature verified and existing JVM core tests passed. Windows x86 and x64 release builds completed.
- Interactive calls, keyboard handling, unattended control and screen sharing depend on device and OS behavior; the latest changes have not been fully validated across all device combinations. Audio latency has not been addressed in this update.
