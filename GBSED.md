# GBSED on Veins — Setup, Operation, and Reference

GBSED is a VANET file-transfer application built on Veins 5.3.1. One vehicle
(`node[0]`) reads a file from disk, splits it into base64-encoded chunks, and
broadcasts them over IEEE 802.11p. A second vehicle (`node[1]`) reassembles the
chunks and writes the file back out. Vehicle motion comes from SUMO over TraCI.

This document covers how the environment was set up on macOS (Apple Silicon),
how to run the simulation, what every file in the scenario does, and the
limits you will hit.

---

## 1. Quick start

```bash
cd ~/Documents/gbsed_veins

# put an input file where the sender expects it
mkdir -p src/veins/modules/application/gbsed/GBSEDApp/scene_data
cp /path/to/your/scene.bin \
   src/veins/modules/application/gbsed/GBSEDApp/scene_data/scene_0000.bin

./run_gbsed.sh              # command-line run (Cmdenv)
./run_gbsed.sh -u Qtenv     # GUI run
```

The received file appears at
`src/veins/modules/application/gbsed/GBSEDApp/received/received_scene_0000.bin`.
It should be byte-identical to the input — verify with
`shasum <input> <output>`.

A run takes about five seconds of wall time for 200 seconds of simulated time.

---

## 2. Environment

### 2.1 What is installed where

| Component | Version | Location |
|---|---|---|
| OMNeT++ | 6.4.0 | `~/Documents/omnet-projects/omnetpp-6.4.0` (managed by `opp_env`) |
| Nix | 2.35.2 | `/nix`, backs the `opp_env` install |
| SUMO | 1.21.0 | `~/Documents/omnet-projects/sumo-venv`, symlinked into `~/.local/bin` |
| Veins + GBSED | 5.3.1 | this repository |

### 2.2 Why the build needs `opp_env`

Your OMNeT++ is an `opp_env`-managed installation. Its `Makefile.inc` begins with:

```make
ifndef OPP_ENV_VERSION
  $(error This OMNeT++ installation cannot be used outside an opp_env shell.)
endif
```

So a bare `make` in this repo fails. Every build and run must go through
`opp_env run` or `opp_env shell`. Additionally, `nix` is not on the `PATH` in
non-interactive shells, so the Nix profile has to be sourced first. Both are
handled by `run_gbsed.sh`.

To build by hand:

```bash
. /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh
cd ~/Documents/gbsed_veins
./configure                                    # only needed once
opp_env run omnetpp-6.4.0 -w ~/Documents/omnet-projects --no-isolated \
    -c 'cd ~/Documents/gbsed_veins && make MODE=release -j8'
```

This produces `out/clang-release/src/libveins.dylib` and `bin/veins_run`.

### 2.3 Why SUMO 1.21.0 specifically

The version is not arbitrary. Three constraints have to hold at once on an
Apple Silicon Mac, and only 1.21.0 satisfies all three.

| Version | Apple Silicon build | Self-contained | TraCI API | Usable |
|---|---|---|---|---|
| 1.18.0 | no (x86_64 only) | — | — | no |
| 1.19.0 | yes | no — needs Homebrew `xerces-c`, `fox`, `libtiff.5` | — | no |
| 1.20.0 | yes | no — same problem | — | no |
| **1.21.0** | **yes** | **yes** | **21** | **yes** |
| 1.23.x | yes | no — needs Homebrew `xerces-c-3.3` | — | no |
| 1.27.1 | yes | yes | 22 | no — too new |

Two separate traps here:

- **Unbundled wheels.** Several `eclipse-sumo` macOS wheels link against pinned
  Homebrew libraries that are not present, so the binary fails at `dyld` load
  time with `Library not loaded: /opt/homebrew/opt/xerces-c/...`.
- **TraCI API version.** Veins 5.3.1 supports TraCI API versions 15–21
  (`src/veins/modules/mobility/traci/TraCICommandInterface.cc:41`). SUMO 1.27.1
  reports version **22**, so it connects and then aborts with
  `TraCI server reports unsupported TraCI API version: 22`.

SUMO 1.18.0 was the original target but has no `arm64` wheel — only `x86_64`,
which would require running under Rosetta. 1.21.0 is the nearest version with a
native, working, protocol-compatible build.

Homebrew is not a good route here: the `dlr-ts/sumo` tap ships SUMO 1.20.0 with
bottles only up to `arm64_sonoma`. On macOS 26 it would build from source
(30+ minutes) and still land on a version whose wheel problems it shares.

Nix is also a dead end: `sumo` in nixpkgs 25.11 fails to evaluate because its
`fox-1.6.57` dependency is marked broken on Darwin, and `opp_env` does not
propagate `NIXPKGS_ALLOW_BROKEN` into the flake evaluation.

### 2.4 Reinstalling SUMO from scratch

```bash
python3 -m venv ~/Documents/omnet-projects/sumo-venv
~/Documents/omnet-projects/sumo-venv/bin/pip install "eclipse-sumo==1.21.0"

for b in sumo sumo-gui netconvert netedit netgenerate duarouter \
         polyconvert od2trips activitygen marouter jtrrouter dfrouter; do
    ln -sf ~/Documents/omnet-projects/sumo-venv/bin/$b ~/.local/bin/$b
done
```

`~/.local/bin` is already on your `PATH` and is writable, so no `sudo` is
needed. `/usr/bin` is SIP-protected on macOS and cannot be written to at all,
so a Linux-style `/usr/bin/sumo` layout is not reproducible here.

`SUMO_HOME` must point at the package data directory, not the venv root:

```
~/Documents/omnet-projects/sumo-venv/lib/python3.9/site-packages/sumo
```

---

## 3. How a run works

```
run_gbsed.sh
  │
  ├─ sources the Nix profile           (puts nix on PATH)
  ├─ exports SUMO_HOME / PATH          (points at the SUMO 1.21.0 venv)
  ├─ starts bin/veins_launchd          (listens on TCP 9999)
  │
  └─ opp_env run omnetpp-6.4.0
       └─ bin/veins_run -p -u Cmdenv   (from the GBSEDApp directory)
            └─ opp_run + libveins.dylib
                 └─ TraCIScenarioManagerLaunchd
                      └── TCP 9999 ──> veins_launchd
                                          └─ spawns a private SUMO instance
                                             from launchd.xml, proxies TraCI
```

`veins_launchd` is a broker. Rather than the simulation talking to one
long-lived SUMO process, it hands out a fresh sandboxed SUMO instance per
simulation run, seeded from the files listed in `launchd.xml`. This is what
lets several runs proceed without stepping on each other.

The `-p` flag on `veins_run` makes it pass absolute paths for the NED and
library directories, which is required when running from a subdirectory rather
than the Veins root.

---

## 4. File reference

### 4.1 Scenario directory — `src/veins/modules/application/gbsed/GBSEDApp/`

| File | Role |
|---|---|
| `omnetpp.ini` | The master configuration. Radio parameters, mobility, which node sends and which receives, file paths. This is the file you will edit most. |
| `GBSEDScenerio.ned` | Declares `network GBSEDScenario extends Scenario`. Veins' `Scenario` already contains the TraCI manager, connection manager, annotation manager, and the dynamically created `node[]` vector. |
| `launchd.xml` | Tells `veins_launchd` which files to copy into each sandboxed SUMO instance, and which is the config file. |
| `gbsed.sumo.cfg` | The SUMO configuration: names the network and route files, sets the 0–200 s window. |
| `gbsed.net.xml` | The road network — a 2×2 grid of junctions `A0(0,0)`, `A1(0,1000)`, `B0(1000,0)`, `B1(1000,1000)`, with 8 directed edges between them. |
| `gbsed.rou.xml` | Vehicle definitions. One `vType` (`car`, `maxSpeed=15` m/s) and two vehicles, `veh0` and `veh1`, both on `route0` (the single edge `A0A1`). |
| `config.xml` | Physical-layer models: `SimplePathlossModel` with `alpha=2.0`, and `Decider80211p` at 5.890 GHz. |
| `antenna.xml` | Sampled antenna patterns. The scenario selects the `monopole` entry; `panorama` and `patch` are also defined but unused. |
| `scene_data/` | **Input.** Where you place the file(s) the sender transmits. Not in version control — you must create it. |
| `received/` | **Output.** Created automatically by the receiver. |
| `results/` | **Output.** OMNeT++ scalar (`.sca`) and vector (`.vec`) recordings, created automatically. |

### 4.2 Application source — `src/veins/modules/application/gbsed/`

| File | Role |
|---|---|
| `GBSEDApp.ned` | Module interface. Declares the parameters `isSender`, `filePath`, `outputDir`, `chunkSize`. Extends `DemoBaseApplLayer`. |
| `GBSEDApp.h` | Class declaration — sender and receiver state, both live in this one class. |
| `GBSEDApp.cc` | The implementation. Base64 codec, file loading, chunk scheduling, reassembly, output. |
| `GBSEDMessage.msg` | The wire format. Generates `GBSEDMessage_m.h/.cc` at build time. |

### 4.3 Repository-level

| File | Role |
|---|---|
| `run_gbsed.sh` | Convenience wrapper — the whole launch sequence in one command. |
| `configure` | Generates `src/Makefile` via `opp_makemake`. Run once. |
| `bin/veins_run` | Generated by `make`. Wraps `opp_run` with the right NED/library paths. |
| `bin/veins_launchd` | The SUMO broker described above. |

---

## 5. The protocol

`GBSEDMessage` extends `BaseFrame1609_4` and carries:

| Field | Meaning |
|---|---|
| `chunkIndex` | 0-based index of this chunk |
| `totalChunks` | how many chunks make up the file |
| `totalSize` | the file's size in bytes |
| `chunkData` | base64 of this chunk's bytes |
| `fileName` | basename of the source file |
| `isLastFile` | true if this is the final file in the send queue |

**Sender.** At `initialize()` it splits `filePath` on `;` into a queue, loads
the first file, and schedules the first send for `t + 10 s` — deliberately past
the point where TraCI has created the vehicles. From then it broadcasts one
chunk every 2 seconds. When a file is exhausted it loads the next one after a
2-second gap.

**Receiver.** On the first message, or whenever `fileName` changes, it allocates
a `totalSize` buffer and a `totalChunks` bitmap. Each chunk is decoded and
copied to `chunkIndex * chunkSize`. Once every chunk has arrived the file is
written and the counter advances.

**There is no acknowledgement, no retransmission, and no ordering requirement.**
Chunks are plain broadcasts. Any chunk lost to interference or range is lost
permanently, and the file is simply never completed — the receiver logs a
warning at the next file switch, if there is one. Base64 also inflates every
chunk by 4/3 on the air.

---

## 6. Configuration

Key parameters in `omnetpp.ini`:

```ini
sim-time-limit = 200s

*.manager.port = 9999                 # must match veins_launchd
*.manager.launchConfig = xmldoc("launchd.xml")
*.manager.updateInterval = 1s

*.connectionManager.maxInterfDist = 2600m
*.**.nic.mac1609_4.txPower = 20mW
*.**.nic.mac1609_4.bitrate = 6Mbps
*.**.nic.phy80211p.minPowerLevel = -110dBm

*.node[*].applType = "org.car2x.veins.modules.application.gbsed.GBSEDApp"

*.node[0].appl.isSender = true
*.node[0].appl.filePath = "scene_data/scene_0000.bin"
*.node[1].appl.isSender = false
*.node[1].appl.outputDir = "received"
```

Paths are relative to the working directory, which is the `GBSEDApp` directory.

To send several files from one node, separate them with semicolons:

```ini
*.node[0].appl.filePath = "scene_data/a.bin;scene_data/b.bin"
```

Node indices follow SUMO vehicle creation order: `veh0` becomes `node[0]`,
`veh1` becomes `node[1]`.

---

## 7. Limits

### 7.1 Maximum transferable file size — about 30 KB

This is the constraint most likely to surprise you, and it is **not**
`sim-time-limit`.

From SUMO's own trip output:

```
veh0  depart=0.00  arrival=74.00     <- the sender
veh1  depart=3.00  arrival=85.00     <- the receiver
```

Both vehicles traverse the single 1000 m edge `A0A1` at 15 m/s and are then
removed from the network. The sender therefore only exists from `t=0` to
`t=74`, and it does not start transmitting until `t=10`. At one 1000-byte
chunk every 2 seconds, that is roughly 33 chunks — and the last few are lost in
practice.

Measured on this setup:

| File size | Chunks | Last chunk at | Result |
|---|---|---|---|
| 12 KB | 12 | t=32 s | completes |
| 20 KB | 20 | t=48 s | completes |
| 24 KB | 24 | t=56 s | completes |
| 28 KB | 28 | t=64 s | completes |
| 30 KB | 30 | t=68 s | completes |
| 32 KB | 32 | t=72 s | **never completes** |
| 40 KB | 40 | t=88 s | **never completes** |

`sim-time-limit = 200s` is misleading: nothing can happen after `t≈74`, because
by then the sender no longer exists. When a transfer does not complete, the
simulation still exits reporting success — it just produces no output file.

To move more data, any of these work:

- raise `chunkSize` (the message grows, but the per-chunk overhead amortises)
- shorten the 2-second send interval — it is **hardcoded** as `simTime() + 2.0`
  in `GBSEDApp.cc`, not a NED parameter
- give the vehicles a longer route in `gbsed.rou.xml`
- lower `maxSpeed` in the `vType` so the vehicles stay in the network longer

> **Updated.** The 2-second interval and the 10-second start are no longer
> hardcoded: they are the `sendInterval` and `startTime` NED parameters, set
> in `omnetpp.ini`. The transferable volume is now
> `(vehicle_lifetime - startTime) / sendInterval` chunks, and the vehicle
> lifetime follows from the route and `maxSpeed` in `gbsed.rou.xml`. The
> table above still describes the stock 15 m/s single-edge route.

### 7.2 Other constraints

- Base64 encoding inflates on-air payload by roughly 33%.
- Exactly two nodes are assumed. `node[0]` sends, `node[1]` receives. More
  vehicles in `gbsed.rou.xml` would be created but would sit idle.
- Sender and receiver roles are static, set at initialization.
- Port 9999 must be free. `run_gbsed.sh` reuses an existing listener if one is
  already there.

---

## 8. Changes made to this repository

Four changes were needed. Three were required to build or run at all.

### 8.1 `TraCILauncher.cc` — missing `<csignal>` (build blocker)

```
error: use of undeclared identifier 'kill'
```

Upstream Veins 5.3.1 calls `kill(pid, 15)` without including `<csignal>`.
Older compilers pulled it in transitively; the clang in this toolchain does
not. Added the include next to the existing `<sys/wait.h>`.

### 8.2 `GBSEDScenerio.ned` — missing package declaration (run blocker)

```
Declared package '' does not match expected package
'org.car2x.veins.modules.application.gbsed.GBSEDApp'
```

The scenario lives inside the `src/veins` NED source tree, where OMNeT++ derives
the expected package name from the directory path. With no `package` line, NED
loading failed outright and **no** NED types loaded. Added:

```ned
package org.car2x.veins.modules.application.gbsed.GBSEDApp;
```

> Worth knowing: scenario directories conventionally live under `examples/`,
> not inside `src/`. The current location works now that the package is
> declared, but moving it would be more idiomatic and would remove this class
> of problem.

### 8.3 `GBSEDApp.cc` — `writeReceivedFile()` failed silently (correctness bug)

The original:

```cpp
std::string outPath = outputDir + "/received_" + currentReceivingFile;
std::ofstream out(outPath, std::ios::binary);
out.write((char*)receivedBuffer.data(), receivedBuffer.size());
out.close();
EV_INFO << "GBSEDApp: wrote received file to " << outPath << endl;
```

Three problems. The output directory was never created, so if `received/` did
not exist the stream failed to open. Neither the open nor the write was checked.
And the log line claimed success unconditionally. A fully received file was
therefore discarded while the simulation reported a clean run — the failure was
invisible.

The fix creates the directory with `std::filesystem::create_directories`, joins
paths through `std::filesystem::path` (so a trailing slash in `outputDir` is
handled), and raises `cRuntimeError` if the directory cannot be created, the
file cannot be opened, or the write fails. Verified: pointing `outputDir` at an
unwritable path now aborts with

```
GBSEDApp: could not create output directory '/System/nope/deep': Operation not permitted
```

instead of silently dropping the data.

### 8.4 `run_gbsed.sh` — new file

Wraps the Nix sourcing, `SUMO_HOME` export, `veins_launchd` lifecycle, and
`opp_env run` invocation. It is a convenience only; nothing depends on it.

---

## 9. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `This OMNeT++ installation cannot be used outside an opp_env shell` | You ran `make` directly. Go through `opp_env run`. |
| `Nix not installed -- see installation hints` | Nix is not on `PATH`. Source `/nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh`. |
| `Could not connect to TraCI server: Connection refused` | `veins_launchd` is not running. Start it, or use `run_gbsed.sh`. |
| `TraCI server reports unsupported TraCI API version: 22` | SUMO is too new. Veins 5.3.1 needs API ≤ 21 — use SUMO 1.21.0. |
| `Library not loaded: /opt/homebrew/opt/xerces-c/...` | An `eclipse-sumo` wheel that is not self-contained. Use 1.21.0. |
| `Declared package '' does not match expected package ...` | A `.ned` file under `src/veins` without a `package` line. |
| Run succeeds but `received/` is empty | The transfer never completed — the file is almost certainly over the ~30 KB ceiling. See §7.1. |
| `Cannot assign '...' to parameter: syntax error` | String parameters overridden on the command line need embedded quotes: `--*.node[1].appl.outputDir='"path"'`. |

Useful commands:

```bash
# is the broker listening?
lsof -iTCP:9999 -sTCP:LISTEN

# broker log
tail -f /tmp/veins_launchd.log

# per-run SUMO logs
tail -f /var/folders/**/T/sumo-launchd.log

# verify a transfer
shasum src/veins/modules/application/gbsed/GBSEDApp/scene_data/scene_0000.bin \
       src/veins/modules/application/gbsed/GBSEDApp/received/received_scene_0000.bin
```

To see the application's own log lines, turn off express mode — `EV_INFO`
output is suppressed while `cmdenv-express-mode = true`:

```bash
./run_gbsed.sh -u Cmdenv --cmdenv-express-mode=false
```

---

## 10. GBSED semantic pipeline

The application no longer moves arbitrary files in the abstract: the `.bin`
files it carries are serialized scene graphs produced by the GBSED semantic
encoder in the sibling `gbsed` repository. **For setup and the full workflow,
read `RUNNING.md` in this repository** -- it is the portable, machine-agnostic
guide. This document is the macOS/`opp_env` build history and the rationale
behind each change. In short:

```
gbsed_encode.py  ->  scene_data/frame_%04d.bin  ->  [this simulation]
                 ->  received/received_frame_%04d.bin  ->  gbsed_decode.py
```

A scene graph serializes to 200 B - 4 KB, so a frame is normally one chunk.

### 10.1 Changes made for it

**`GBSEDApp.ned` / `GBSEDApp.cc` - timing parameters.** `startTime` (default
10 s) and `sendInterval` (default 2 s) replace the hardcoded `simTime() + 10.0`
and `simTime() + 2.0`. Without these the transmittable volume was fixed by the
source, which made the frame count the experiment's binding constraint rather
than the radio.

**`GBSEDMessage.msg` - `txPosX` / `txPosY`.** The sender stamps its own
position into every chunk so the receiver can compute the separation at
reception. A lost chunk otherwise has no distance associated with it, and
delivery could not be correlated with range.

**`GBSEDApp.cc` - `logChunk()`.** Appends `tx_log.csv` (sender) and
`rx_log.csv` (receiver) to `outputDir`, one row per chunk, gated on the
`writeCsvLog` parameter. Also records `chunksSent` / `filesReceived` /
`chunksHeard` scalars in the `.sca` output.

**`gbsed.rou.xml` - diverging routes.** `veh0` north on `A0A1`, `veh1` east on
`A0B0`, both at 8 m/s. Previously both vehicles drove the same edge at the
same speed and stayed within range for the whole run, so delivery was always
100% and the simulation could not measure anything. They now separate as
`sqrt(2)*8*t` and cross the range cliff (near 480 m) mid-sequence. 8 m/s also
makes each vehicle live ~125 s, outlasting the send queue, so distance is the
only variable. The original route file is kept at
`src/veins/modules/application/gbsed/.orig/gbsed.rou.xml`.

> A caution learned the hard way: SUMO rejects `--` inside an XML comment
> ("'--' sequence is illegal in comment"), and the resulting failure surfaces
> in OMNeT++ as `Attempted to read past end of byte buffer` in
> `TraCICommandInterface::getVersion()`, which looks like a TraCI version
> problem rather than a malformed route file.

### 10.2 Measured result

20 frames from a driving sequence, 22 chunks, sent every 2.5 s from t=10:

| | in-range baseline | range sweep |
|---|---|---|
| route | both on `A0A1`, 15 m/s | diverging, 8 m/s |
| delivered | 20/20 | 14/20 |
| semantic fidelity | 100% | 70% |
| cliff | never reached | ~480 m |

Every delivered frame reconstructs bit-exact and node/edge-identical, in both
scenarios. Frame 14 is the instructive failure: a two-chunk frame whose first
chunk arrived at 488 m and whose second did not, so the whole scene graph was
discarded.
