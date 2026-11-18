BUILD := ../../build/m0
CFLAGS := -O2 -Wall -Wextra -std=gnu11

all: $(BUILD)/fanwatch $(BUILD)/fhtool

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
	rm -f $(BUILD)/fanwatch $(BUILD)/fhtool

.PHONY: all up down run clean
