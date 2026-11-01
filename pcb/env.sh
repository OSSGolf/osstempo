# Source this before any kicad-agent call:  source pcb/env.sh
#
# The board source lives here, in osstempo. The toolkit that builds it lives in a
# sibling checkout and is NOT part of this repo. KICAD_AGENT_PROJECT_ROOT points the
# toolkit at this directory as its board root.

OSSTEMPO_PCB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")" && pwd)"
KICAD_APP=/Applications/KiCad/KiCad.app
KICAD_PYVER=$KICAD_APP/Contents/Frameworks/Python.framework/Versions/3.9

export KICAD_AGENT_PROJECT_ROOT="$OSSTEMPO_PCB_ROOT"
# pcb/bin holds the podman shim (see its header) and must win over Homebrew's.
export PATH="$OSSTEMPO_PCB_ROOT/bin:$KICAD_APP/Contents/MacOS:$PATH"

# The toolkit re-executes its own source under KiCad's bundled Python for pcbnew work.
export KICAD_PYTHON="$KICAD_PYVER/bin/python3.9"
export KICAD_PYTHONPATH="$KICAD_PYVER/lib/python3.9/site-packages"

# macOS hands out TMPDIR as /var/folders/..., and /var is a symlink to /private/var.
# The toolkit refuses symlinked components in its KiBot transaction path, so resolve
# it. It must stay OUTSIDE the project root: the toolkit seals a copy of the source
# tree under TMPDIR, and a temp dir inside the root trips its mutation guard.
export TMPDIR="$(cd "${TMPDIR:-/tmp}" && pwd -P)"

# Convenience: the toolkit CLI from the sibling checkout.
export KICAD_AGENT="$OSSTEMPO_PCB_ROOT/../../kicad-agent-toolkit/.venv/bin/kicad-agent"
