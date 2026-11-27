#!/bin/sh
# Rebuilds the .COM examples (needs nasm).
cd "$(dirname "$0")" || exit 1
for f in hello hlexit hlprof imulshl crtcstart spin blocks ftstinf jlloop acpisl; do nasm -f bin "$f.asm" -o "$f.com"; done
for t in "1 pmimul" "2 pmpartl" "3 pmstfwd" "4 pmbranch" "5 pmagi" "6 pmbase" "7 pmrand" "8 pmfdadd" "9 pmfdfd" "10 pmaddi"; do
    set -- $t
    nasm -f bin -DTEST="$1" pmbench.asm -o "$2.com"
done
