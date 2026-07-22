#!/bin/bash
# Reassemble NNUE from chunks
cat networks/chunks/nnue.nnue.part.* > networks/nnue.nnue
echo "Reassembled $(wc -c < networks/nnue.nnue) bytes"
ls -lh networks/nnue.nnue
