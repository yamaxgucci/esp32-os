# ArgonOS phone app (Android)

A board's screen and keyboard on an Android phone, over **Bluetooth** when the
phone and the board share no network, or over **Wi-Fi** when they do.

The screen is the board's own page, `apps/phone/web/index.html`, unchanged: the
app shows it in a WebView and puts a Bluetooth link where its WebSocket was
(`assets/shim.js` in the page, `MainActivity.java` behind it). Over Wi-Fi the
app simply opens the board's page at `http://<board>:8765/`.

## Build

```
python tools/phoneapp/build.py            # -> build/phoneapp/ArgonOS.apk
python tools/phoneapp/build.py --install  # and adb install -r it
```

No Gradle: aapt2, javac, d8, zipalign and apksigner from the Android SDK
(`ANDROID_HOME`, or the one Unity's Android support installed), and a JDK
(`JAVA_HOME`, or Unity's). The APK is signed with a debug key made on the first
build at `~/.android/argon-debug.keystore` - keep it, or the next build will not
install over this one without uninstalling first.

## The board

PHONE.SYS serves Bluetooth when the system has the radio up. It does not raise
the radio itself, just as it does not join a Wi-Fi network:

```
[bt]
on = yes          ; or `bt on` at the prompt
```

The board advertises service `0xfff0` under `[phone] name`, else `[net]
hostname`, else `argon`. Text and keys only over Bluetooth; pictures need Wi-Fi.

Needs a firmware with ABI 0.64 (`ble->adv_recv`, `adv_mtu`, `radio_on`) and the
BLE peripheral (`CONFIG_ARGON_BLE_PERIPHERAL`, on for the S3).

## Without a board

```
python tools/phoneapp/boardsim.py --password argon
```

A pretend board on this PC's Bluetooth: the same service and messages in front
of a small fake console (`help`, `ver`, `cls`, `echo`, `dir`). Needs the PC's
Bluetooth switched on and the `bless` package.

## The wire

As `apps/phone/phone.c` describes it: a write to `0xfff2` is one message, phone
to board; notifications on `0xfff1` are pieces of messages, board to phone - a
flag byte (`0x40` first piece, `0x80` last) and the bytes. The messages are the
WebSocket's, plus `'Q'` + reason ("replaced", "badpass"), which is how the board
says what a WebSocket says with its close frame.
