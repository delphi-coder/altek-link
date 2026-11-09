# Changelog

User-facing updates for ALTEK Link. Earlier Windows-only notes are available in [windows/docs/CHANGES.md](windows/docs/CHANGES.md).

## Android 0.2.0 build 3 and Windows 0.2.0 build 2 (2026-11-09)

- Share copied text between devices during remote control. Choose controller to remote, remote to controller, both directions, or turn sharing off.
- Keep the copied text on the device after the remote session ends. On Android 10 and later, return to ALTEK Link when needed to let it read and send text copied in another app.
- See the remote Android screen sooner: the Android viewer now preserves the initial video frames while its display prepares.
- Find the installed Android build number in Settings.
- Windows screen sharing now defaults to 8 frames per second for new configurations.
- Find clear explanations in newly created Windows settings files. Existing settings and comments are kept.
- Avoid unnecessary changes to the Windows settings file when its values have not changed.

## Android 0.1.17 and Windows 0.1.7 build 18 (2026-11-07)

Android and Windows downloads are available together in the same release.

### Video calls

- **See yourself during a call.** A movable self-view shows which camera is active and what it sees. Double-tap the small video to swap it with the main view; tap the screen to hide or show it.

### Screen sharing and remote control

- **Unattended Android access.** Give a trusted contact permission to connect automatically, so you can reach a phone even when it is out of reach. This feature requires root access and must be enabled for that contact.
- **Screen-sharing settings for Android.** Adjust the maximum screen size, frame rate, bitrate and keyframe interval for your device and network. With root-based screenrecord, Android controls the frame rate and keyframe interval.
- **More resilient H.264 screen sharing.** Screen sharing now recovers with a fresh keyframe when transmission falls behind, helping prevent an outdated frame queue from building up.
- **Easier double-clicking from Android.** Double-tap on the Android screen to double-click naturally on a remote Windows PC.
- **Keyboard input from Windows to Android.** Type into the remote Android device with your PC keyboard, including text in the active Windows keyboard language. Android Accessibility must be enabled.
- **Better text entry in Samsung Messages.** Typing into the recipient search field no longer includes its placeholder text on supported older Samsung devices.
- **Keyboard control between Windows PCs.** Remote keyboard focus and system-key handling have been improved for PC-to-PC screen sharing.
- **Control pinch zoom on Windows.** Choose whether remote pinch zoom works everywhere, is disabled on the desktop, or is disabled completely.

### Windows settings

- **Open settings.ini directly.** A new button takes you straight to the settings file.
- **Clearer settings layout.** The bottom of the Settings window has more room for its buttons and version information.
