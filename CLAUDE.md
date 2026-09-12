# GBSED over Veins

**Read `overview.md` first** — it covers the architecture, the invariants that
break silently, the reference environment, and how to configure this on a
different machine.

- `overview.md` — orientation for an AI assistant working on this project
- `RUNNING.md` — human step-by-step setup and run guide
- `GBSED.md` — build history for the reference machine (macOS + opp_env)

This repository is Veins 5.3.1 plus the GBSED application. The Python
encoder/decoder lives in the sibling `gbsed` repository; both are needed.

Quick start: `./run_gbsed.sh` (it is self-configuring — see `overview.md` §5).
