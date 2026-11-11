# SPDX-License-Identifier: GPL-3.0-only
# Points apt on a CI runner at Ubuntu's own archive and has it retry.
#
# The hosted runners read packages from azure.archive.ubuntu.com. When that
# name stops resolving, every Linux and Android job fails on its first
# apt-get install. archive.ubuntu.com serves the same packages.
set -eu

for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
  [ -f "$f" ] || continue
  sudo sed -i 's|azure\.archive\.ubuntu\.com|archive.ubuntu.com|g' "$f"
done
printf 'Acquire::Retries "5";\nAcquire::http::Timeout "30";\n' | sudo tee /etc/apt/apt.conf.d/80-ci-retries >/dev/null
sudo apt-get update
