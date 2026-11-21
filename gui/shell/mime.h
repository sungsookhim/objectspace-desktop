#ifndef WPS_MIME_H
#define WPS_MIME_H
#include <stdbool.h>
#include <stddef.h>
/* A file's type (shared-mime-info, by its name; text/plain for text). */
const char *mime_of_file(const char *path);
/* Whether a program (a .desktop file, or an executable: any) opens these files. */
bool program_opens(const char *program, const char *const *files, size_t nfiles);
#endif
