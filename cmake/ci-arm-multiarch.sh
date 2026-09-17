#!/usr/bin/env bash
# Set up a Debian multiarch cross-build for a Raspberry Pi target.
#
# Used by the CI workflow, and runnable by hand to reproduce that job locally
# -- which is the point: the ARM jobs are the ones nobody can check by running
# the binary, so being able to rehearse the setup off the runner is worth more
# here than in the native jobs.
#
#   cmake/ci-arm-multiarch.sh arm64 aarch64-linux-gnu
#   cmake/ci-arm-multiarch.sh armhf arm-linux-gnueabihf
set -e

ARCH="${1:?usage: $0 <debian-arch> <triple>}"
TRIPLE="${2:?usage: $0 <debian-arch> <triple>}"
CODENAME=$(awk -F= '$1=="UBUNTU_CODENAME" {print $2}' /etc/os-release)
: "${CODENAME:?could not read UBUNTU_CODENAME from /etc/os-release}"

# Ubuntu 24.04 onwards keeps its sources in deb822 files that declare no
# Architectures. Adding a foreign one without pinning these first makes apt
# ask the amd64-only mirror for its indexes, and the update fails.
for f in /etc/apt/sources.list.d/*.sources; do
  [ -e "$f" ] || continue
  grep -q '^Architectures:' "$f" || sudo sed -i '/^Types: deb$/a Architectures: amd64' "$f"
done

# Ubuntu 22.04 and earlier keep one-line entries in sources.list, which the
# loop above does not touch. Same problem, same fix: say amd64 explicitly, or
# apt asks the amd64-only mirror for ARM indexes and gets a 404.
if [ -f /etc/apt/sources.list ]; then
  sudo sed -i -E 's|^deb[[:space:]]+([a-z+]+:)|deb [arch=amd64] \1|' /etc/apt/sources.list
fi

# ARM lives on ports.ubuntu.com, including -security, for which
# security.ubuntu.com answers 404.
sudo tee "/etc/apt/sources.list.d/$ARCH.sources" >/dev/null <<EOF
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports/
Suites: $CODENAME $CODENAME-updates $CODENAME-backports $CODENAME-security
Components: main restricted universe multiverse
Architectures: $ARCH
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF

sudo dpkg --add-architecture "$ARCH"
sudo apt-get update

# Package names are gcc-<triple>, not <triple>-gcc; the latter is the binary
# name and matches no package.
sudo apt-get install -y cmake ninja-build pkg-config \
  "gcc-$TRIPLE" "g++-$TRIPLE" "binutils-$TRIPLE"

# The libraries the target binary links against.
DEV_LIBS="libasound2-dev libx11-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libxcomposite-dev libfreetype6-dev libfontconfig1-dev \
  libglu1-mesa-dev libxi-dev"

# shellcheck disable=SC2086
sudo apt-get install -y $(for p in $DEV_LIBS; do printf '%s:%s ' "$p" "$ARCH"; done)

# ...and the same set for the HOST, which is not optional and is easy to
# think it is. A cross-build still builds one thing natively: juceaide, the
# generator JUCE runs on the build machine to produce headers and resources.
# It links juce_graphics, so it needs the host's freetype and fontconfig
# headers, and without them the configure step dies inside juceaide with
# "ft2build.h: No such file or directory" -- which reads like a missing
# TARGET library and is not one.
# shellcheck disable=SC2086
sudo apt-get install -y $DEV_LIBS

"$TRIPLE-g++" --version
