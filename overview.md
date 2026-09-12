# Project overview — for an AI coding assistant

Read this first. It describes what the project does, how the two repositories
fit together, which invariants break silently if violated, and how to get the
simulation running on a machine that is not the one it was developed on.

`RUNNING.md` is the human setup guide and has more step-by-step detail.
`GBSED.md` is the build history for the reference machine. This file is the
orientation document.

---

## 1. What this is

GBSED is a semantic communication framework for connected vehicles. Instead of
transmitting camera images, it turns a driving scene into a **scene graph**
(actors, lanes, and the spatial relations between them), compresses that graph,
and transmits only the compressed representation. The receiver rebuilds the
graph and can act on it directly.

The published pipeline transmits over a simulated MIMO-OFDM physical layer
(Sionna, 3GPP CDL-C). **This project replaces that channel with a real VANET
simulation**: SUMO mobility, IEEE 802.11p, OMNeT++/Veins.

```
image ──► SceneGraph ──► encode() ──► sem_compression() ──► format_storage() ──► .bin
                                                                                  │
                                                              [ OMNeT++ / Veins / 802.11p ]
                                                                                  │
SceneGraph' ◄── decode() ◄── sem_decompression() ◄── format_loading() ◄──── received .bin
```

### The architecture decision that matters most

**The Python semantic layer never runs inside the simulation.** Encoding is a
pre-process, decoding a post-process, and Veins moves opaque bytes in between.

This was deliberate. The encoder's output does not depend on anything the
simulation knows — same images in, same bytes out, regardless of vehicle
positions or link state. Running it in-loop (via IPC or a subprocess) would
cost ~1 s per frame of wall clock on every run, including every point of a
parameter sweep, and buy nothing. A simulation run currently takes ~0.3 s;
in-loop encoding would make it ~20 s.

If you are asked to move encoding or decoding *into* the simulation loop,
push back unless the goal is one of these two, which genuinely require it:

- **adaptive encoding** — the sender chooses which relation slices to send
  based on live link quality;
- **the receiver acting on the decoded graph** — e.g. braking, feeding back
  into SUMO.

Even for the first, there is a cheaper route: precompute each relation slice
as a separate payload offline and let the C++ app choose among them at send
time. No IPC, full simulation speed.

If IPC is built anyway, the serious trap is **state leakage**: both the sender
and receiver applications live in the *same* OMNeT++ process, so they would
share one Python server. If that server caches the encode result and the
receiver's decode reads it, the receiver gets a perfect graph without the
bytes ever crossing the radio — fidelity becomes 100% forever and the
simulation measures nothing. Any such server must be stateless.

---

## 2. Repository layout

Three directories, expected as siblings:

```
<workspace>/
├── gbsed/            # the semantic layer (encoder/decoder). Python.
├── roadscene2vec/    # https://github.com/AICPS/roadscene2vec  (dependency)
└── gbsed_veins/      # this repo: Veins 5.3.1 + the GBSED application. C++.
```

`gbsed` locates `roadscene2vec` automatically if it is a sibling; otherwise
set `ROADSCENE2VEC_HOME`.

### Key files

| path | what it is |
|---|---|
| `gbsed/gbsed_semantic.py` | **the shared semantic chain.** detectron2 stub, `LiteExtractor`, config loading, the four serialization functions, detector, graph comparison. Single copy of logic that was duplicated between `load_model.ipynb` and `pipeline/pipeline.py`. |
| `gbsed/sgautoencoder/sg_autoencoder.py` | `encode()`, `decode()`, `sem_compression()`, `sem_decompression()`. The paper's core. |
| `gbsed/pipeline/pipeline.py` | the original end-to-end pipeline with the Sionna channel. **Not used by this workflow** — importing it drags in TensorFlow and Sionna. |
| `gbsed/tools/gbsed_encode.py` | images → `.bin` + `.meta.json` + `manifest.json` |
| `gbsed/tools/gbsed_decode.py` | received `.bin` → scene graphs + `fidelity.csv` |
| `gbsed/Config/pipeline_extraction.yaml` | `ACTOR_NAMES`, `RELATION_NAMES`, thresholds. **See invariant 1.** |
| `gbsed_veins/src/veins/modules/application/gbsed/GBSEDApp.{h,cc}` | the application: chunking, base64, reassembly, CSV logs |
| `gbsed_veins/src/veins/modules/application/gbsed/GBSEDMessage.msg` | wire format |
| `gbsed_veins/src/veins/modules/application/gbsed/GBSEDApp/` | the scenario: `omnetpp.ini`, SUMO net/routes, radio config |
| `gbsed_veins/src/veins/modules/application/gbsed/.orig/` | pre-modification copies, for the baseline scenario |
| `gbsed_veins/run_gbsed.sh` | the launcher. Self-configuring; see §5. |

---

## 3. Invariants that break silently

These are the things that produce *plausible wrong answers* rather than errors.
Treat them as constraints when editing.

**1. The codebook is shared, not transmitted.** `ACTOR_NAMES` and
`RELATION_NAMES` in `Config/pipeline_extraction.yaml` are *index spaces*. A
label of `8` means nothing except "position 8 in ACTOR_NAMES". Node names are
**re-derived** at the receiver from label indices plus a positional heuristic
(`sg_autoencoder._get_all_node_names`). Reordering either list silently
mis-decodes every previously encoded `.bin`. Both CLIs hash the two lists into
a `codebook` fingerprint stored in `meta.json`, and the decoder flags a
mismatch — that check is the only guard.

**2. Node identity and feature columns are positional.** `T[r, j, k]` refers to
nodes by index in `list(sg.g.adjacency())`. The feature matrix is built with
`list(node.attr.values())`, so column meaning comes from Python dict insertion
order in `get_nodes_from_bboxes`, and `_get_node()` hardcodes `features[0]`
… `features[8]` to read them back. **Adding an attribute anywhere in
`get_nodes_from_bboxes` silently misreads every column after it.**

**3. `format_loading` ignores its last length field.** It does
`comp_T = to_read[cur_idx:]` — takes the remainder rather than the declared
count. Upstream this is deliberate (the Sionna path pads to a fixed block
size). Over a file transport the byte count is exact, so they agree. But it
means a corrupted payload can reshape into a plausible-but-wrong graph instead
of raising.

**4. NED package must match directory.** A `.ned` under `src/veins` whose
`package` line does not match its path makes NED loading fail outright — *no*
NED types load, not just that one.

**5. `--` is illegal inside an XML comment.** SUMO rejects the route file, and
OMNeT++ surfaces this as `Attempted to read past end of byte buffer` in
`TraCICommandInterface::getVersion()` — which looks like a TraCI version
mismatch. Always validate directly:
`sumo -c gbsed.sumo.cfg --no-step-log --end 80`.

**6. Generated message files are gitignored.** `GBSEDMessage_m.{h,cc}` are
produced by `opp_msgc` from the `.msg` at build time and excluded by
`.gitignore`. Never edit or commit them; edit the `.msg` and rebuild.

**7. Frames are numbered, not cleaned.** The encoder writes `frame_0000…`
and deletes nothing. A leftover frame from a previous run appears in results
as `LOST`. Always clear `scene_data/`, `decoded/` and `received/` first.

---

## 4. Reference environment (the machine this was built on)

| component | version | notes |
|---|---|---|
| macOS | 26.6.1, arm64 (Apple Silicon) | |
| OMNeT++ | 6.4.0 | **managed by `opp_env` 0.36.1**, backed by Nix |
| Veins | 5.3.1 | this repository |
| SUMO | 1.21.0 | pip `eclipse-sumo` in a venv, symlinked into `~/.local/bin` |
| Python | 3.12.13 | conda env `av` at `~/miniconda3/envs/av` |
| torch / torchvision | 2.8.0 / 0.23.0 | CPU; MPS also works |
| detectron2 | **not installed** | stubbed out; see below |

Machine-specific paths on the reference machine:

- opp_env workspace: `~/Documents/omnet-projects` (contains `.opp_env_workspace`)
- SUMO_HOME: `~/Documents/omnet-projects/sumo-venv/lib/python3.9/site-packages/sumo`
- repos: `~/Documents/gbsed_veins`, `~/Desktop/PythonEnvs/gbsed`, `~/Desktop/PythonEnvs/roadscene2vec`

**None of these paths are hardcoded anywhere.** The launcher detects them; the
Python tools use relative paths and environment variables.

### Why these versions

**SUMO 1.21.0 is not arbitrary.** Veins 5.3.1 accepts TraCI API versions 15–21
(`src/veins/modules/mobility/traci/TraCICommandInterface.cc`). SUMO 1.21.0
reports 21; SUMO 1.27.x reports 22 and aborts with
`TraCI server reports unsupported TraCI API version: 22`. Several `eclipse-sumo`
macOS wheels also link against Homebrew libraries that are not present and fail
at dyld load time. On Linux/WSL the packaging problems disappear, but **the
TraCI API ceiling still applies** — do not install a SUMO newer than ~1.21
without checking.

**detectron2 is stubbed, not installed.** `roadscene2vec`'s `image_extractor`
imports it at module level, and its `DefaultPredictor` defaults to
`cfg.MODEL.DEVICE == "cuda"`. `gbsed_semantic.py` installs a fake module before
that import and replaces `RealExtractor` with a `LiteExtractor` that provides
only the two attributes the semantic layer actually uses (`relation_extractor`,
`bev`). Object detection is torchvision's Faster R-CNN instead. **Do not try to
install detectron2 to "fix" this** — the stub is the design.

---

## 5. Configuring on a different machine

`run_gbsed.sh` is self-configuring. It:

1. finds `sumo` on PATH (or `$SUMO_HOME/bin/sumo`);
2. derives `SUMO_HOME` by resolving the binary through symlinks and checking
   the system, pip-wheel and source-build layouts;
3. decides whether OMNeT++ needs an `opp_env` wrapper by **actually invoking
   `opp_run`** — an opp_env-managed install puts `opp_run` on PATH but refuses
   to run outside its shell, so merely finding the binary proves nothing;
4. if wrapping is needed, finds the workspace by the `.opp_env_workspace`
   marker and picks the `omnetpp-*` project inside it.

**On a normal Linux/WSL install with `opp_run` and `sumo` on PATH, it needs no
configuration at all.** Overrides go in `run_gbsed.local` (untracked; copy
`run_gbsed.local.example`): `SUMO_BIN`, `SUMO_HOME`, `USE_OPP_ENV`,
`OPP_WORKSPACE`, `OPP_ENV_NAME`, `VEINS_PORT`.

### Diagnostic procedure

| run this | if it… | conclude |
|---|---|---|
| `opp_run -h` | succeeds | plain OMNeT++ install; `USE_OPP_ENV=0` |
| | says "cannot be used outside an opp_env shell" | opp_env-managed; `USE_OPP_ENV=1` |
| | not found | source OMNeT++'s `setenv`, or install opp_env |
| `find ~ -maxdepth 4 -name .opp_env_workspace` | prints a path | its **parent** is `OPP_WORKSPACE` |
| `sumo --version` | prints ≤ 1.21 | fine |
| | prints ≥ 1.23 | TraCI API likely 22; Veins 5.3.1 will abort |
| `echo $SUMO_HOME && ls $SUMO_HOME/data` | lists files | correct |
| `python -c "import gbsed_semantic as g; print(g._R2V_ROOT)"` | prints a path | roadscene2vec found |
| | `SystemExit` | clone it as a sibling or set `ROADSCENE2VEC_HOME` |

### Python environment

Install **only** the semantic layer's dependencies:

```bash
pip install numpy torch torchvision opencv-python networkx pandas matplotlib pyyaml pydot
```

Do **not** install `gbsed/requirements.txt` — 181 pinned packages including
TensorFlow, Sionna and CUDA 12.6 wheels, none of which this workflow uses.
`--visualize` additionally needs the graphviz `dot` binary
(`apt-get install graphviz`).

The first encode downloads ~160 MB of COCO weights for Faster R-CNN.

### WSL specifics

- **Keep the repos on the Linux filesystem**, not `/mnt/c/`. The 9P mount is
  several times slower and permissions behave differently.
- CRLF line endings from a Windows clone break the `#!/usr/bin/env python3`
  shebangs (`bad interpreter: ...^M`). `.gitattributes` forces LF; for an
  already-broken clone use `git config core.autocrlf input` and re-checkout.
- Qtenv needs a display: WSLg on Windows 11, VcXsrv plus `DISPLAY` on 10.
- A GPU is usable: pass `--device cuda` to the encoder.

### Building

```bash
./configure && make MODE=release -j8
```

Under opp_env, wrap it:
`opp_env run <project> -w <workspace> --no-isolated -c 'cd <repo> && make MODE=release -j8'`.

This repo carries a fix upstream Veins 5.3.1 needs on recent clang:
`TraCILauncher.cc` calls `kill()` without including `<csignal>`.

---

## 6. Running

```bash
export GBSED_VEINS_APP=<path>/gbsed_veins/src/veins/modules/application/gbsed/GBSEDApp

# 1. clear previous artifacts (see invariant 7)
rm -rf <gbsed>/scene_data <gbsed>/decoded "$GBSED_VEINS_APP/scene_data" "$GBSED_VEINS_APP/received"

# 2. encode  (--stage defaults to $GBSED_VEINS_APP)
cd <gbsed> && python tools/gbsed_encode.py --images <images> --out scene_data --visualize

# 3. write the filePath line into omnetpp.ini from the manifest
python - <<'PY'
import json, os, re, pathlib
p = pathlib.Path(os.environ["GBSED_VEINS_APP"]) / "omnetpp.ini"
fp = json.load(open("scene_data/manifest.json"))["omnetpp_filePath"]
p.write_text(re.sub(r'^\*\.node\[0\]\.appl\.filePath = .*$',
                    '*.node[0].appl.filePath = "%s"' % fp, p.read_text(), flags=re.M))
PY

# 4. simulate       (-u Qtenv for the GUI)
cd <gbsed_veins> && ./run_gbsed.sh

# 5. decode and score  (--received defaults to $GBSED_VEINS_APP/received)
cd <gbsed> && python tools/gbsed_decode.py --meta scene_data --out decoded --visualize
```

Steps 2–3 are only needed when the images change.

---

## 7. Expected results — how to tell working from broken

Two scenarios, selected by the route file.

| | in-range baseline | range sweep (**shipped default**) |
|---|---|---|
| routes | both vehicles on `A0A1`, 15 m/s | `veh0` north, `veh1` east, 8 m/s |
| separation | stays small | `sqrt(2)*8*t`, ~100 m → ~710 m |
| delivered | **20/20** | **14/20** |
| semantic fidelity | 100% | 70% |
| mean edge F1 (delivered) | 1.000 | 1.000 |
| range cliff | never reached | ~480 m |

Swap scenarios:

```bash
cd src/veins/modules/application/gbsed
cp .orig/gbsed.rou.xml GBSEDApp/gbsed.rou.xml   # baseline
git checkout GBSEDApp/gbsed.rou.xml             # back to the sweep
```

**Diagnostic rule:** every *delivered* frame must be bit-exact and
node/edge-identical, in both scenarios. Mean edge F1 over delivered frames is
1.000. If a delivered frame is `DEGRADED` or `CORRUPT`, the fault is in the
chain (codebook mismatch, a changed feature column, a serialization edit) —
**not** the channel. The baseline scenario should always give 100%; if it does
not, stop and fix that before interpreting any sweep result.

On a different SUMO or radio configuration the exact cliff will move, so
14/20 is not itself a regression test — 20/20 on the baseline is.

Frame 14 in the sweep is instructive: a two-chunk frame whose first chunk
arrived at 488 m and whose second did not, so the entire graph was discarded.

---

## 8. Parameters and their coupling

In `GBSEDApp/omnetpp.ini`:

```ini
*.node[0].appl.isSender     = true
*.node[0].appl.filePath     = "scene_data/frame_0000.bin;..."   # ';'-separated queue
*.node[1].appl.outputDir    = "received"
*.node[*].appl.startTime    = 10s      # must clear TraCI vehicle creation
*.node[*].appl.sendInterval = 2.5s
*.node[*].appl.chunkSize    = 1000     # bytes, before base64
*.node[*].appl.writeCsvLog  = true
```

**How many chunks fit in one run:**

```
(vehicle_lifetime - startTime) / sendInterval
```

where `vehicle_lifetime = route_length / maxSpeed` from `gbsed.rou.xml`. A
scene graph serializes to 200 B – 4 KB, so a frame is normally one chunk. Pass
`--tx-budget` to the encoder so its warning matches your scenario.

The 8 m/s in the sweep is deliberate, not merely "slower": at 15 m/s the
vehicles are removed at t≈67 s, *before* the send queue finishes, which would
confound distance with the sender disappearing. At 8 m/s they live ~125 s and
outlast the queue, so distance is the only variable.

Node indices follow SUMO vehicle creation order: `veh0` → `node[0]`.

### Distance logs

With `writeCsvLog = true` the app writes `tx_log.csv` (sender) and
`rx_log.csv` (receiver) into `outputDir`, one row per chunk:
`fileName, chunkIndex, totalChunks, simTime, txX, txY, rxX, rxY, distance`.
The sender stamps its position into every message so the receiver can compute
separation. Both are truncated at `initialize()`, so runs do not accumulate.
`gbsed_decode.py` folds them into `fidelity.csv` as `tx_time`, `rx_time`,
`distance_m`, `chunks_sent`, `chunks_heard` — which is how a `LOST` frame
still reports the range at which it was dropped.

---

## 9. Known limitations (deliberate — do not "fix" without asking)

- **No integrity check on the wire.** `GBSEDMessage` has no checksum. Combined
  with invariant 3, a corrupted payload can decode into a wrong graph. The
  decoder compares SHA-256 against `meta.json` after the fact. Adding a
  `crc32` field to the `.msg` is the clean fix and would not change the file
  format.
- **All-or-nothing frames.** One lost chunk loses the whole frame.
  `sem_compression` already produces independent relation slices, so laying
  the payload out as *chunk 0 = labels+features, chunk k = relation slice
  L[k]* would degrade a frame by one relation type instead of losing it. This
  is the highest-value unimplemented improvement.
- **Exactly two nodes.** `node[0]` sends, `node[1]` receives, roles fixed at
  initialization. Extra SUMO vehicles are created and sit idle.
- **Base64** inflates on-air payload by ~33%.
- **No downstream task inference.** The paper runs an MRGCN risk classifier on
  the reconstructed graphs (`pipeline.py` + `Config/pipeline_learning.yaml`).
  That is not wired into this workflow.

---

## 10. Conventions

- Run artifacts (`scene_data/`, `decoded/`, `received/`, `results/`) are
  gitignored in both repos. Never commit them.
- `run_gbsed.local` is untracked by design; `run_gbsed.local.example` is the
  tracked template.
- `.orig/` holds pre-modification copies of the scenario files and **is**
  tracked — the baseline scenario depends on it.
- `.gitattributes` in both repos forces LF on scripts for WSL compatibility.
- The Python tools take `--config`; encoder and decoder must be given the same
  one (invariant 1).
