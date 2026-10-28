# Migrating off IBC — the dependency surface, and why not yet

IbcAlpha/IBC was **retired on 2026-09-01**. The maintainer: *"I'll continue to
support it until 1 September 2026, and after that I will let it die."* The VPS
runs **IBC 3.24.1** (jar dated 2026-07-02).

> **CORRECTED 2026-10-28.** 3.24.1 was *not* the last release. **IBC 3.24.2**
> shipped 2026-08-21 and is the real final IbcAlpha release (the 09-01 commit only
> revised the README). It exists *specifically* for TWS/Gateway 10.48+: it
> forwards the build's own `javaOptions` from `i4jparams.conf`, falls back to
> `<install>\jre\bin` for Java, and restores the auto-restart confirmation handler
> 3.24.1 lacks (IbcAlpha/IBC #377). Both 10.50 problems hit on 2026-10-27/28 are
> ones it fixes. See the end of this file.

This file is the checklist a replacement has to satisfy. It exists because the
knowledge is currently spread across `Start-IbGateway.ps1`'s comments, a memory
note, and two incidents — and a swap tested against recollection is a swap that
breaks at 23:55.

## Two migrations, not one. Do not combine them.

| | deadline | risk | blast radius |
|---|---|---|---|
| **IB Gateway 10.45 → 10.50.1** | **hard: 2026-12-15** | low | reversible in seconds |
| **IBC → a successor** | none | high | unattended login |

The gateway upgrade is forced and cheap. The IBC migration is neither.

**The gateway upgrade is also the experiment that tells us whether the IBC
migration is urgent.** If IBC 3.24.1 drives 10.50's dialogs, there is no fire and
the successor can be adopted whenever one has matured. Do the gateway first,
alone, and watch the overnight restarts before touching IBC (weeknights are the
gateway's own soft restart; the IBC cold restart is Sundays only — see item 6).

Rollback for the gateway is genuinely cheap: `Start-IbGateway.ps1` selects the
**highest numeric** version directory under `C:\Jts\ibgateway`, so reverting is
renaming `1050` to something non-numeric. Nothing hardcodes a version.

**Two machines are affected, not one.** The VPS is on 10.45; the development PC
is on 10.48 (`C:\Jts\ibgateway\1048`). Both are below the 10.50.1 floor.

## What this setup actually needs from IBC

Only these. IBC does far more (FIX, live-account 2FA, dozens of dialogs); none of
the rest is load-bearing here.

1. **Windows, with a desktop session.** Not Docker, not headless — the terminal
   is a GUI app that dies without one, so the box has an RDP session anyway.
2. **Launch IB Gateway ≥10.50**, paper mode, API port forced to **4002**
   (`OverrideTwsApiPort`).
3. **Type the paper credentials.** No 2FA on paper.
4. **Dismiss, unattended:** the paper-trading disclaimer, "Newer Version",
   the non-brokerage-account warning, and existing-session-detected
   (`ExistingSessionDetectedAction=primary`).
5. **Auto-accept the API connection prompt**
   (`AcceptIncomingConnectionAction=accept`). Left at `manual` the app cannot
   connect at all.
6. **A weekly cold restart, and a backstop for the nightly soft one.**

   > **CORRECTED 2026-10-28.** This item used to say IBC gives us "a nightly COLD
   > restart at 23:55". It does not. IBC 3.24.1's own `config.ini` documents
   > `ColdRestartTime` as **Sundays only** ("When this time is reached on Sundays,
   > IBC tidily closes TWS ... initiating the usual full logon"), and its log
   > agrees: *"Gateway will be cold restarted at 2026/11/01 23:55"*, a Sunday.

   On weeknights the gateway does its **own soft auto-restart** at the Lock and
   Exit time in its settings. That reuses the session, and on this box the reused
   session can come back **stale**: port open, API accepting, unable to reach
   IBKR, rejecting clients with "Client disconnected before version was sent".
   What actually rescues those nights is `Watch-IbGateway.ps1`'s broker-down
   relaunch — the ~00:58 cold logins scattered through the launcher logs. 0.2.14
   believed `ColdRestartTime` had removed this failure; it only made it weekly
   on Sundays. A replacement must keep both: the weekly full re-authentication,
   and a backstop for a stale weeknight session.

   **The retiring maintainer's own reason for dropping IBC was "the auto-restart
   mechanism in Gateway and TWS" — i.e. precisely the mechanism that fails here.**
   A successor that only offers a soft restart does not replace IBC for this box.
7. **Must not leave the main window minimized** (`MinimizeMainWindow=no`):
   dialogs are modal children and become unreachable.

### Not IBC's job, and must stay ours

These live in `Start-IbGateway.ps1` and must survive any swap:

- **The login-attempt governor** (`Add-LoginAttempt`). **IBKR locks accounts on
  repeated failed logins.** Nothing in IBC or any successor protects this; a
  retry loop around a wrong password is how the account is lost. `-Force`
  bypasses it and is for a human at the console only.
- **The launch mutex**, so two concurrent starts cannot both sail past the port
  probe and launch.
- **The paper/live asymmetry**: cold restart for paper, soft for live (a live
  cold re-login can need a TOTP that nothing can type, and would wedge nightly).
- **24-hour time format.** IBC 3.24.1 silently drops an AM/PM suffix:
  `ColdRestartTime=11:55 PM` restarted at **11:55 AM**, mid-session. Any
  successor's time parsing must be re-verified against this.

## Candidates, as of 2026-10-06 — neither is ready

- **[XYUU/IBC](https://github.com/XYUU/IBC)** — the designated successor; the
  maintainer publicly took over. Refactored to Gradle, config renamed
  `.ini` → `.properties`, added automated 2FA, multi-platform. **But its only
  tagged release is v3.24.1 (June 22), cut before the handover** — the same
  version already installed. The new work is unreleased, and no release notes
  mention 10.50.
- **[zdomokos/IBC](https://github.com/zdomokos/IBC)** — actively built (Oct 2026),
  Windows-only, single `ibc.ps1` PowerShell 7 launcher. Explicitly states it
  *"does not keep backward compatibility with existing IBC installations: config
  and script formats may change."* For an unattended box, a maintainer promising
  future format churn is a liability, not a feature.
- **ibg-controller** (Python) — oriented at headless Docker. Wrong shape for a
  Windows desktop session; see requirement 1.

Neither keeps our `config.ini` plus `IBC_PATH` / `TWS_MAJOR_VRSN` / `IBC_INI`
launch interface, so **migration cost is similar for either** — which means the
choice should be made on durability, not on drop-in-ness.

**Recommendation: target XYUU, wait for a tagged post-handover release**, and do
not build an abstraction over an interface that has not shipped yet.

## When a successor does ship, the acceptance test

Not "it started". Run on the VPS, in this order:

1. `scripts\Verify-IbGateway.ps1` → PASS (build >= 1050, farms 3/3, authed, no
   client-id conflict).
2. `scripts\Start-IbGateway.ps1 -Restart` then `Verify-IbGateway.ps1` → PASS.
   This is the cold-login path, on demand, with the governor still armed.
3. **Let the scheduled restarts happen**, then verify the next morning before the
   open: at least one weeknight soft auto-restart, and one **Sunday** 23:55 cold
   restart (`ColdRestartTime` fires on Sundays only). These are the only checks
   that exercise the *scheduled* paths, and their failure costs a trading day.
   `Verify-IbGateway.ps1`'s "settings loaded" line must PASS after each.
4. Confirm the AM/PM landmine: the successor's log must say the restart is
   scheduled for **23:55**, not 11:55.
5. Keep the previous IBC jar on disk. Rollback is restoring it.

---

## 2026-10-27: the 10.50 upgrade failed, and IBC was not the reason

The upgrade to 10.50.1f ran on 2026-10-27 at 02:26 and took the box out of the
market for the whole session. **The predicted risk materialised, but not in the
predicted form.** This document said the unknown was whether IBC 3.24.1 could
drive 10.50's *dialogs*. It never reached a dialog:

```
Error: Can't find suitable Java installation
```

**Java discovery, not dialog handling.** `IBC\scripts\StartIBC.bat` (lines
404-423) looks for a JVM in this order, and nothing else:

1. `$env:JAVA_PATH`, if it contains `java.exe`
2. `<install>\.install4j\pref_jre.cfg` + `\bin`
3. `<install>\.install4j\inst_jre.cfg` + `\bin`
4. `%PROGRAMDATA%\Oracle\Java\javapath`

10.45 passes at step 2: its installer registered a **shared** JRE and wrote
`pref_jre.cfg` pointing at `C:\Program Files\Common Files\i4j_jres\...zulu`.

10.50 **bundles its JRE inside the install directory** and writes **no
`pref_jre.cfg` at all**. Its `inst_jre.cfg` points at the installer's own temp
directory, which is deleted when the installer exits. So steps 2-4 all miss
while a working `java.exe` sits in `<install>\jre\bin`. The gateway's own
`ibgateway.exe` finds it — install4j resolves it relative to the launcher — so
launching the exe by hand "works", which makes this look like an IBC/10.50
incompatibility. **It is not.** IBC has to be told, and 0.51.0 tells it.

### What actually cost the session

Not the Java problem — that is one line of config. The cost came from
**ordering**: `Update-IbGateway.ps1` restarted onto the new build and verified
*afterwards*. `Start-IbGateway.ps1` killed a healthy 10.45 gateway, 10.50 failed
to launch, and the verify step 45 s later could only report the damage. A
`-Check` run beforehand would not have helped either: it checked preconditions
on the *old* build, because the new one did not exist yet.

**0.51.0 adds a launchability gate between the install and the restart**, so a
build whose JVM cannot be resolved can no longer cost us the gateway we already
have. `Verify-IbGateway.ps1` gained the same check, because its "gateway build
1050" PASS was a claim about a *directory name* and said nothing about whether
the thing could start.

### Generalisable, and the same lesson as six previous entries

A check narrower than the invariant it claims. "Is the build new enough?" was
standing in for "will the gateway come up?", and the gap between those two
questions was a trading day. **An upgrade verifier must test the artefact, not
its version number.**

### Still open — ANSWERED 2026-10-27

Whether IBC 3.24.1 can drive 10.50's **login dialogs**: **yes.** Four password
logins and one soft auto-restart on 2026-10-27/28 all completed. The problem
turned out to be one layer down, below.

---

## 2026-10-28: 10.50 could not read its own settings — Java 25's XML limits

With the JVM found, 10.50 came up and authenticated — and then, on **every**
start, showed:

```
We are having trouble loading your settings, they may be corrupted.
File does not exist C:\Jts\<userdir>\ibg.xml
```

The file existed. At the nightly soft restart the exiting instance wrote it at
00:45:01 and the new one rejected it at 00:45:12 — **its own build's output,
eleven seconds old.** 10.45 never showed it once in months, under the same IBC
and directories.

**Cause.** 10.50 bundles **Java 25**, and JDK 24 tightened the XML parser's
limits (JDK-8343006): `elementAttributeLimit` 10000 → 200, `totalEntitySizeLimit`
50,000,000 → 100000, `maxGeneralEntitySizeLimit` unlimited → 100000. The
settings file exceeds them. IB's own launcher raises all three again through the
`javaOptions` variable in `<install>\.install4j\i4jparams.conf`; IBC 3.24.1
builds its own java command line and forwards none of it (only `-DjxBrowserKey`).
So the parser rejects every settings file and the gateway runs on **factory
defaults** after every start. 10.45 ran on a shared Zulu 17 with the old limits.
0.51.0's `JAVA_PATH` — required, 10.50 cannot run without its JRE — is what put
the gateway on Java 25 under IBC.

All four local facts were checked on the VPS, read-only: `i4jparams.conf` carries
`10000 / 0 / 0`; the bundled `jre\conf\jaxp.properties` carries `200 / 100000 /
100000`; IBC's launch line for `1050\jre\bin\java.exe` has zero `jdk.xml` options;
`StartIBC.bat` blanks `JAVA_TOOL_OPTIONS` (line 446) and leaves
`JDK_JAVA_OPTIONS` alone. Publicly reproduced with a one-variable A/B on the same
10.50.1f build: extrange/ibkr-docker #158.

**Why it looked cosmetic and was not.** IBC re-applies the API port and the
Read-Only flag on every start, so trading worked throughout. But every setting
IBC does not manage was reset on every launch, and IBC's log said so: *"Read-Only
API checkbox is **now** set to: false"*, where 10.45 had always logged *"already
set"*. The nightly restart also slid from 00:55 to 00:45 ET — the default Lock and
Exit time showing through.

**Do not answer the dialog.** Per the IBC maintainer's 3.24.2 notes, "Yes"
(recover from backup) strips the API Trusted IPs, and "No" leaves the settings
"in some indeterminate state". Neither can help: the parse has already failed.

**0.52.0 fix.** `Start-IbGateway.ps1` reads the selected build's own `-Djdk.xml.*`
tokens from its `i4jparams.conf` and passes them through `JDK_JAVA_OPTIONS`, set
or cleared on every launch (10.45 has none, so a rollback launches exactly as
before). It reaches every JVM that IBC instance starts — `StartIBC.bat` builds
its environment once and loops back to `:startIBC` for the soft and cold restarts
alike — so it takes effect at the next **fresh** launch and stays. Proven on the
VPS's own Java 25: with the variable, `java -XshowSettings:properties` reports
all three limits; without it, none. System properties outrank `jaxp.properties`,
so nothing inside the install is edited. `Verify-IbGateway.ps1` now FAILS when
the last start showed the dialog — report only, never a restart trigger.

### The next IBC step: 3.24.2, side by side

3.24.2 forwards the whole `javaOptions` value (including the `--enable-native-access`
and `--add-exports` entries 3.24.1 also omits for 10.50), so it would make the
`JDK_JAVA_OPTIONS` forwarding redundant and harmless. Plan it as its own change,
on a weekend, with the acceptance test above:

1. Unzip `IBCWin-3.24.2.zip` to `C:\IBC-3.24.2` while everything runs. Never
   rename `C:\IBC` with the keepalive active: if it fires while the folder is
   missing, `Start-IbGateway.ps1` spends a governor slot before its `IBC.jar`
   check fails.
2. Point `IBC_PATH` (and the `IBC.jar` checks in `Start-IbGateway.ps1` and
   `Verify-IbGateway.ps1`) at the new folder. Rollback is a one-line change.
3. Confirm first that `DisplayBannerAndLaunch.bat` and its environment-variable
   interface are unchanged in 3.24.2 — `Start-IbGateway.ps1` calls it directly.
4. Its `getExtraJavaOptions.ps1` pastes the `javaOptions` value unquoted into a
   cmd line; check the IBC log's java command after the first launch.

**Live-account hazard until then.** For a live account `Start-IbGateway.ps1`
writes `AutoRestartTime`, which makes IBC change the restart time in the config
dialog — and 3.24.1 has no handler for the confirmation that raises (#377), so the
change, and the API port applied with it, may silently not commit. With settings
resetting on every launch it would have happened on every launch. Do not run a
live account through this launcher before settings load reliably and IBC is on
3.24.2.
