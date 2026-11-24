/*
 * The service's environment: directories, the user's Objectspace identity and the
 * host ID.
 *
 * Directories follow the XDG conventions and can be overridden for tests:
 *   OSD_CONFIG_DIR   default $XDG_CONFIG_HOME/objectspace   (~/.config/objectspace)
 *   OSD_STATE_DIR    default $XDG_DATA_HOME/objectspace     (~/.local/share/objectspace)
 *   OSD_RUNTIME_DIR  default $XDG_RUNTIME_DIR       (the socket lives here)
 *   OSD_HOST_ID      default /etc/machine-id
 *
 * The identity (design: State that travels with a volume) is a random ID in
 * <config>/identity, created on first use. Copying that file to another
 * computer makes the user the same Objectspace user there.
 */
#ifndef OSD_ENV_H
#define OSD_ENV_H

#include <limits.h>
#include <stddef.h>

/* A volume ID: filesystem UUID, plus ":<root>" for a subvolume or bind mount. */
#define OSD_VOLID_MAX (64 + PATH_MAX)

typedef struct {
    char identity[40];
    char host[64];
    char writer[120];            /* "<identity>/<host>": names a row's author */
    char config_dir[PATH_MAX];
    char state_dir[PATH_MAX];
    char runtime_dir[PATH_MAX];
} osd_env;

/* 0 on success; on failure, -1 with a message in err. */
int osd_env_load(osd_env *e, char *err, size_t errlen);

/* mkdir -p with the given mode for the last component. */
int osd_mkdirs(const char *path, unsigned mode);

#endif
