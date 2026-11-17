# outerssh-client-android

A small Android app that browses a server's web apps over SSH, built on
[OuterSSH](https://github.com/outergroup/OuterSSH). It connects to one server,
runs OuterSSH's HTTP proxy, points a WebView at it, and opens the server's
[homepage](https://github.com/outergroup/OuterSSH/blob/main/OuterSSH/README.md#server-defined-homepage).
Installing [Outer Shell](https://outershell.org) on a server makes it that
server's homepage, on Linux, macOS, and Windows.
Type a port number to open `localhost:<port>` on the server; tap Home to go back.

Use it as a starting point for your own client.

## Configure

Edit `app/src/main/java/com/example/outersshclient/Config.kt`: host, port, username,
and either a private key (pasted as text, with an optional passphrase) or a password.

`HOST_KEY_FINGERPRINT` pins the server's host key. While it's empty the app trusts
whatever key the server presents and logs its fingerprint; copy it from Logcat
(tag `OuterSSHClient`) into the config.

## Server-side helpers

OuterSSH uploads a small helper to the server when it first needs one, such as to
open a Unix socket like Outer Shell's. The build downloads signed helpers for Linux,
macOS, and Windows servers from outerssh.org into `helpers/` (git-ignored), checks
each against `helpers.sha256`, and bundles them as uncompressed assets.

To use helpers you build yourself (see `OuterSSH/Release/README.md`), put them in
`helpers/` and their hashes in `helpers.sha256`; the build only downloads files
whose hash doesn't match.

## Build and run

Clone with the OuterSSH submodule:

```sh
git clone --recurse-submodules <this repo>
```

Open the project in Android Studio and run it, or from the command line:

```sh
./gradlew installDebug
```

The build needs the Android SDK (platform 37), NDK 30.0.16248370 and CMake 4.1.2,
which Android Studio's SDK Manager installs. The app requires Android 13 (API 33)
or later. On API 37 and later it asks for local network access
the first time it starts.

## How it works

- `OuterSSHClient.kt` is the Kotlin side of a small JNI layer, `app/src/main/cpp/client.c`.
  The C code creates OuterSSH's event queue and watches its eventfd on the main
  thread's looper, creates a session pool and an HTTP proxy, answers OuterSSH's
  questions (uploading helpers straight from the APK), and converts homepage URLs.
- `MainActivity.kt` routes the WebView through the proxy with androidx.webkit's
  `ProxyController` and answers the proxy's authentication challenge with the
  credentials OuterSSH generated.
- OuterSSH is built from the `OuterSSH` submodule with Android's platform
  cryptography (`OUTERSSH_ANDROID_CRYPTO`). Its native code calls back into the
  Java class `org.outerssh.AndroidCrypto`, which the build takes from the
  submodule; `JNI_OnLoad` in `client.c` connects them with
  `outerssh_android_initialize`. If you enable shrinking, keep that class's public
  static methods: `-keep class org.outerssh.AndroidCrypto { public static *; }`.

## Limitations

- Root-owned sockets need `sudo`, which the starter declines.
- Supports `http+unix`, `http+pipe` and `http://localhost` homepages only.
- No saved settings, tabs or bookmarks: edit the code.
