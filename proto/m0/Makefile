# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
BUILD := ../../build/m0
CFLAGS := -O2 -Wall -Wextra -std=gnu11

all: $(BUILD)/fanwatch $(BUILD)/fhtool $(BUILD)/watchd

$(BUILD)/%: %.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

up:
	./testbed.sh up

down:
	./testbed.sh down

run: all
	./m0.py

clean:
	rm -f $(BUILD)/fanwatch $(BUILD)/fhtool $(BUILD)/watchd

.PHONY: all up down run clean
