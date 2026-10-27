# Migrating off IBC — the dependency surface, and why not yet

IbcAlpha/IBC was **retired on 2026-09-01**. The maintainer: *"I'll continue to
support it until 1 September 2026, and after that I will let it die."* The VPS
runs **IBC 3.24.1** (jar dated 2026-07-02), so it will never be updated again.

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
alone, and watch one nightly cold restart before touching IBC.

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
6. **A nightly COLD restart at 23:55** — kill and re-login, not the gateway's own
   soft `AutoRestartTime`. This is not a preference. The soft restart reuses the
   session and that session comes back **stale** on this box: port open, API
   accepting, and unable to reach IBKR, rejecting clients with "Client
   disconnected before version was sent" until a full cold relaunch. That is the
   failure 0.2.14 fixed by switching paper to `ColdRestartTime`.

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
3. **Let one real 23:55 cold restart happen**, then verify the next morning
   before the open. This is the only check that exercises the *scheduled* path,
   and it is the one whose failure costs a trading day.
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

### Still open

Whether IBC 3.24.1 can drive 10.50's **login dialogs** — the original question —
is *still unanswered*, because the 02:26 attempt never got far enough to find
out. The acceptance test above stands unchanged, and step 3 (one real 23:55 cold
restart) remains the one whose failure costs a trading day.
