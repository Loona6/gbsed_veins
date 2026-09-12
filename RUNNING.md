# Running the GBSED pipeline over Veins

End-to-end: a folder of driving images becomes scene graphs, the scene graphs
are serialized and transmitted between two vehicles over IEEE 802.11p in
OMNeT++/Veins with SUMO mobility, and the receiver's bytes are decoded back
into scene graphs and scored against the originals.

```
  images/                                            this repository
     │                                                      │
     ▼  gbsed_encode.py                                     │
  scene_data/frame_%04d.bin      ─── staged ──►   GBSEDApp/scene_data/
             frame_%04d.meta.json                           │
             manifest.json                                  │  node[0] ──802.11p──► node[1]
     │                                                      ▼
     │                                          GBSEDApp/received/received_frame_%04d.bin
     ▼  gbsed_decode.py  ◄──────────────────────────────────┘
  decoded/fidelity.csv
          png/frame_%04d_reconstructed.png
```

The Python half never runs inside the simulation. Encoding is a pre-process,
decoding a post-process, and Veins moves opaque bytes in between. Nothing in
the encoder depends on simulation state, so this costs nothing and keeps runs
fast and reproducible.

This document assumes **no particular OMNeT++ install**. It was developed on
macOS with an `opp_env`-managed OMNeT++ 6.4.0, but the launcher probes for
what you have. Linux and WSL2 with a normal OMNeT++ install are the simpler
case.

---

## 1. What you need

| | version used | notes |
|---|---|---|
| OMNeT++ | 6.4.0 | 6.x generally; `opp_run` must work |
| SUMO | 1.21.0 | **TraCI API 15–21 only** — see §6 |
| Python | 3.12 | for the encoder/decoder |
| graphviz | any | optional, only for `--visualize` |

Two repositories plus one dependency, laid out as siblings:

```
<workspace>/
├── gbsed/            # encoder/decoder  (the semantic layer)
├── roadscene2vec/    # https://github.com/AICPS/roadscene2vec
└── gbsed_veins/      # this repository  (Veins 5.3.1 + the GBSED application)
```

`gbsed` finds `roadscene2vec` automatically if it sits next to it. If yours
lives elsewhere, set `ROADSCENE2VEC_HOME`.

### WSL notes

- **Keep the repositories on the Linux filesystem** (`~/work/...`), not under
  `/mnt/c/`. Builds and simulations are several times slower across the
  9P mount, and file-permission behaviour differs.
- If you clone on Windows and build in WSL, CRLF line endings break the
  `#!/usr/bin/env python3` shebangs. A `.gitattributes` in this repo forces LF
  for scripts; if you already have a broken clone, `git config core.autocrlf
  input` and re-checkout.
- The GUI (`-u Qtenv`) needs a display. WSL2 on Windows 11 provides WSLg out
  of the box. On Windows 10, install VcXsrv and `export DISPLAY=$(grep
  nameserver /etc/resolv.conf | awk '{print $2}'):0`.

---

## 2. Python environment

Only the semantic layer is needed. Do **not** install the full
`gbsed/requirements.txt` unless you also want the Sionna/TensorFlow wireless
model and the MRGCN classifier — it is large and pinned to CUDA 12.6.

```bash
conda create -n gbsed python=3.12 -y && conda activate gbsed
pip install numpy torch torchvision opencv-python networkx pandas matplotlib pyyaml pydot
```

`--visualize` also needs the graphviz `dot` binary:

```bash
sudo apt-get install -y graphviz     # Debian/Ubuntu/WSL
```

Verify:

```bash
cd <workspace>/gbsed && python -c "import gbsed_semantic as g; print('ok:', g._R2V_ROOT)"
```

The first encode downloads ~160 MB of COCO weights for torchvision's
Faster R-CNN. detectron2 is **not** required — it is stubbed out.

---

## 3. Build Veins

```bash
cd <workspace>/gbsed_veins
./configure          # generates src/Makefile via opp_makemake; run once
make MODE=release -j8
```

That produces `out/*/src/libveins.dylib` (or `.so`) and `bin/veins_run`.

If your OMNeT++ is managed by `opp_env`, a bare `make` will refuse with
*"This OMNeT++ installation cannot be used outside an opp_env shell"*. Wrap it:

```bash
opp_env run omnetpp-6.4.0 -w <your-workspace> --no-isolated \
    -c 'cd <workspace>/gbsed_veins && make MODE=release -j8'
```

This repository already carries a fix that upstream Veins 5.3.1 needs on
recent clang: `TraCILauncher.cc` calls `kill()` without including `<csignal>`.

### If you have your own Veins checkout

Simplest is to build this repository — it *is* Veins 5.3.1 with the
application added. If you would rather graft it onto your own tree, copy:

```
src/veins/modules/application/gbsed/          # 4 source files + the scenario
```

and rebuild. The scenario directory declares
`package org.car2x.veins.modules.application.gbsed.GBSEDApp`, which must match
its path under `src/veins`, or NED loading fails outright.

---

## 4. Configure the launcher

`run_gbsed.sh` works out its own environment and normally needs **no
configuration at all**. It:

- finds `sumo` on your PATH (or via `$SUMO_HOME/bin/sumo`);
- derives `SUMO_HOME` by following the binary through any symlinks and
  checking the system, pip-wheel and source-build layouts;
- decides whether OMNeT++ needs wrapping by actually invoking `opp_run` —
  an `opp_env`-managed install puts it on PATH but refuses to run outside
  its shell, so merely finding the binary proves nothing;
- if wrapping is needed, locates the opp_env workspace by looking for the
  `.opp_env_workspace` marker, and picks the `omnetpp-*` project inside it.

If any of that guesses wrong, put overrides in `run_gbsed.local` (git-ignored):

```bash
cp run_gbsed.local.example run_gbsed.local
```

| variable | meaning |
|---|---|
| `SUMO_BIN` | path to `sumo` if not on PATH |
| `SUMO_HOME` | SUMO data directory, if the derivation fails |
| `USE_OPP_ENV` | force `0` (plain install) or `1` (opp_env); default is to probe |
| `OPP_WORKSPACE` | the directory containing `.opp_env_workspace` |
| `OPP_ENV_NAME` | e.g. `omnetpp-6.4.0`; defaults to the first `omnetpp-*` in the workspace |
| `VEINS_PORT` | `veins_launchd` port, default 9999 |

Also export this once, so the Python tools know where the scenario is:

```bash
export GBSED_VEINS_APP=<workspace>/gbsed_veins/src/veins/modules/application/gbsed/GBSEDApp
```

---

## 5. Run it

### 5.1 Encode

```bash
cd <workspace>/gbsed
rm -rf scene_data decoded "$GBSED_VEINS_APP/scene_data" "$GBSED_VEINS_APP/received"
python tools/gbsed_encode.py --images /path/to/your/images --out scene_data --stage "$GBSED_VEINS_APP" --visualize
```

Frames are numbered `frame_0000…`, and nothing deletes old ones — hence the
`rm -rf`. Otherwise a leftover frame from a previous run appears as `LOST`.

Check the `detections` column: you want `car`, `truck`, `bus`, `person` on
most rows. Frames with no traffic participants are skipped; if too many are,
lower `--score-thresh` (try `0.3`). Useful flags: `--device cuda|mps|cpu`,
`--limit N`, `--config`.

The encoder prints the `omnetpp.ini` line to use and warns if the queue
exceeds what the scenario can transmit.

### 5.2 Point the scenario at the frames

The line is long, so let the manifest write it:

```bash
python - <<'PY'
import json, os, re, pathlib
p = pathlib.Path(os.environ["GBSED_VEINS_APP"]) / "omnetpp.ini"
fp = json.load(open("scene_data/manifest.json"))["omnetpp_filePath"]
s = p.read_text()
p.write_text(re.sub(r'^\*\.node\[0\]\.appl\.filePath = .*$',
                    '*.node[0].appl.filePath = "%s"' % fp, s, flags=re.M))
print("filePath set to", fp.count(";") + 1, "frame(s)")
PY
```

### 5.3 Simulate

```bash
cd <workspace>/gbsed_veins
./run_gbsed.sh                 # command line
./run_gbsed.sh -u Qtenv        # GUI
```

Roughly five seconds of wall time for 200 s of simulated time.

### 5.4 Decode and score

```bash
cd <workspace>/gbsed
python tools/gbsed_decode.py --meta scene_data --out decoded --visualize
```

`--received` defaults to `$GBSED_VEINS_APP/received`.

Per frame you get one of four outcomes:

| status | meaning |
|---|---|
| `EXACT` | reconstructed graph is node- and edge-identical to the original |
| `DEGRADED` | decoded, but differs — scored by edge precision/recall/F1 |
| `CORRUPT` | payload arrived but could not be parsed |
| `LOST` | the receiver never completed this file |

Outputs: `decoded/fidelity.csv`, `decoded/png/*_reconstructed.png`, and
`scene_data/png/*_original.png` for comparison.

---

## 6. Two scenarios

The route file decides whether you are *verifying* the chain or *measuring* it.

**Range sweep (shipped default).** `veh0` goes north, `veh1` goes east from
the same junction at 8 m/s, so their separation grows as `sqrt(2)*8*t`. The
802.11p range cliff sits near 480 m and the sequence crosses it mid-run, so
you get a fidelity-versus-distance curve. At 8 m/s both vehicles outlive the
whole send queue, so distance is the only variable.

**In-range baseline.** Both vehicles on the same edge at 15 m/s: they stay
close, nothing is lost, every frame is `EXACT`. Use it to prove the chain is
lossless before attributing anything to the channel.

```bash
cd src/veins/modules/application/gbsed
cp .orig/gbsed.rou.xml GBSEDApp/gbsed.rou.xml     # baseline
git checkout GBSEDApp/gbsed.rou.xml               # back to the sweep
```

### Expected results

20 frames from a driving sequence, 22 chunks, one every 2.5 s from t=10:

| | in-range baseline | range sweep |
|---|---|---|
| delivered | 20/20 | 14/20 |
| semantic fidelity | 100% | 70% |
| mean edge F1 (delivered) | 1.000 | 1.000 |

Every delivered frame reconstructs bit-exact in both scenarios. Frame 14 in
the sweep is the instructive failure: a two-chunk frame whose first chunk
arrived at 488 m and whose second did not, so the whole graph was discarded.

If your SUMO or radio configuration differs, the exact cutoff will move. The
baseline scenario should still give 100%; if it does not, something is wrong
with the chain rather than the channel.

---

## 7. Key parameters

In `GBSEDApp/omnetpp.ini`:

```ini
*.node[0].appl.isSender    = true
*.node[0].appl.filePath    = "scene_data/frame_0000.bin;..."   # ';'-separated queue
*.node[1].appl.outputDir   = "received"
*.node[*].appl.startTime    = 10s      # first chunk; must clear TraCI vehicle creation
*.node[*].appl.sendInterval = 2.5s     # gap between chunks
*.node[*].appl.chunkSize    = 1000     # bytes per chunk, before base64
*.node[*].appl.writeCsvLog  = true     # tx_log.csv / rx_log.csv
```

How many chunks fit in one run:
`(vehicle_lifetime - startTime) / sendInterval`. The vehicle lifetime follows
from the route length and `maxSpeed` in `gbsed.rou.xml`. A scene graph is
200 B – 4 KB, so a frame is normally one chunk. Pass `--tx-budget` to the
encoder so its warning matches your scenario.

Node indices follow SUMO vehicle creation order: `veh0` → `node[0]`.

### Distance logs

With `writeCsvLog = true`, the app writes `tx_log.csv` (sender) and
`rx_log.csv` (receiver) into `outputDir`, one row per chunk, columns
`fileName, chunkIndex, totalChunks, simTime, txX, txY, rxX, rxY, distance`.
The sender stamps its position into every message so the receiver can compute
the separation. `gbsed_decode.py` folds both into `fidelity.csv` as
`tx_time`, `rx_time`, `distance_m`, `chunks_sent`, `chunks_heard` — which is
how a `LOST` frame still reports the range at which it was dropped.

---

## 8. Known limits

- **Shared codebook.** `ACTOR_NAMES` and `RELATION_NAMES` in
  `gbsed/Config/pipeline_extraction.yaml` are index spaces. They are never
  transmitted; node names are re-derived at the receiver from label indices
  plus a positional heuristic. Encoder and decoder must load the same YAML.
  Both hash the two lists into a `codebook` fingerprint and the decoder flags
  a mismatch — but pass `--config` consistently.
- **No integrity check on the wire.** `GBSEDMessage` carries no checksum, and
  `format_loading` ignores the declared length of its last section, so a
  corrupted payload can reshape into a plausible-but-wrong graph rather than
  raising. The decoder compares SHA-256 against `meta.json` after the fact.
- **All-or-nothing frames.** One lost chunk loses the whole frame. Splitting
  the payload by relation slice (`sem_compression` already produces them
  independently) would degrade a frame by one relation type instead. Not
  implemented.
- **Exactly two nodes.** `node[0]` sends, `node[1]` receives, roles fixed at
  initialization. Extra vehicles are created and sit idle.
- Base64 inflates on-air payload by ~33%.

---

## 9. Troubleshooting

| symptom | cause and fix |
|---|---|
| `This OMNeT++ installation cannot be used outside an opp_env shell` | You ran `make` or `opp_run` directly. Go through `run_gbsed.sh`, or wrap the command in `opp_env run`. |
| `'<dir>' is not an opp_env workspace, run 'opp_env init'` | The workspace auto-detection picked the wrong directory, or you set `OPP_WORKSPACE` to one. The right directory is the one containing a `.opp_env_workspace` file — find it with `find ~ -maxdepth 4 -name .opp_env_workspace`. |
| `No opp_env workspace found in ... or its parent directories` | You ran `opp_env shell` from the Veins checkout. Run it from the workspace directory instead — but `run_gbsed.sh` does this for you, so you should not need to. |
| `error: no 'sumo' on PATH` | Install SUMO or set `SUMO_BIN` in `run_gbsed.local`. |
| `TraCI server reports unsupported TraCI API version: 22` | SUMO too new. Veins 5.3.1 accepts API 15–21 (`TraCICommandInterface.cc`). SUMO 1.21.0 reports 21. |
| `Attempted to read past end of byte buffer` in `getVersion()` | SUMO died at startup — usually a malformed `.rou.xml` or `.net.xml`. **Note `--` is illegal inside an XML comment.** Test directly: `sumo -c gbsed.sumo.cfg --no-step-log --end 80`. |
| `Could not connect to TraCI server: Connection refused` | `veins_launchd` is not running. `run_gbsed.sh` starts it; check `/tmp/veins_launchd.log`. |
| `Declared package '' does not match expected package ...` | A `.ned` under `src/veins` without a `package` line matching its directory. |
| Run succeeds but `received/` is empty | Nothing was delivered. Check `tx_log.csv` exists (the sender ran) and `rx_log.csv` (anything was heard). If the sender never ran, `filePath` is wrong. |
| Everything `LOST` after frame N | The vehicles left radio range, or the sender's vehicle left the network. Compare `tx_time` in `fidelity.csv` against the vehicle lifetime. |
| `bad interpreter: ... ^M` | CRLF line endings from a Windows clone. See §1. |
| Qtenv will not open on WSL | No display. See §1. |
| No `EV_INFO` output in Cmdenv | Express mode suppresses it: `./run_gbsed.sh --cmdenv-express-mode=false`. |

Useful commands:

```bash
lsof -iTCP:9999 -sTCP:LISTEN        # or: ss -ltn 'sport = :9999'
tail -f /tmp/veins_launchd.log
sha256sum scene_data/frame_0000.bin received/received_frame_0000.bin
```

---

## 10. Where things live

| path | role |
|---|---|
| `src/veins/modules/application/gbsed/GBSEDApp.{h,cc}` | the application: chunking, base64, reassembly, CSV logs |
| `src/veins/modules/application/gbsed/GBSEDApp.ned` | parameters |
| `src/veins/modules/application/gbsed/GBSEDMessage.msg` | wire format |
| `src/veins/modules/application/gbsed/GBSEDApp/` | the scenario: `omnetpp.ini`, SUMO net/routes, radio config |
| `src/veins/modules/application/gbsed/.orig/` | pre-modification copies of the above |
| `run_gbsed.sh`, `run_gbsed.local.example` | launcher and its per-machine overrides |
| `GBSED.md` | the macOS/`opp_env` setup history and per-change rationale |
| `../gbsed/gbsed_semantic.py` | the shared semantic chain |
| `../gbsed/tools/` | `gbsed_encode.py`, `gbsed_decode.py`, and their README |
