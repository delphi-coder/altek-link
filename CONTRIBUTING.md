# Contributing

Bug reports, tested fixes, compatibility results, and focused improvements are welcome. If you
modify ALTEK Link, please consider sending the improvement back as a patch or pull request so the
Windows and Android implementations can remain interoperable.

The MIT license does not require upstream contribution. This request is a collaboration norm, not
an additional license restriction.

Before submitting a change:

- preserve protocol-v1 interoperability;
- keep Android minSdk 16 / Java 7 compatibility;
- keep Windows 7, x86/x64, C89/C90, and Visual Studio 2010 compatibility;
- do not commit signing keys, local settings, logs, generated binaries, or captured media;
- run the documented build and test commands for every affected platform.
