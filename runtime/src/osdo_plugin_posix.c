/*
 * Plug-in loading for POSIX systems (dlopen). A plug-in exports
 *     osdo_status osdo_plugin_init(uint32_t abi_version);
 * Plug-ins are never unloaded: their class and method definitions stay
 * referenced by the registry.
 */
#include "osdo.h"

#include <dlfcn.h>
#include <stdio.h>

osdo_status osdo_load_plugin(const char *path, char *err, size_t errlen)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        if (err) snprintf(err, errlen, "%s", dlerror());
        return OSDO_EPLUGIN;
    }
    osdo_plugin_init_fn init;
    *(void **)&init = dlsym(h, "osdo_plugin_init");
    if (!init) {
        if (err) snprintf(err, errlen, "%s: no osdo_plugin_init", path);
        return OSDO_EPLUGIN;
    }
    osdo_status st = init(OSDO_ABI_VERSION);
    if (st != OSDO_OK && err) snprintf(err, errlen, "%s: %s", path, osdo_strerror(st));
    return st;
}
