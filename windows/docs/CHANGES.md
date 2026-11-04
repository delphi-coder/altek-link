# 0.1.1

Fixes
1. Header photo now repaints when another contact is selected (and when that contact's photo
   arrives). It is an owner-drawn button and was never invalidated.
2. Message text can be selected with the mouse (drag, double-click = word, Ctrl+A = all) and copied
   with Ctrl+C or the right-click menu. Selection can span several messages; dragging past the
   edge auto-scrolls. Copy whole message and Open link remain in the menu.
3. Received file offers show a visible **Download** button (size included) with a progress bar
   while receiving, then **Open** / **Show in folder**. A failed transfer shows **Retry download**.
   Single click works; the old hidden double-click is gone.
4. The message box has a visible rounded frame; the toolbar/header/bottom strips are tinted so the
   white box stands out. Static labels no longer draw on a grey rectangle.
5. All icons (attach, emoji, send, audio call, video, camera, mic, speaker, rotate, settings,
   hang up, answer) are redrawn as anti-aliased vector shapes (src/icons.c, 4x supersampling, no
   GDI+ dependency). Call buttons are round.

Notes
- New files: src/icons.c, include/icons.h. Makefile / build scripts pick up src/*.c automatically.
- Not compiled with MinGW/VS2010 or run on Windows by the author of this patch. Syntax-checked
  against stub headers only; icon rasteriser was compiled natively and its output inspected.

# 0.1.2 (build fixes, found by compiling with VS2010 and TDM-GCC 9.2)
- image.c: a variable named `small` collided with `#define small char` from the VS2010 SDK
  (rpcndr.h). Renamed to `thumb`. This produced the long run of C2059/C2143 errors.
- link.h: now includes <objbase.h>, so CoInitializeEx / COINIT_* are declared in ui.c even with
  WIN32_LEAN_AND_MEAN.
- link.h: silences the harmless C4005 INT*_MIN/MAX redefinition warnings (stdint.h vs intsafe.h).
- camera.c: (superseded in 0.1.5) a local copy of a GUID missing from old TDM-GCC headers.

# 0.1.3 (second round of build fixes)
- VS2010: link failed because bin\x86 did not exist; build-vs2010.bat now creates bin\<arch>.
  The Makefile (TDM-GCC) had the same latent problem and now creates it too.
- TDM-GCC: SP_PROT_TLS1_2_CLIENT/SERVER missing from old headers; defined in link.h with the
  Windows SDK values (0x800 / 0x400), plus the TLS 1.1 pair and SCH_CRED_DISABLE_RECONNECTS.
- icons.c: include order fixed so _WIN32_WINNT is no longer redefined after math.h.
- build-tdm.bat runs make with -k so every compile error is listed in one run.

# 0.1.4 (not released as such)
Experimental webcam format fallback and extra camera logging. Dropped again in 0.1.5: with an
updated TDM-GCC the original camera.c builds and the webcam works, so camera.c is the 0.1.0 file.

# 0.1.5
- Chat input box: right-click menu (Undo, Cut, Copy, Paste, Delete, Select all). Rich edit swallows
  the right click, so the menu is built by the app. Paste inserts plain text only.
- Settings > Color theme: Green (original) or Petrol blue. Switches sent-message bubble, selected
  contact row, icon/send-button accent, file buttons, progress bar and the tint of bars/background.
  Stored as `theme` in settings.ini; applied immediately on Save.
- camera.c restored to the original 0.1.0 version.

# 0.1.6

- Desktop sharing: answering an incoming "screen" call now streams this PC's primary display through
  the Android Screen-engine protocol (kind 1 format + kind 2 H.264 frames; no audio, like a viewer
  session). Desktop frames are captured with GDI StretchBlt, converted RGB->I420, and software H.264
  encoded exactly like the camera path; the remote Android displays it using the existing 90 CW
  rotation of that path.
- Screen call: the "screen" call kind is accepted for incoming requests and can be dialled from the
  new Screen button (ICON_SCREEN) in the contact header. When Windows dials, it decodes the remote
  H.264 stream (so you can view/control the remote's screen) rather than sharing its own.
- Controller gestures from the call window: clicking/dragging on the preview sends tap/swipe to the
  remote; the receiving side (when Windows is the shared screen) injects them into its desktop with
  SendInput.
- APP_VERSION bumped to 0.1.6; build verified with TDM-GCC 10.3 (`mingw32-make ARCH=x64`).
