/*
 * Plug-in classes, run in sandboxed osd-plughost processes (design:
 * Plug-in isolation; decided 2026-11-18: never in-process).
 *
 * For each plug-in package found (any .so file in <config>/plugins, in
 * /usr/lib/objectspace/plugins, or in the folders of $OSD_PLUGIN_DIRS), osdd starts a host and registers a
 * proxy for each class it reports. Inside osdd a proxy behaves like its
 * parent class; methods the plug-in introduces or overrides are run in the
 * host when called through the service API, asynchronously and with a
 * deadline. A call that misses its deadline gets the parent's behaviour;
 * repeated misses or a crash restart the host. Class replacements the
 * plug-in asks for are applied in osdd too.
 */
#ifndef OSD_PLUGINS_H
#define OSD_PLUGINS_H

#include <stdbool.h>
#include <sys/types.h>

#include "cbor.h"
#include "env.h"
#include "osdo.h"
#include "osdo_loop.h"

void osd_plugins_init(osdo_loop *loop, const osd_env *env);
void osd_plugins_shutdown(void);

/* Whether `pid` is a plug-in host this service started (and reaps). */
bool osd_plugins_is_host(pid_t pid);

/* Is `method` on `o` implemented by a plug-in (so it must go remote)? */
bool osd_plugin_routes(osdo_object *o, const char *method);

/* Call it in the plug-in's host. `done` runs on the loop thread with the
 * result, or with NULL if the deadline passed or the host failed (the
 * caller then uses the parent's behaviour). Changes the method made to the
 * object's settings are applied before `done`. */
typedef void (*osd_remote_fn)(void *arg, const cv *result);
void osd_plugin_call(osdo_object *o, const char *path, const char *method, const cv *args,
                     int deadline_ms, osd_remote_fn done, void *arg);

/* After osdo_shutdown: free the proxy class definitions. */
void osd_plugins_free_defs(void);

/* Status for the stats request. */
void osd_plugins_status(cbuf *out);

#endif
