/* Ending and locking the session (session.h). */
#include "session.h"

#include <stdio.h>
#include <string.h>

#include "events.h"
#include "winlist.h"

/* OS/2 asked before shutting down; a session of today can also end with
 * the machine left on (log off) or restart it. */
static const char *const end_how[] = { "poweroff", "reboot", "logout" };
static char end_from[32];

static void end(int choice)
{
    if (choice < 0 || choice > 2) return;   /* Cancel */
    events_raise("session.end", end_how[choice], end_from, (ui_rect){ 0, 0, 0, 0 });
    if (!winlist_ext("shutdown", end_how[choice]))
        fprintf(stderr, "osd-shell: %s: ending the session needs the Objectspace session or the GNOME Shell extension\n", end_from);
}

static void end_chosen(void *ctx, int choice) { (void)ctx; end(choice); }
static void logoff_chosen(void *ctx, int choice) { (void)ctx; if (choice == 0) end(2); }

bool session_lock(const char *from)
{
    events_raise("session.lock", "", from, (ui_rect){ 0, 0, 0, 0 });
    if (winlist_ext("lock", NULL)) return true;
    fprintf(stderr, "osd-shell: %s: Lockup needs the Objectspace session or the GNOME Shell extension\n", from);
    return false;
}

void session_ask_end(ui_app *a, const char *from)
{
    static const char *const buttons[] = { "Shut down", "Restart", "Log off", "Cancel" };
    snprintf(end_from, sizeof end_from, "%s", from);
    ui_dialog_open(a, "Shut Down", "End this session: shut the computer down, restart it, or log off?", buttons, 4, 0,
                   end_chosen, NULL);
}

void session_ask_logoff(ui_app *a, const char *from)
{
    static const char *const buttons[] = { "Log off", "Cancel" };
    snprintf(end_from, sizeof end_from, "%s", from);
    ui_dialog_open(a, "Log Off", "End this session and log off?", buttons, 2, 0, logoff_chosen, NULL);
}
