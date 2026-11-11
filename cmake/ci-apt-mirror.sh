# SPDX-License-Identifier: GPL-3.0-only
# Points apt on a CI runner at Ubuntu's own archive, over IPv4, with retries.
#
# The hosted runners read packages from azure.archive.ubuntu.com, listed in
# /etc/apt/apt-mirrors.txt. When that name stops resolving, every Linux and
# Android job fails on its first apt-get install; archive.ubuntu.com serves
# the same packages. The runners have no IPv6 route, so a host answered only
# by its IPv6 addresses (ports.ubuntu.com, for the ARM packages) fails too.
set -eu

for f in /etc/apt/apt-mirrors.txt /etc/apt/sources.list /etc/apt/sources.list.d/*.list \
         /etc/apt/sources.list.d/*.sources; do
  [ -f "$f" ] || continue
  sudo sed -i 's|azure\.archive\.ubuntu\.com|archive.ubuntu.com|g' "$f"
done
printf 'Acquire::Retries "5";\nAcquire::http::Timeout "30";\nAcquire::ForceIPv4 "true";\n' |
  sudo tee /etc/apt/apt.conf.d/80-ci-retries >/dev/null
sudo apt-get update
