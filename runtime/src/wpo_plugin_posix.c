/*
 * Plug-in loading for POSIX systems (dlopen). A plug-in exports
 *     wpo_status wpo_plugin_init(uint32_t abi_version);
 * Plug-ins are never unloaded: their class and method definitions stay
 * referenced by the registry.
 */
#include "wpo.h"

#include <dlfcn.h>
#include <stdio.h>

wpo_status wpo_load_plugin(const char *path, char *err, size_t errlen)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        if (err) snprintf(err, errlen, "%s", dlerror());
        return WPO_EPLUGIN;
    }
    wpo_plugin_init_fn init;
    *(void **)&init = dlsym(h, "wpo_plugin_init");
    if (!init) {
        if (err) snprintf(err, errlen, "%s: no wpo_plugin_init", path);
        return WPO_EPLUGIN;
    }
    wpo_status st = init(WPO_ABI_VERSION);
    if (st != WPO_OK && err) snprintf(err, errlen, "%s: %s", path, wpo_strerror(st));
    return st;
}
