#!/bin/sh
# xzero installer — POSIX sh
# Usage: curl -fsSL https://raw.githubusercontent.com/REPO/main/install.sh | sh
#        curl -fsSL .../install.sh | sh -s -- v0.1.0
# Env: XZERO_REPO (default: xzero/xzero), XZERO_INSTALL_DIR (default: $HOME/.local/bin), GITHUB_TOKEN
set -eu

REPO="${XZERO_REPO:-xzero/xzero}"
INSTALL_DIR="${XZERO_INSTALL_DIR:-$HOME/.local/bin}"
VERSION="${1:-latest}"

# Parse --version style if passed via sh -s --
for arg in "$@"; do
  case "$arg" in
    v*|latest) VERSION="$arg" ;;
  esac
done

# Detect OS
OS="$(uname -s 2>/dev/null || echo unknown)"
case "$OS" in
  Linux*) OS=linux ;;
  Darwin*) OS=darwin ;;
  MINGW*|MSYS*|CYGWIN*) OS=windows ;;
  *) echo "Unsupported OS: $OS" >&2; exit 1 ;;
esac

# Detect Arch — modern only: x64, arm64
ARCH="$(uname -m 2>/dev/null || echo unknown)"
case "$ARCH" in
  x86_64|amd64) ARCH=x64 ;;
  aarch64|arm64) ARCH=arm64 ;;
  *) echo "Unsupported arch: $ARCH (only x64 and arm64 supported)" >&2; exit 1 ;;
esac

echo "Installing xzero $VERSION for $OS-$ARCH -> $INSTALL_DIR"

# Resolve latest tag if needed
if [ "$VERSION" = "latest" ]; then
  if command -v curl >/dev/null 2>&1; then
    VERSION="$(curl -fsSL -H "Authorization: Bearer ${GITHUB_TOKEN:-}" "https://api.github.com/repos/${REPO}/releases/latest" | grep '"tag_name"' | sed -E 's/.*"tag_name": *"([^"]+)".*/\1/' || true)"
  elif command -v wget >/dev/null 2>&1; then
    VERSION="$(wget -qO- --header="Authorization: Bearer ${GITHUB_TOKEN:-}" "https://api.github.com/repos/${REPO}/releases/latest" | grep '"tag_name"' | sed -E 's/.*"tag_name": *"([^"]+)".*/\1/' || true)"
  fi
  if [ -z "$VERSION" ] || [ "$VERSION" = "null" ]; then
    echo "Failed to resolve latest version. Set XZERO_REPO correctly or pass version explicitly (e.g. v0.1.0)." >&2
    exit 1
  fi
  echo "Latest is $VERSION"
fi

# Normalize version has leading v
case "$VERSION" in
  v*) ;;
  *) VERSION="v$VERSION" ;;
esac

# Artifact name matches release.yml: xzero-<version>-<os>-<arch>.tar.gz (darwin/linux) or .zip (windows)
if [ "$OS" = "windows" ]; then
  ARTIFACT="xzero-${VERSION}-${OS}-${ARCH}.zip"
else
  ARTIFACT="xzero-${VERSION}-${OS}-${ARCH}.tar.gz"
fi

URL="https://github.com/${REPO}/releases/download/${VERSION}/${ARTIFACT}"
SHA_URL="https://github.com/${REPO}/releases/download/${VERSION}/SHA256SUMS"

echo "Downloading $URL"

TMPDIR="$(mktemp -d 2>/dev/null || mktemp -d -t xzero)"
trap 'rm -rf "$TMPDIR"' EXIT INT TERM

# Download
if command -v curl >/dev/null 2>&1; then
  curl -fL --progress-bar -o "$TMPDIR/$ARTIFACT" "$URL" || { echo "Download failed: $URL" >&2; exit 1; }
  # Try SHA256SUMS (best effort)
  curl -fsSL -o "$TMPDIR/SHA256SUMS" "$SHA_URL" 2>/dev/null || true
elif command -v wget >/dev/null 2>&1; then
  wget -O "$TMPDIR/$ARTIFACT" "$URL" || { echo "Download failed" >&2; exit 1; }
  wget -qO "$TMPDIR/SHA256SUMS" "$SHA_URL" 2>/dev/null || true
else
  echo "Need curl or wget" >&2; exit 1
fi

# Verify checksum if available
if [ -f "$TMPDIR/SHA256SUMS" ] && command -v sha256sum >/dev/null 2>&1; then
  # SHA256SUMS contains entries for all archs; extract matching line
  if grep -q "$ARTIFACT" "$TMPDIR/SHA256SUMS" 2>/dev/null; then
    (cd "$TMPDIR" && grep "$ARTIFACT" SHA256SUMS | sha256sum -c -) || echo "Warning: checksum mismatch" >&2
  fi
elif command -v shasum >/dev/null 2>&1 && [ -f "$TMPDIR/SHA256SUMS" ]; then
  (cd "$TMPDIR" && grep "$ARTIFACT" SHA256SUMS | shasum -a 256 -c -) || echo "Warning: checksum mismatch" >&2
fi

# Extract
mkdir -p "$INSTALL_DIR"
if [ "$OS" = "windows" ]; then
  if command -v unzip >/dev/null 2>&1; then
    unzip -o "$TMPDIR/$ARTIFACT" -d "$TMPDIR/out" >/dev/null
    # release zip contains xzero-windows-*.exe + SHA256SUMS, pick exe
    find "$TMPDIR/out" -name "xzero*.exe" -type f | head -n1 | xargs -I{} cp "{}" "$INSTALL_DIR/xzero.exe"
    chmod +x "$INSTALL_DIR/xzero.exe" 2>/dev/null || true
  else
    echo "Need unzip for Windows artifact" >&2; exit 1
  fi
else
  tar -xzf "$TMPDIR/$ARTIFACT" -C "$TMPDIR"
  # Artifact is tar with binary + SHA256SUMS; find binary
  BIN="$(find "$TMPDIR" -maxdepth 3 -name "xzero-*" -type f | head -n1)"
  if [ -z "$BIN" ]; then BIN="$(find "$TMPDIR" -name "xzero*" -type f | head -n1)"; fi
  cp "$BIN" "$INSTALL_DIR/xzero"
  chmod +x "$INSTALL_DIR/xzero"
fi

# Verify
if [ -x "$INSTALL_DIR/xzero" ]; then
  echo "Installed to $INSTALL_DIR/xzero"
  "$INSTALL_DIR/xzero" --version || true
else
  if [ -x "$INSTALL_DIR/xzero.exe" ]; then
    echo "Installed to $INSTALL_DIR/xzero.exe"
    "$INSTALL_DIR/xzero.exe" --version || true
  fi
fi

# PATH hint
case ":$PATH:" in
  *":$INSTALL_DIR:"*) ;;
  *)
    echo ""
    echo "Add to PATH:"
    echo "  export PATH=\"$INSTALL_DIR:\$PATH\"  # then restart shell"
    echo "  Or run: $INSTALL_DIR/xzero --help"
    ;;
esac

echo "Done. Configure with: $INSTALL_DIR/xzero  (will prompt for base URL / API key)"
