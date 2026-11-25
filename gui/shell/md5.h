/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* MD5 of a string, as 32 hex digits (the thumbnail standard's file names;
 * not for anything secret). */
#ifndef OSD_MD5_H
#define OSD_MD5_H

#include <stdint.h>

void md5_hex(const char *s, char out[33]);

#endif
