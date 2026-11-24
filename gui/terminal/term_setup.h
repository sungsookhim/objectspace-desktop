/* Objectspace Terminal's looks from Objectspace's settings objects (term_setup.c). */
#ifndef OSD_TERM_SETUP_H
#define OSD_TERM_SETUP_H

#include <stdbool.h>
#include <stddef.h>

/* The scheme in use and the System's frame options applied; the System's
 * Terminal font into font ("" left as it was). False: no osdd. */
bool term_setup_read(char *font, size_t fn);

#endif
