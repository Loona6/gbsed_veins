#!/bin/bash
#
# Build-agnostic launcher for the GBSED Veins scenario.
#
#   ./run_gbsed.sh              -> Cmdenv (command line)
#   ./run_gbsed.sh -u Qtenv     -> GUI
#
# Any arguments are passed through to opp_run.
#
# The script discovers its environment rather than assuming one: it finds
# SUMO, derives SUMO_HOME, and works out whether OMNeT++ needs to be wrapped
# in an opp_env shell. It should need no configuration on a plain Linux/WSL
# install. Override anything it guesses wrong in run_gbsed.local (untracked;
# see run_gbsed.local.example) or in the environment:
#
#   SUMO_BIN        path to the sumo binary
#   SUMO_HOME       SUMO data directory
#   VEINS_PORT      port veins_launchd listens on   (default: 9999)
#   USE_OPP_ENV     force 1 or 0 instead of probing
#   OPP_WORKSPACE   opp_env workspace directory     (auto-detected)
#   OPP_ENV_NAME    opp_env project name            (auto-detected)
#
set -e

VEINS_ROOT="$(cd "$(dirname "$0")" && pwd)"
SIM_DIR="$VEINS_ROOT/src/veins/modules/application/gbsed/GBSEDApp"

# Per-machine overrides, untracked so everyone can keep their own paths.
if [ -f "$VEINS_ROOT/run_gbsed.local" ]; then
    . "$VEINS_ROOT/run_gbsed.local"
fi

: "${VEINS_PORT:=9999}"

die() { echo "error: $*" >&2; exit 1; }

# ---------------------------------------------------------------- SUMO ------
if [ -z "${SUMO_BIN:-}" ]; then
    if command -v sumo >/dev/null 2>&1; then
        SUMO_BIN="$(command -v sumo)"
    elif [ -n "${SUMO_HOME:-}" ] && [ -x "$SUMO_HOME/bin/sumo" ]; then
        SUMO_BIN="$SUMO_HOME/bin/sumo"
    else
        die "no 'sumo' on PATH and \$SUMO_HOME/bin/sumo not found.
       Install SUMO, or set SUMO_BIN in run_gbsed.local."
    fi
fi

# SUMO needs SUMO_HOME to find its data files. Derive it from the binary,
# following symlinks (~/.local/bin/sumo is often a link into a venv) and
# covering the three layouts we have seen: system install, pip wheel, and
# a self-built tree.
if [ -z "${SUMO_HOME:-}" ] || [ ! -d "${SUMO_HOME:-}/data" ]; then
    real_bin="$SUMO_BIN"
    if command -v realpath >/dev/null 2>&1; then
        real_bin="$(realpath "$SUMO_BIN")"
    elif command -v python3 >/dev/null 2>&1; then
        real_bin="$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$SUMO_BIN")"
    fi
    bin_root="$(cd "$(dirname "$real_bin")/.." && pwd)"
    for cand in \
        "$bin_root" \
        "$bin_root/share/sumo" \
        "$bin_root"/lib/python*/site-packages/sumo \
        "$(dirname "$bin_root")/share/sumo"
    do
        if [ -d "$cand/data" ]; then
            export SUMO_HOME="$cand"
            break
        fi
    done
fi
if [ -n "${SUMO_HOME:-}" ]; then
    export SUMO_HOME
    echo ">> sumo: $SUMO_BIN  (SUMO_HOME=$SUMO_HOME)"
else
    echo ">> sumo: $SUMO_BIN  (SUMO_HOME not found; simple scenarios still work)"
fi

# --------------------------------------------------------- build present? ---
[ -x "$VEINS_ROOT/bin/veins_run" ] || die "bin/veins_run is missing -- the project has not been built.
       See RUNNING.md, section 'Build Veins'."

# ------------------------------------------------------- OMNeT++ run mode ---
# An opp_env-managed OMNeT++ puts opp_run on PATH but refuses to run outside
# its shell, so merely finding the binary proves nothing -- invoke it and see.
if [ -z "${USE_OPP_ENV:-}" ]; then
    USE_OPP_ENV=1
    if command -v opp_run >/dev/null 2>&1 && opp_run -h >/dev/null 2>&1; then
        USE_OPP_ENV=0
    fi
fi

if [ "$USE_OPP_ENV" != "0" ]; then
    # Nix backs the opp_env install and is not on PATH in non-interactive shells.
    NIX_PROFILE=/nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh
    # shellcheck disable=SC1090
    [ -f "$NIX_PROFILE" ] && . "$NIX_PROFILE"

    command -v opp_env >/dev/null 2>&1 || die "neither a usable opp_run nor opp_env was found on PATH.
       If you have a normal OMNeT++ install, source its setenv first.
       If it is opp_env-managed, make sure opp_env is installed."

    # An opp_env workspace is a directory containing .opp_env_workspace.
    # Search rather than assume: a wrong guess fails deep inside opp_env with
    # a message that does not say which directory it should have been.
    is_ws() { [ -n "$1" ] && [ -e "$1/.opp_env_workspace" ]; }

    if ! is_ws "${OPP_WORKSPACE:-}"; then
        found=""
        # Cheap candidates first, then a bounded search under $HOME.
        for cand in "$HOME/omnetpp-workspace" "$HOME/Documents/omnet-projects" \
                    "$(dirname "$VEINS_ROOT")" "$VEINS_ROOT" "$PWD"; do
            if is_ws "$cand"; then found="$cand"; break; fi
        done
        if [ -z "$found" ]; then
            found="$(find "$HOME" -maxdepth 4 -name .opp_env_workspace -print 2>/dev/null \
                     | head -n1 | xargs -I{} dirname {} 2>/dev/null || true)"
        fi
        [ -n "$found" ] || die "could not find an opp_env workspace (a directory containing
       .opp_env_workspace). Set OPP_WORKSPACE in run_gbsed.local, or run
       'opp_env init' in the directory that holds your OMNeT++ install."
        OPP_WORKSPACE="$found"
    fi

    # Pick the OMNeT++ project inside that workspace if not told which.
    if [ -z "${OPP_ENV_NAME:-}" ]; then
        OPP_ENV_NAME="$(ls -1d "$OPP_WORKSPACE"/omnetpp-* 2>/dev/null \
                        | head -n1 | xargs -I{} basename {} 2>/dev/null || true)"
        [ -n "$OPP_ENV_NAME" ] || die "no omnetpp-* project found in $OPP_WORKSPACE.
       Set OPP_ENV_NAME in run_gbsed.local."
    fi
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
    port_busy || die "veins_launchd did not come up; see /tmp/veins_launchd.log"
fi

mkdir -p "$SIM_DIR/scene_data"    # the receiver creates its own output dir

# ----------------------------------------------------------------- run ------
ARGS="${*:--u Cmdenv}"

if [ "$USE_OPP_ENV" = "0" ]; then
    echo ">> running natively (opp_run on PATH)"
    cd "$SIM_DIR"
    exec "$VEINS_ROOT/bin/veins_run" -p $ARGS
else
    echo ">> running via opp_env ($OPP_ENV_NAME, workspace $OPP_WORKSPACE)"
    exec opp_env run "$OPP_ENV_NAME" -w "$OPP_WORKSPACE" --no-isolated \
        -c "cd '$SIM_DIR' && '$VEINS_ROOT/bin/veins_run' -p $ARGS"
fi
