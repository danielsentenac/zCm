#!/usr/bin/env bash
set -euo pipefail

html_dir="${1:-docs/_build/html}"

if [ ! -d "$html_dir" ]; then
  echo "error: HTML output directory not found: $html_dir" >&2
  exit 1
fi

inject_theme_links() {
  local file="$1"
  local tmp
  tmp="$(mktemp)"

  awk '
    /<\/head>/ && !done {
      print "<link href=\"doxygen-awesome.css\" rel=\"stylesheet\" type=\"text/css\"/>";
      print "<link href=\"doxygen-awesome-sidebar-only.css\" rel=\"stylesheet\" type=\"text/css\"/>";
      print "<link href=\"zcm-dark.css\" rel=\"stylesheet\" type=\"text/css\"/>";
      done = 1;
    }
    { print }
  ' "$file" > "$tmp"

  mv "$tmp" "$file"
}

patched=0
for file in "$html_dir"/*.html "$html_dir"/search/*.html; do
  [ -f "$file" ] || continue
  if ! rg -q "doxygen-awesome.css" "$file"; then
    inject_theme_links "$file"
    patched=$((patched + 1))
  fi
done

echo "Patched $patched HTML file(s) with Doxygen Awesome links."
