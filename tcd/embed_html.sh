#!/bin/bash
# Convert monitor.html to a C const char array for embedding
INPUT="monitor.html"
OUTPUT="monitor_html.cpp"

echo "// Auto-generated from monitor.html — do not edit" > "$OUTPUT"
echo "#include <cstddef>" >> "$OUTPUT"
echo "" >> "$OUTPUT"
# the dashboard's Saira font goes in as a data URI, so the page needs nothing from outside
FONT="../dashboard/fonts/saira-latin.woff2"
B64=""
[ -f "$FONT" ] && B64=$(base64 < "$FONT" | tr -d '\n')
HTML=$(cat "$INPUT")
echo -n 'extern const char monitor_html[] = R"RAWHTML(' >> "$OUTPUT"
printf '%s\n' "${HTML//\/\*SAIRA_WOFF2\*\//$B64}" >> "$OUTPUT"
echo ')RAWHTML";' >> "$OUTPUT"
echo "" >> "$OUTPUT"
echo "extern const unsigned int monitor_html_len = sizeof(monitor_html) - 1;" >> "$OUTPUT"
echo "Generated $OUTPUT from $INPUT"
