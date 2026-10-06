/*
 * vlhe_restart.c - stop and start ONE component, without disturbing
 * the others.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * WHY ITS OWN FILE, AND NOT vlhe_backend.c OR vlhe_cli.c.
 *
 * NOT THE BACKEND, because every host test links that file and this
 * code needs `vlhe_apply.c' - which runs insmod and starts daemons.
 * Dragging that into test binaries that today cannot do either is
 * the wrong direction on a workstation where CLAUDE.md forbids
 * running any load path at all. The Makefile comment at
 * VLHE_CLI_SRCS says the same thing from the other side:
 * "vlhe_apply.c IS CLI-ONLY, not in BACKEND_SRCS".
 *
 * NOT vlhe_cli.c, because the FAKE GUI links that (for
 * `vlhe_cli_is_subcommand', so vlhe.gtk can act as the CLI) and the
 * fake backend has its own scripted `vlhe_restart' that must win.
 * Two definitions in one link is what putting it there produced.
 *
 * SO: linked by vlhe.gtk.295, vlhe.gtk.su.295 and vlhe.295, and
 * NOT by vlhe.gtk.fake.295 or any host test.
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <string.h>

#include "vlhe_backend.h"
#include "vlhe_apply.h"

/*
 * WHICH SCOPE STARTS THIS DAEMON, and -1 for a name we do not run.
 *
 * ONE TABLE, USED BY BOTH FUNCTIONS BELOW, so "what restarts vmidid"
 * and "what pages does vmidid care about" cannot drift apart.
 */
static int
daemon_scope(const char *name)
{
    if (name == NULL)
        return -1;
    if (strcmp(name, "vmidid") == 0)
        return VLHE_ENABLE_MIDI;
    if (strcmp(name, "vdiscd") == 0)
        return VLHE_ENABLE_CD;
    if (strcmp(name, "vsoundd") == 0)
        return VLHE_ENABLE_SOUND;
    return -1;
}

int
vlhe_restart_wants_page(const char *name, const char *page)
{
    int scope = daemon_scope(name);

    if (scope < 0 || page == NULL)
        return 0;

    /*
     * SOUND SETTINGS FEEDS ALL THREE - `out_token()' in vlhe_apply.c
     * picks `@VSOUND@' or `@VSOUND:@CARD@@' from whether the Sound
     * module is in the plan, and that is the `-o' every daemon is
     * started with. A pending change there alters how the synth is
     * started even though the user was on a different page.
     */
    if (strcmp(page, "Sound Settings") == 0)
        return 1;

    if (scope == VLHE_ENABLE_MIDI)
        return strcmp(page, "Midi Settings") == 0;
    if (scope == VLHE_ENABLE_CD)
        return strcmp(page, "CD Settings") == 0;

    /* vsoundd: Sound Settings only, and that is handled above. */
    return 0;
}

int
vlhe_restart(const char *name)
{
    struct vlhe_plan p;
    int scope = daemon_scope(name);
    int rc;

    /*
     * THE GUI DOES THIS ITSELF, AND THAT REVERSES WHAT THIS COMMENT
     * USED TO SAY - 2026-09-27.
     *
     * It read: "THE INIT SCRIPT IS THE MECHANISM ... a GUI that
     * killed and respawned would be a second path to the same thing",
     * and returned -1 because the script was unwritten. The script
     * exists now, and it still cannot serve this button for two
     * reasons:
     *
     *   1. ITS `restart' IS THE WHOLE STACK - `$0 stop; sleep 1;
     *      $0 start'. That tears down vdiscd and drops the disc the
     *      user has loaded, which is the exact thing they asked to
     *      avoid: *"I can start vmidid if I forgot to pick a sound
     *      font without pulling everything down"*.
     *   2. PORTABLE MODE HAS NO INIT SCRIPT AT ALL. A tarball on a
     *      CF card has no /etc/init.d/vlhe, and the user's
     *      requirement is that this works there too.
     *
     * So the two are not competing paths to one thing: the script is
     * the mechanism AT BOOT, this is the mechanism for ONE daemon at
     * runtime, and neither can do the other's job.
     *
     * AND IT IS NOT A SECOND COPY OF THE LOGIC. The plan is built by
     * vlhe_plan_build_forced() - the same function the per-row Load
     * buttons use - so which binary, which flags and which order all
     * still come from vlhe_apply.c reading the config. Only the stop
     * is ours.
     */
    if (scope < 0)
        return -1;

    /*
     * DOWN FIRST, THEN UP - TWO PLANS, AND A LOAD PLAN ALONE WOULD
     * NOT HAVE DONE IT.
     *
     * Checked rather than assumed, 2026-09-27: an already-running
     * daemon is `optional' to the runner and is SKIPPED with
     * "already there" (vlhe_apply.c:5052), on the correct reasoning
     * that the machine is closer to the wanted state. So building
     * only the load plan would have reported success and restarted
     * nothing - the shape of failure this evening has produced
     * twice already.
     */
    /*
     * AND IT TAKES THE SCOPE'S MODULE WITH IT. A MIDI-scoped unload
     * is "stop vmidid" AND "rmmod vmidi" (vlhe_apply.c:1393), which
     * the forced load below puts back. That is heavier than the
     * words "restart the daemon" suggest, and it is deliberate:
     * it is exactly the sequence the per-row Load button runs, so
     * there is one code path rather than a lighter special case
     * here that could diverge from it.
     *
     * WHAT IT DOES NOT TOUCH IS THE POINT. The scope is one
     * component, so restarting the synth leaves vdisc loaded and
     * the user's disc attached - the whole reason for the button.
     */
    /*
     * `<= 0', NOT `!= 0' - THE BUILDERS RETURN A STEP COUNT.
     *
     * This read `!= 0' until 2026-09-27 and therefore failed on
     * EVERY press: vlhe_plan_build_*() returns out->n, the number of
     * steps, so any real plan "failed" and the button reported
     * "Could not restart vmidid" without running anything. -1 is the
     * refusal (a scope naming nothing) and 0 means an empty plan,
     * which is equally nothing to do.
     *
     * vlhe_plan_run() IS the other way round - 0 is success and a
     * non-zero is the 1-based step that stopped it - which is why
     * the two are tested differently two lines apart.
     */
    if (vlhe_plan_build_scoped(&p, 1, scope) <= 0)
        return -1;

    /*
     * THE TEARDOWN'S RESULT IS NOT CHECKED, DELIBERATELY.
     *
     * "Nothing is required on the unload side" (vlhe_apply.h) - a
     * daemon that is already stopped and a module that is already
     * gone are both the wanted state, and the whole POINT of this
     * button is the case where the daemon is NOT running: the user
     * forgot a soundfont, vmidid refused to start, and they want it
     * started now. Failing here because there was nothing to stop
     * would refuse exactly the press the button exists for - which
     * is what the Status page showed on 2026-09-27, `vmidid not
     * running' beside a Restart that would not.
     *
     * A teardown that genuinely fails shows up as the LOAD failing
     * below, which is the result the user cares about.
     */
    (void) vlhe_plan_run(&p, NULL);

    /*
     * FORCED, like the Load buttons: pressing a button that names a
     * daemon IS the request to start it, so a clear Include-in-load
     * box must not refuse it. It does not write the setting.
     */
    /* `1', NOT `scope', for the FORCED flag - it is a boolean, and
     * passing a bitmask happened to work only because every scope
     * constant is non-zero. */
    if (vlhe_plan_build_forced(&p, 0, scope, 1) <= 0)
        return -1;

    rc = vlhe_plan_run(&p, NULL);
    return rc == 0 ? 0 : -1;
}

int
vlhe_restart_synth(void)
{
    /*
     * THE SAME THING BY A SHORTER NAME - 2026-09-27. This was a stub
     * for the same reason vlhe_restart() was, and it is discharged
     * the same way rather than separately: design/09 asks for it
     * specifically, because a wedged vmidi keeps the MIDI slot and
     * unloading the module takes the whole audio path with it.
     */
    return vlhe_restart("vmidid");
}
