#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# The screen reader test (G4): in a headless GNOME session, the AT-SPI bus
# and registry (started by hand: the private session cannot activate
# them), tests/gui/a11y.py listening, and the shell's a11y self-test.
#   tests/gnome/with-gnome.sh -- tests/gui/a11y-run.sh
cd "$(dirname "$0")/../.."
/usr/libexec/at-spi-bus-launcher --launch-immediately & L=$!
sleep 1
/usr/libexec/at-spi2-registryd & R=$!
sleep 1
tests/gui/a11y.py 10.5 & P=$!
tests/gui/selftest.sh a11y "touch alpha beta gamma; mkdir delta"
wait $P
kill $R $L
