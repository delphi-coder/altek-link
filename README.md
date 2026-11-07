# ALTEK Link

ALTEK Link is a lightweight peer-to-peer LAN application for Android and Windows. Devices discover
each other locally and can exchange messages and files, make audio/video calls, and share or
control screens without an account, cloud service, or central server.

The active clients are:

- `android/` — Android 4.1+ (minSdk 16), targetSdk 33.
- `windows/` — Windows 7+, x86 and x64.

## Android build and private signing key

The private signing key is deliberately not part of this repository. Create and protect your own
keystore, then set these environment variables before running `build.bat`:

```bat
set ALTEK_KEYSTORE=D:\private\altek-release.keystore
set ALTEK_KEY_ALIAS=altek-release
set ALTEK_KEYSTORE_PASS=your-keystore-password
set ALTEK_KEY_PASS=your-key-password
cd android
build.bat release
tests\run-core-tests.bat
```

For repeated local builds, copy `signing.example.bat` to the ignored
`android\.local\signing.bat`, fill in the private values, and run `build.bat` normally.
The script loads that file automatically when signing variables are not already set.

Never publish or commit the keystore. People who build their own APK must use their own key; an APK
signed with a different key cannot update an existing installation.

## Windows build

```bat
cd windows
build.bat release
```

Both projects accept `build.bat release`, `build.bat debug`, or `build.bat clean`; omitting the
mode selects `release`. The Windows wrapper builds x86 with VS2010 and x64 with TDM-GCC by default.
Pass `x86` or `x64` as its second argument to build only one architecture.

The source directories are intentionally version-independent. Keep them named `android/` and
`windows/` when product versions change; put version numbers in manifests, source constants,
build outputs, Git tags, and release assets instead.

Android `install.bat` installs the last successfully built mode by default. Pass `install.bat debug`
or `install.bat release` to override it explicitly.

The Windows build number in `include/build_number.h` is advanced once per logical source build
cycle. Both architectures from the same cycle use the same number.

## Diagnostics

Release builds contain no diagnostic-log implementation or configuration UI. Debug builds read the
receiver IPv4 from the private, ignored `.local/udphost` file. When it is missing, `build.bat debug`
asks once and creates it. Never include `.local/` in a release archive.

## License

First-party ALTEK Link code is available under the MIT License. Bundled third-party components and
artwork retain their own notices and licenses.

## Version history

See [CHANGELOG.md](CHANGELOG.md) for recent changes and release notes.
