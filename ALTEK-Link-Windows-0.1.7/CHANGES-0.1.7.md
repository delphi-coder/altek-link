# 0.1.7 - fix: phones could not see the shared desktop

## Build 7

- Kept architecture-matched OpenH264 runtime DLLs permanently beside their release/debug EXE
  locations. They are Cisco runtime dependencies, not build artifacts: build and clean do not copy,
  replace, or delete them. Clean removes only files produced by the local toolchain.

## Build 6

- Added parameterized release/debug/clean builds. Release contains no diagnostic logging behavior;
  debug reads its private UDP receiver from ignored `.local\udphost`.
- Removed UDP logging prompts and settings from both release UIs.
- Added clean commands for Android and Windows and separate debug/release output paths.

## Build 1

- Added a persistent build counter in `include/build_number.h`; the main title and Settings now
  display the product version and build number.
- Reduced call-window control flicker during remote desktop viewing by excluding child controls
  from parent frame painting and avoiding redundant visibility/repaint work.

## Build 2

- Leaving remote-desktop full screen now immediately reveals the normal controls and performs one
  complete parent/child repaint, clearing stale full-screen pixels from the bottom control strip.

## Build 3

- Reworked the default avatar into a theme-colored circular profile silhouette.
- Replaced the audio-call handset with a clearer anti-aliased telephone shape.
- Added the default-enabled `minimize_screen_share` setting. Accepting an incoming desktop share
  now minimizes visible ALTEK Link windows before acceptance signaling and frame capture start.

## Build 4

- Refined the theme-colored default avatar into a larger, simpler head-and-shoulders silhouette.
- Redrew the audio-call icon as a diagonal classic handset with three radiating sound-wave arcs.

## Build 5

- Disabled UDP diagnostic logging by default and added a one-time first-run configuration prompt;
  the receiver IPv4 now exists only in each installation's private settings.
- Added Windows VERSIONINFO metadata so Explorer shows product version 0.1.7 and build 5.
- Rebuilt both architectures with the current application manifest.

Cause: screen.c's desktop-capture function (screen_capture) built its "format" and "frame" packets
using the OLD audio/video call layout (the same one camera.c uses for webcam-over-video-call),
not the "screen" call's own layout that ScreenEngine.java on the Android side actually expects.
These look similar (a kind byte, then format/frame fields) but are genuinely different protocols:

- format packet: sent as kind=1, height(4 bytes), width(4 bytes), SPS/PPS - missing the h264-flag
  byte ScreenEngine.java expects at that position, width/height in the wrong order, and the config
  one byte earlier than it should be.
- frame packet: sent as kind=2, pts(8 bytes), key-flag(4 bytes), H.264 data - ScreenEngine.java
  expects key-flag(1 byte) THEN pts(8 bytes). On top of the field mismatch, the packet was also
  only transmitted as `len + 10` bytes even though what was actually written was a 13-byte header
  plus the full frame - silently cutting the last 3 bytes off every single frame's H.264 data.

This file's own receive side (further down in screen_run, used when this PC is the controller
viewing a phone's shared screen) already used the correct `b + 10` offset for both format and
frame - which is exactly ScreenEngine.java's layout, since both its format and frame headers happen
to total 10 bytes. That's why PC-sees-phone and phone-sees-phone already worked: every sender
involved in those cases used the correct protocol. Only this one send path - a PC's own desktop
going out to a phone - used the wrong one.

Fixed both packets in screen_capture() (src/screen.c) to match the real layout: format = kind,
h264-flag(1), width(4), height(4), SPS/PPS; frame = kind, key-flag(1), pts(8), H.264 data. The
receive side and the control-message functions (screen_send_tap/swipe/key) were already correct
and are unchanged.

Not rebuilt or run here - only the source was provided. Please rebuild and test.
