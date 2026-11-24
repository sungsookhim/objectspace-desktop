#ifndef OSD_MIME_H
#define OSD_MIME_H
#include <stdbool.h>
#include <stddef.h>
/* A file's type (shared-mime-info, by its name; text/plain for text). */
const char *mime_of_file(const char *path);
/* ... from its name alone (no file read: for drawing), or NULL. */
const char *mime_of_name(const char *name);
/* Whether a program (a .desktop file, or an executable: any) opens these files. */
bool program_opens(const char *program, const char *const *files, size_t nfiles);
#endif
