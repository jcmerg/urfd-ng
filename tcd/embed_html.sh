#!/bin/bash
# Convert monitor.html to a C const char array for embedding
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
INPUT="monitor.html"
OUTPUT="monitor_html.cpp"
# written to a temporary file first: a run that fails leaves no half file that
# make would take as up to date
TMP="$OUTPUT.tmp"
trap 'rm -f "$TMP"' EXIT

echo "// Auto-generated from monitor.html — do not edit" > "$TMP"
echo "#include <cstddef>" >> "$TMP"
echo "" >> "$TMP"
# the dashboard's Saira font goes in as a data URI, so the page needs nothing from outside
FONT="../dashboard/fonts/saira-latin.woff2"
B64=""
if [ -f "$FONT" ]; then
    B64=$(base64 < "$FONT" | tr -d '\n')
else
    echo "embed_html.sh: $FONT is missing, the monitor page falls back to other fonts" >&2
fi
# the urfd version of the same checkout names the GitHub release this tcd came with
VERSION_H="${URFD_DIR:-../reflector}/Version.h"
URFD_VER=""
if [ -f "$VERSION_H" ]; then
    URFD_VER=$(awk '/#define URFD_VERSION_(MAJOR|MINOR|REVISION)/ { v = v (v == "" ? "" : ".") $3 } END { print v }' "$VERSION_H")
else
    echo "embed_html.sh: $VERSION_H is missing, the monitor page links to the repository" >&2
fi
HTML=$(cat "$INPUT")
HTML="${HTML//\/\*URFD_VERSION\*\//$URFD_VER}"
echo -n 'extern const char monitor_html[] = R"RAWHTML(' >> "$TMP"
printf '%s\n' "${HTML//\/\*SAIRA_WOFF2\*\//$B64}" >> "$TMP"
echo ')RAWHTML";' >> "$TMP"
echo "" >> "$TMP"
echo "extern const unsigned int monitor_html_len = sizeof(monitor_html) - 1;" >> "$TMP"
mv -f "$TMP" "$OUTPUT"
echo "Generated $OUTPUT from $INPUT"
