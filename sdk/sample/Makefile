# A WPS plug-in, built with the SDK (the wps-devel package): make, then
# make install (your own plug-ins: ~/.config/wps/plugins), and log in again.
# (wpsctl plugin new NAME makes one like this; WPS_SDK and WPS_INC say
# where the SDK is, for a checkout: make WPS_SDK=... WPS_INC=...)
NAME    ?= WPImageFile
SO      ?= $(shell echo $(NAME) | tr A-Z a-z).so
WPS_SDK ?= /usr/share/wps/sdk
WPS_INC ?= /usr/include/wps
CC      ?= cc
PYTHON  ?= python3

all: $(SO)

gen/$(NAME).reg.c: $(NAME).wpi
	mkdir -p gen
	$(PYTHON) $(WPS_SDK)/wpoc.py -I $(WPS_SDK)/classes -o gen $(NAME).wpi

$(SO): $(NAME).c gen/$(NAME).reg.c
	$(CC) -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fPIC -shared -I$(WPS_INC) -Igen -o $@ $(NAME).c gen/$(NAME).reg.c

install: $(SO)
	install -d $(HOME)/.config/wps/plugins
	install -m 755 $(SO) $(HOME)/.config/wps/plugins/

clean:
	rm -rf gen $(SO)

.PHONY: all install clean
