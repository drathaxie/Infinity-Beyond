# What is actually inside the AQW Infinity Android APK, anticheat wise?

I pulled the Android build apart and went looking for anticheat. This is what I found, what I did not find, and how you can check every claim yourself.

Short version: **there is no anticheat in this app.** There is no anticheat software, no root detection the game itself acts on, no code that checks whether the APK has been modified and no anti-debugging. What protection exists is in the server design instead. There is also a third party analytics SDK doing its usual default telemetry, which is worth reading once. Details below.

Measured against the stock `aq2d.apk`:
- Version **0.0.254** (versionCode 9).
- Package `com.Artix.aq2d`.
- Unity 6000.3.17f1.
- IL2CPP scripting backend (metadata version 39).
- Unencrypted.

At the time of writing (Friday the 4th of September 2026) the APK is **NOT** on the Google Play Store. This build was sideloaded from the Artix site. With no concrete confirmation of their future intent, this document will acknowledge the APK as if it was downloaded from Google Play.

---

## How I checked:

Everything below came from four passes over the stock APK.

1. Unzipped it and listed every native library in `lib/`.
2. Dumped the Android manifest with `aapt2` to read the permissions.
3. Disassembled `classes.dex` with `dexdump` (604,082 lines) and mapped every suspicious API call back to the class and method that calls it.
4. Parsed `global-metadata.dat`. That file is the IL2CPP name table and it is not encrypted in this build, so it yields **106,190 identifiers** (class, method, and field names) and **21,273 string literals**. Identifiers in that file are grouped per assembly, so I sliced out just the game's own code, `Assembly-CSharp`, which is lines 13,505 to 28,805 of the name table.

The repro commands are at the bottom.

---

## 1. There is no anticheat software in the app.

The entire native library folder is this:

```
lib/arm64-v8a/
  lib_burst_generated.so     Unity Burst compiler output
  libil2cpp.so               the game's C# compiled to native code
  libmain.so                 Unity's loader stub
  libsteam_api.so            Steam API, unused on Android
  libunity.so                the Unity engine
```

That is it. No EasyAntiCheat/BattlEye/Denuvo/DexGuard/Arxan/Promon/Talsec/ACE. Every commercial mobile anticheat ships its own `.so` file, and there is not one here.

**No anti-debugging either.** Anticheat and anti-tamper code almost always checks whether a debugger is attached, usually by calling `ptrace` or reading `TracerPid` out of `/proc/self/status`. I counted occurrences:

| Library | `ptrace` | `TracerPid` |
|---|---|---|
| `libil2cpp.so` (all the game code) | **0** | **0** |
| `libunity.so` (the engine) | 1 | 1 |

Zero in the game. The single hit in `libunity.so` is stock Unity crash handler code that ships in every Unity app on the planet. It is not looking for you.

**The permissions back this up.** The manifest asks for exactly three things:

```
android.permission.INTERNET
android.permission.ACCESS_NETWORK_STATE
com.android.vending.BILLING
```

No `QUERY_ALL_PACKAGES`, which is the permission a detector needs to scan your phone for known cheat tools. No Play Integrity or SafetyNet attestation metadata. There is also no `android:name` on the `<application>` tag, meaning the app has no custom Application class, which is the normal place to run integrity checks before the game even starts.

**And the game code checks nothing.** I traced every call to the Android APIs you would use to verify an app has not been tampered with (`getPackageInfo`, `getApkContentsSigners`, `signingInfo`, `MessageDigest`) and every reference to `/system/bin/su`. All of them belong to third party libraries: Google Play Services, Google Play Billing, and GameAnalytics. **Not a single one is in Artix's own code.**

They also left `IngameDebugConsole.Runtime.dll` and `CodeStage.AFPSCounter.Runtime.dll` in the shipping release. Nobody who is about to bolt on anticheat ships a debug console in a retail build.

---

## 2. One caveat: the bundled analytics SDK has its own default telemetry.

This is not anticheat, but it is the reason I am not saying "you are invisible," so it belongs in the list.

The game bundles **GameAnalytics**, a standard analytics SDK used by a very large number of Unity games. Like every install of it, it annotates the events it sends with device and build metadata of its own accord. Two of those annotations are environment facts rather than gameplay ones:

```
"jailbroken"              from isDeviceRooted()
"android_app_signature"   a hash of the APK's signing certificate
```

So a modified build is not byte-for-byte identical to a Play Store install in what the SDK reports about it. That is a real drawback and worth knowing before anyone assumes otherwise.

Some context on how much weight to put on it. **None of this is Artix's code** - it is stock, out of the box SDK behaviour that ships with the library, it is not gameplay data, and nothing in the game reads either annotation back or acts on it. It lands in a third party analytics product built for retention and funnel charts, alongside a great many other annotations, and analytics dashboards are generally read for aggregates rather than individuals. It was plainly not put there to look at anybody.

The honest summary: it is not zero, and I am not going to tell you it is. It is also not anticheat, nothing in this app is, and this is the only thing in the whole APK that describes your setup at all.

---

## 3. Moderation is done by humans.

There is a full player reporting flow in the client. The UI class is `UIReportPlayerPopup` and it has exactly the parts you would expect: `SelectCategory`, `categoryButtons`, `reasonInput`, `submitButton`, `selectedCategory`. You reach it from the right click player menu (`Context_ReportPlayer`) or from `OpenReportPanel`, and `/report` also exists as a chat command (`HandleReport`).

The report categories are stored as plain text in the game. Two of them:

```
Botting / Hacking
Scamming / Phishing
```

So botting is a named, explicitly acknowledged concern. It is just handled by players reporting other players and a human reading the report, rather than by automated detection. There is also a chat rate limiter, which shows up as the message `Spam Guard {0} Seconds`.

That is the complete anti-abuse pipeline in the client: report a player, a human looks at it. Nothing out of the ordinary when you compare this to how moderation works in the Flash release of Adventure Quest Worlds.

---

## 4. The protection is in the server design.

This is why they can get away with having no anticheat. The game does not trust the client with any number that matters. Look at the shape of the network protocol, which I pulled from the `Request*` and `Response*` classes in the game's own code:

| What I found | What it means in plain terms |
|---|---|
| `ResponseServerHitbox` (fields `cx`, `cy`) and `ResponseServerRange` (field `hw`) | **The server tells the client** what the hitbox and attack range are. The client never gets to claim them. |
| `RequestPlayerHit`, `RequestMonHit`, `RequestAttackInput`, `RequestAttackStream` producing `ResponseAttack` and `ResponseStatUpdate` (fields `DmgMin`, `DmgMax`) | Hits are **requests**, not statements. You ask to hit something. The server decides whether you did and how much damage happened. |
| `RequestMoveOK`, `RequestMovement` / `ResponseMovement`, `RequestStopWalk` / `ResponseStopWalk`, `RequestMoveToCell` | Movement is acknowledged by the server rather than accepted on trust. |
| `RequestTryQuestComplete` | Note the word **Try**. The client asks. The server validates and decides. |

The practical consequence: you cannot send a packet that says "I did 999999 damage" and have it work, because the client never sends damage numbers in the first place. It sends intent, and the server does the math.

This is the correct way to build an online game, and it is why a client side anticheat would be mostly redundant. The thing a server built like this stays exposed to is **rate and frequency**, not impossible values. Which is exactly the kind of check you would add on the server, where no client update is needed and nobody outside Artix can see it.

---

## 5. Staff tools exist and are gated on the server.

The client contains a staff command system. The access control fields are `_accessLevelRequired`, `PlayerAccessLessThan`, `_accessDeniedMessage`, `isStaff`, `intAccessLevel`, and `AccessLevel`. The staff commands include:

```
HandleDev  HandleDevOn  HandleDevOff  HandleNoclip  HandleClip
HandleTogglePads  HandleHitboxes  HandleCells  HandleScan
HandleDlogin  HandleDpass
```

The client sends the command and the **server** checks your access level, so these are not usable by ordinary accounts just because the code is present. `HandleScan` plus the `ResponseKick` packet are the live enforcement tools a GM already has today. Again: a human, deciding.

---

## Things that looked suspicious and turned out not to be:

I want to list these because a couple of them will absolutely get misread by the next person who goes looking, and I do not want to see them repeated as fact.

- **`PmahsKey` on `RequestPlayerHit`.** Looks exactly like an anti-spam token the server hands out so you cannot fake hit packets. It is not. It belongs to `PlayerHotTile`, a damaging floor tile mechanic (`HotTile`, `SafeTile`, `TickLoop`, `intervalMs`). It just identifies which hazard tile hurt you.
- **`RequestMeasurement`.** Despite the name, it is not a network packet at all. It is the Android on screen keyboard height helper (`MeasureOnUiThread`, `settleSeconds`, `HeightPixels`).
- **`VerifyMe`.** Belongs to `ActionScript`, the level scripting component. It verifies a scene object, not a player.
- **`frida` appearing in `classes.dex`.** False positive. It is matching inside a giant top level domain regex in `androidx.core.util.PatternsCompat`.
- **`Unity.Purchasing.Security` and `ObfuscatedAccountId` / `ObfuscatedProfileId`.** Real, but these are Google Play in-app purchase receipt validation and purchase fraud prevention. They have nothing to do with gameplay.

---

## What does this mean?

Right now: no anticheat, no tamper detection, no root enforcement and no debugger detection. Protection comes from the server owning the math, plus player reports read by humans. The only thing in the app that describes your setup at all is the bundled analytics SDK's default annotations which is covered in section 2.

I am not going to pretend that is permanent. This could change tomorrow. Here is what I would watch for in future releases, since all of these are cheap to re-check on any new APK:

1. **A new `.so` file in `lib/`.** That is how every commercial anticheat arrives. Today that list is five files.
2. **`android:name` appearing on the `<application>` tag in the manifest.** That would mean a custom Application class exists, which is where startup integrity checks live.
3. **`QUERY_ALL_PACKAGES` or Play Integrity / SafetyNet metadata in the manifest.** Those are permission level tells, visible before the app even runs.
4. **New identifiers in the `Assembly-CSharp` name slice** matching things like `integrity`, `attest`, `checksum`, or `tamper`. Today there are none.
5. **New `Request*` classes** in the protocol, particularly anything that sends client state upward rather than intent.

None of that requires guesswork. It is a diff against a known baseline, which this document can act as.

---

## Check it yourself:

All you need is Python 3, `unzip`, and the Android SDK build tools.

```bash
# unpack
unzip -q aq2d.apk -d apk/

# what native libraries ship
ls -la apk/lib/arm64-v8a/

# permissions and manifest metadata
aapt2 dump xmltree --file AndroidManifest.xml aq2d.apk | grep -i 'permission\|meta-data'

# disassemble the Java layer and find who calls what
dexdump -d apk/classes.dex > dex.dis
grep -n 'getApkContentsSigners\|/system/bin/su\|isDeviceRooted\|jailbroken' dex.dis

# count anti-debug primitives in the native libraries
python -c "d=open('apk/lib/arm64-v8a/libil2cpp.so','rb').read(); print(d.count(b'ptrace'), d.count(b'TracerPid'))"
python -c "d=open('apk/lib/arm64-v8a/libunity.so','rb').read();  print(d.count(b'ptrace'), d.count(b'TracerPid'))"
```

For the IL2CPP name table, `global-metadata.dat` is at `apk/assets/bin/Data/Managed/Metadata/global-metadata.dat`. In this build the header is a sanity value, then the version, then a series of (offset, size, count) triples. The identifier strings are a plain null-terminated block at offset 665,872 running 1,923,010 bytes, and the string literals are a separate block at offset 85,476 addressed through an offset table at 380. Splitting that identifier block on null bytes gives you all 106,190 names in assembly order, which is how the `Assembly-CSharp` slice above was isolated.

---

*Analysis performed on `aq2d.apk` 0.0.254 (versionCode 9), September 2026. Every claim above is a direct observation from that file. If a later release changes any of it, this document is wrong and I would rather be told.*