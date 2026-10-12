#!/bin/bash
set -euo pipefail

# networks/chunks contains the legacy, incompatible NNUE file from the old
# feature map/exporter. Never overwrite a newly trained compact network with
# those chunks accidentally. Explicit FORCE_REASSEMBLE=1 opts into this legacy
# operation for archival/debug purposes only.
if [[ -s networks/nnue.nnue && "${FORCE_REASSEMBLE:-0}" != "1" ]]; then
  echo "Refusing to overwrite existing networks/nnue.nnue."
  echo "Legacy chunks are not compatible with the compact 5120-feature exporter."
  echo "Set FORCE_REASSEMBLE=1 only if you deliberately want the old network."
  exit 1
fi

cat networks/chunks/nnue.nnue.part.* > networks/nnue.nnue
echo "Reassembled legacy network: $(wc -c < networks/nnue.nnue) bytes"
ls -lh networks/nnue.nnue
