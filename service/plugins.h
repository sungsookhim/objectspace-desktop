/*
 * Plug-in classes, run in sandboxed wps-plughost processes (design:
 * Plug-in isolation; decided 2026-11-18: never in-process).
 *
 * For each plug-in package found (any .so file in <config>/plugins, in
 * /usr/lib/wps/plugins, or in the folders of $WPS_PLUGIN_DIRS), wpsd starts a host and registers a
 * proxy for each class it reports. Inside wpsd a proxy behaves like its
 * parent class; methods the plug-in introduces or overrides are run in the
 * host when called through the service API, asynchronously and with a
 * deadline. A call that misses its deadline gets the parent's behaviour;
 * repeated misses or a crash restart the host. Class replacements the
 * plug-in asks for are applied in wpsd too.
 */
#ifndef WPS_PLUGINS_H
#define WPS_PLUGINS_H

#include <stdbool.h>

#include "cbor.h"
#include "env.h"
#include "wpo.h"
#include "wpo_loop.h"

void wps_plugins_init(wpo_loop *loop, const wps_env *env);
void wps_plugins_shutdown(void);

/* Is `method` on `o` implemented by a plug-in (so it must go remote)? */
bool wps_plugin_routes(wpo_object *o, const char *method);

/* Call it in the plug-in's host. `done` runs on the loop thread with the
 * result, or with NULL if the deadline passed or the host failed (the
 * caller then uses the parent's behaviour). Changes the method made to the
 * object's settings are applied before `done`. */
typedef void (*wps_remote_fn)(void *arg, const cv *result);
void wps_plugin_call(wpo_object *o, const char *path, const char *method, const cv *args,
                     int deadline_ms, wps_remote_fn done, void *arg);

/* After wpo_shutdown: free the proxy class definitions. */
void wps_plugins_free_defs(void);

/* Status for the stats request. */
void wps_plugins_status(cbuf *out);

#endif
