#!/bin/bash
#
# Build-agnostic launcher for the GBSED Veins scenario.
#
#   ./run_gbsed.sh              -> Cmdenv (command line)
#   ./run_gbsed.sh -u Qtenv     -> GUI
#
# Any arguments are passed through to opp_run.
#
# The script discovers its environment rather than assuming one, so it works
# on a plain OMNeT++ install (the usual Linux/WSL case) and on an opp_env or
# Nix-managed one. Override any of these if the guess is wrong:
#
#   SUMO_BIN        path to the sumo binary          (default: first on PATH)
#   VEINS_PORT      port veins_launchd listens on    (default: 9999)
#   OPP_ENV_NAME    opp_env project name             (default: omnetpp-6.4.0)
#   OPP_WORKSPACE   opp_env workspace directory      (default: ~/omnetpp-workspace)
#   USE_OPP_ENV     force 1 or 0 instead of probing
#
set -e

VEINS_ROOT="$(cd "$(dirname "$0")" && pwd)"
SIM_DIR="$VEINS_ROOT/src/veins/modules/application/gbsed/GBSEDApp"

# Per-machine overrides live here and are not tracked by git, so everyone can
# keep their own paths without touching this script. See RUNNING.md.
[ -f "$VEINS_ROOT/run_gbsed.local" ] && . "$VEINS_ROOT/run_gbsed.local"

: "${VEINS_PORT:=9999}"
: "${OPP_ENV_NAME:=omnetpp-6.4.0}"
: "${OPP_WORKSPACE:=$HOME/omnetpp-workspace}"

# ---------------------------------------------------------------- SUMO ------
if [ -z "${SUMO_BIN:-}" ]; then
    if command -v sumo >/dev/null 2>&1; then
        SUMO_BIN="$(command -v sumo)"
    elif [ -n "${SUMO_HOME:-}" ] && [ -x "$SUMO_HOME/bin/sumo" ]; then
        SUMO_BIN="$SUMO_HOME/bin/sumo"
    else
        echo "error: no 'sumo' on PATH and SUMO_HOME/bin/sumo not found." >&2
        echo "       Install SUMO, or set SUMO_BIN=/path/to/sumo." >&2
        exit 1
    fi
fi
if [ -z "${SUMO_HOME:-}" ]; then
    # SUMO needs SUMO_HOME to find its data files; derive it from the binary.
    guess="$(cd "$(dirname "$SUMO_BIN")/.." && pwd)"
    [ -d "$guess/data" ] && export SUMO_HOME="$guess"
fi
echo ">> sumo: $SUMO_BIN${SUMO_HOME:+  (SUMO_HOME=$SUMO_HOME)}"

# --------------------------------------------------------- build present? ---
if [ ! -x "$VEINS_ROOT/bin/veins_run" ]; then
    echo "error: bin/veins_run is missing -- the project has not been built." >&2
    echo "       See RUNNING.md, section 'Build Veins'." >&2
    exit 1
fi

# ------------------------------------------------------------- launchd ------
# veins_launchd brokers a fresh sandboxed SUMO instance per simulation run.
port_busy() { (exec 3<>"/dev/tcp/127.0.0.1/$VEINS_PORT") >/dev/null 2>&1 && exec 3<&- ; }

if port_busy; then
    echo ">> veins_launchd already listening on $VEINS_PORT"
else
    echo ">> starting veins_launchd on port $VEINS_PORT"
    "$VEINS_ROOT/bin/veins_launchd" -c "$SUMO_BIN" -p "$VEINS_PORT" \
        >/tmp/veins_launchd.log 2>&1 &
    LAUNCHD_PID=$!
    trap 'kill $LAUNCHD_PID 2>/dev/null || true' EXIT
    sleep 2
    if ! port_busy; then
        echo "error: veins_launchd did not come up; see /tmp/veins_launchd.log" >&2
        exit 1
    fi
fi

mkdir -p "$SIM_DIR/scene_data"    # the receiver creates its own output dir

# ----------------------------------------------------------------- run ------
ARGS="${*:--u Cmdenv}"

# An opp_env-managed OMNeT++ puts opp_run on PATH but refuses to run outside
# its shell, so merely finding the binary proves nothing -- invoke it and see.
if [ -z "${USE_OPP_ENV:-}" ]; then
    USE_OPP_ENV=1
    if command -v opp_run >/dev/null 2>&1 && opp_run -h >/dev/null 2>&1; then
        USE_OPP_ENV=0
    fi
fi

if [ "$USE_OPP_ENV" = "0" ]; then
    echo ">> running natively (opp_run on PATH)"
    cd "$SIM_DIR"
    exec "$VEINS_ROOT/bin/veins_run" -p $ARGS
else
    # Nix backs the opp_env install and is not on PATH in non-interactive shells.
    NIX_PROFILE=/nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh
    [ -f "$NIX_PROFILE" ] && . "$NIX_PROFILE"
    if ! command -v opp_env >/dev/null 2>&1; then
        echo "error: neither opp_run nor opp_env found on PATH." >&2
        echo "       Source your OMNeT++ setenv, or set USE_OPP_ENV=0." >&2
        exit 1
    fi
    echo ">> running via opp_env ($OPP_ENV_NAME, workspace $OPP_WORKSPACE)"
    exec opp_env run "$OPP_ENV_NAME" -w "$OPP_WORKSPACE" --no-isolated \
        -c "cd '$SIM_DIR' && '$VEINS_ROOT/bin/veins_run' -p $ARGS"
fi
