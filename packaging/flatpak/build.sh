#!/bin/sh
# The Flatpak's build of Masterpiece, run by flatpak-builder in its sandbox.
# Profile-guided when the CI job asked for it by leaving a file named .pgo in
# the source tree: an instrumented build, a training run of the test suite and
# of the program, and the build that uses what they recorded.
set -e
config="-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/app -DMP_BUILD_PLUGIN=OFF -DMP_BUILD_VALIDATOR=OFF"
if [ -f .pgo ]; then
  cmake -S . -B _build $config -DMP_PGO=GENERATE
  cmake --build _build
  _build/tests/mp_tests > _build/train-tests.log 2>&1 || echo "training -- some tests failed"
  _build/apps/MasterpieceApp/MasterpieceApp_artefacts/Release/Masterpiece --train-profile \
    || echo "training -- the program's exercise did not run"
  cmake _build -DMP_PGO=USE
else
  cmake -S . -B _build $config -DMP_BUILD_TESTS=OFF
fi
cmake --build _build
cmake --install _build
