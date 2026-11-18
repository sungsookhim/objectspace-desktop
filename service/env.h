/*
 * The service's environment: directories, the user's WPS identity and the
 * host ID.
 *
 * Directories follow the XDG conventions and can be overridden for tests:
 *   WPS_CONFIG_DIR   default $XDG_CONFIG_HOME/wps   (~/.config/wps)
 *   WPS_STATE_DIR    default $XDG_DATA_HOME/wps     (~/.local/share/wps)
 *   WPS_RUNTIME_DIR  default $XDG_RUNTIME_DIR       (the socket lives here)
 *   WPS_HOST_ID      default /etc/machine-id
 *
 * The identity (design: State that travels with a volume) is a random ID in
 * <config>/identity, created on first use. Copying that file to another
 * computer makes the user the same WPS user there.
 */
#ifndef WPS_ENV_H
#define WPS_ENV_H

#include <limits.h>

typedef struct {
    char identity[40];
    char host[64];
    char writer[120];            /* "<identity>/<host>": names a row's author */
    char config_dir[PATH_MAX];
    char state_dir[PATH_MAX];
    char runtime_dir[PATH_MAX];
} wps_env;

/* 0 on success; on failure, -1 with a message in err. */
int wps_env_load(wps_env *e, char *err, size_t errlen);

/* mkdir -p with the given mode for the last component. */
int wps_mkdirs(const char *path, unsigned mode);

#endif
