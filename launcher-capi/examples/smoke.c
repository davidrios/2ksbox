/* A third front end, in the smallest possible form: a C program that
 * drives the same models the egui and Qt builds do, through
 * `include/launcher_core.h`.
 *
 * It is a *test*, not a demo — `scripts/test.sh host` builds and runs it
 * against a scratch library — and what it tests is that the C ABI is
 * still whole and still means the same thing as the Rust one. It creates
 * a DOS machine through the wizard and checks the answers the shared
 * form gives: 64 MB, a period processor, emulated because a throttle
 * needs TCG, no network card and no USB tablet. Then a disc onto the
 * shelf, the machine seen from the library, and the shelf read back.
 *
 * Everything it prints is checked by the caller, so a change that
 * silently alters one of those defaults fails here as well as in the two
 * GUIs.
 *
 * Build:
 *   cargo build -p launcher-capi
 *   cc -I launcher-capi/include launcher-capi/examples/smoke.c \
 *      target/debug/liblauncher_capi.a -lm -ldl -lpthread -o smoke
 */
#include "launcher_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(const char *what, int ok, const char *saw) {
    printf("  %s %-52s %s\n", ok ? "PASS" : "FAIL", what, saw ? saw : "");
    if (!ok) failures++;
}

/* Every getter hands over a string the caller owns; this checks one and
 * frees it in the same breath, which is also the usage pattern a real
 * front end wants. */
static void check_str(const char *what, char *got, const char *want) {
    check(what, got && strcmp(got, want) == 0, got);
    lc_string_free(got);
}

/* The index of an adapter in *this machine's* list, by a fragment of its
 * label, or -1. The list is per family, so the same adapter is at a
 * different index on Win98 than on XP and a front end never remembers
 * one across a family switch. */
static long video_index(LcWizard *w, const char *want) {
    for (size_t i = 0; i < lc_wizard_video_count(w); i++) {
        char *label = lc_wizard_video_label(w, i);
        int hit = label && strstr(label, want) != NULL;
        lc_string_free(label);
        if (hit) return (long)i;
    }
    return -1;
}

/* The label of the adapter currently selected. */
static char *video_label(LcWizard *w) {
    return lc_wizard_video_label(w, lc_wizard_video(w));
}

/* The index of a label in one of the wizard's pickers, or -1. A front
 * end fills a combo box this way rather than hard-coding the strings. */
static long label_index(uint32_t kind, const char *want) {
    for (size_t i = 0;; i++) {
        char *label = lc_wizard_label(kind, i);
        if (!label) return -1;
        int hit = strcmp(label, want) == 0;
        lc_string_free(label);
        if (hit) return (long)i;
    }
}

/* Which row a disc is on, by path. The shelf is ordered by label, so a
 * disc that was just added is not necessarily the last row — and a
 * renamed one moves. Every front end looks a row up like this rather
 * than remembering an index across an edit. */
static size_t shelf_row(LcShelf *s, const char *path) {
    for (size_t i = 0; i < lc_shelf_count(s); i++) {
        char *p = lc_shelf_path(s, i);
        int hit = p && strcmp(p, path) == 0;
        lc_string_free(p);
        if (hit) return i;
    }
    return (size_t)-1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <library dir> <disc image>\n", argv[0]);
        return 2;
    }
    const char *library_dir = argv[1];
    const char *disc = argv[2];

    printf("== the wizard, through C\n");
    LcWizard *w = lc_wizard_new();

    long dos = label_index(LC_LABEL_FAMILY, "DOS");
    check("the family picker offers DOS", dos >= 0, NULL);
    lc_wizard_open_new(w, (size_t)dos);
    check("the form is open", lc_wizard_is_open(w), NULL);
    check_str("as \"New machine\"", lc_wizard_title(w), "New machine");

    /* Switching family on a *new* machine moves every field nobody has
     * touched to the new family's own default. That is what picking "XP"
     * after "Win98" means, and it was broken for the four fields whose
     * list is per family — the adapter, the card, the MIDI port and the
     * pad — until 2026-09-09: they only moved when the new family did
     * not offer what was in the field at all, so a machine switched from
     * 98 to XP sat on the Cirrus, which is XP's *non*-default.
     *
     * A field somebody has picked is the other half of the rule and must
     * survive the switch, so both directions are checked here. */
    long win98 = label_index(LC_LABEL_FAMILY, "Win98");
    long xp = label_index(LC_LABEL_FAMILY, "XP");
    check("the family picker offers Win98 and XP", win98 >= 0 && xp >= 0, NULL);
    lc_wizard_open_new(w, (size_t)win98);
    char *adapter = video_label(w);
    check("a new Win98 machine starts on the Cirrus",
          adapter && strstr(adapter, "Cirrus") != NULL, adapter);
    lc_string_free(adapter);
    lc_wizard_choose_family(w, (size_t)xp);
    adapter = video_label(w);
    check("switching it to XP moves the untouched adapter to XP's own",
          adapter && strstr(adapter, "d3dpt-vga") != NULL, adapter);
    lc_string_free(adapter);
    check("...and it counts as the default there", lc_wizard_video_is_default(w), NULL);
    check("the card followed too", lc_wizard_sound_is_default(w), NULL);
    check("and the MIDI port", lc_wizard_music_is_default(w), NULL);
    /* The pad follows the same rule in the form, but this API has no pad
     * row yet (M13 is newer than the C ABI), so it is unchecked here. */
    /* Now pick one by hand: it is a decision, and the next family switch
     * must not throw it away. */
    long cirrus = video_index(w, "Cirrus");
    check("XP offers the Cirrus as well", cirrus >= 0, NULL);
    lc_wizard_set_video(w, (size_t)cirrus);
    lc_wizard_choose_family(w, (size_t)win98);
    adapter = video_label(w);
    check("a chosen adapter survives the switch back",
          adapter && strstr(adapter, "Cirrus") != NULL, adapter);
    lc_string_free(adapter);
    /* Unless the new family has no such adapter: DOS is offered neither
     * of ours, so a d3dpt picked on XP cannot come along. */
    lc_wizard_choose_family(w, (size_t)xp);
    lc_wizard_set_video(w, (size_t)video_index(w, "d3dpt-vga"));
    lc_wizard_choose_family(w, (size_t)dos);
    adapter = video_label(w);
    check("but one the new family doesn't offer falls back to its default",
          adapter && strstr(adapter, "Standard VGA") != NULL, adapter);
    lc_string_free(adapter);
    check("...which is what DOS starts on", lc_wizard_video_is_default(w), NULL);
    /* And "Default" puts the field back to *following* the family, so a
     * later switch moves it again rather than pinning what it reset to. */
    lc_wizard_choose_family(w, (size_t)win98);
    lc_wizard_set_video(w, (size_t)video_index(w, "d3dpt-vga"));
    lc_wizard_reset_video(w);
    lc_wizard_choose_family(w, (size_t)xp);
    adapter = video_label(w);
    check("\"Default\" makes the adapter follow the family again",
          adapter && strstr(adapter, "d3dpt-vga") != NULL, adapter);
    lc_string_free(adapter);

    lc_wizard_open_new(w, (size_t)dos);

    /* The whole point of a shared form: these are the same answers the
     * two GUIs get, because it is the same code. */
    check("DOS opens on 64 MB", lc_wizard_ram_mb(w) == 64, NULL);
    check("...which is the family's default", lc_wizard_ram_is_default(w), NULL);

    char *cpu = lc_wizard_label(LC_LABEL_CPU_SPEED, lc_wizard_cpu_speed(w));
    check("a period processor, not full speed", cpu && strncmp(cpu, "486DX2-66", 9) == 0, cpu);
    lc_string_free(cpu);

    char *accel = lc_wizard_label(LC_LABEL_ACCEL, lc_wizard_accel(w));
    check("emulated, because a throttle needs TCG", accel && strcmp(accel, "Emulation") == 0, accel);
    lc_string_free(accel);

    /* A DOS machine has no Direct3D to place, so the 3D line is absent
     * whatever this host's GPU is (ADR-013). */
    bool gfx_warning = true;
    char *gfx = lc_wizard_graphics_note(w, &gfx_warning);
    check("a DOS machine says nothing about 3D", gfx == NULL && !gfx_warning, gfx);
    lc_string_free(gfx);

    check("no network card", !lc_wizard_network(w), NULL);
    char *note = lc_wizard_network_note(w);
    check("...and it says so", note && strstr(note, "No network adapter") != NULL, note);
    lc_string_free(note);

    /* And no USB tablet: a DOS mouse driver reads the PS/2 controller,
     * so an absolute device would leave the guest with no pointer. The
     * checkbox is the same one the two GUIs draw. */
    check("no USB tablet either", !lc_wizard_seamless_mouse(w), NULL);
    char *pointer = lc_wizard_seamless_mouse_note(w);
    check("...and it names the hotkey", pointer && strstr(pointer, "Ctrl+Alt+G") != NULL, pointer);
    lc_string_free(pointer);
    lc_wizard_choose_seamless_mouse(w, true);
    check("turning it on takes", lc_wizard_seamless_mouse(w), NULL);
    pointer = lc_wizard_seamless_mouse_note(w);
    check("...and DOS says the tablet leaves it blind",
          pointer && strstr(pointer, "no pointer at all") != NULL, pointer);
    lc_string_free(pointer);
    lc_wizard_choose_seamless_mouse(w, false);

    /* The Voodoo 2 (doc 21): off unless picked, and the sentence under
     * the checkbox says what a Glide game does either way. */
    check("no Voodoo 2 unless picked", !lc_wizard_voodoo2(w), NULL);
    char *voodoo = lc_wizard_voodoo2_note(w);
    check("...and off says Glide still has the pass-through",
          voodoo && strstr(voodoo, "pass-through") != NULL, voodoo);
    lc_string_free(voodoo);
    lc_wizard_choose_voodoo2(w, true);
    check("picking the Voodoo 2 takes", lc_wizard_voodoo2(w), NULL);
    voodoo = lc_wizard_voodoo2_note(w);
    check("...and on names the driver the guest needs",
          voodoo && strstr(voodoo, "3dfx's own Voodoo2 driver") != NULL, voodoo);
    lc_string_free(voodoo);
    lc_wizard_choose_voodoo2(w, false);

    /* The sound card and the MIDI port (doc 20 §6). A DOS machine starts
     * on the Sound Blaster — the card its games know how to find — and
     * on a General MIDI port, because a DOS machine has no synthesizer
     * of its own otherwise. The FM chip is in neither list: it comes
     * with the card that carried one, which is what the note says. */
    char *card = lc_wizard_sound_label(w, lc_wizard_sound(w));
    check("a DOS machine starts on the Sound Blaster",
          card && strstr(card, "Sound Blaster 16") != NULL, card);
    lc_string_free(card);
    check("...which is the family's own default", lc_wizard_sound_is_default(w), NULL);
    char *card_note = lc_wizard_sound_note(w);
    check("...and its note names the AUTOEXEC line",
          card_note && strstr(card_note, "BLASTER=A220") != NULL, card_note);
    lc_string_free(card_note);
    char *port = lc_wizard_music_label(w, lc_wizard_music(w));
    check("and on a General MIDI port", port && strstr(port, "General MIDI") != NULL, port);
    lc_string_free(port);
    check("the bank field is offered with it", lc_wizard_soundfont_applies(w), NULL);
    check("...and the ROM one is not", !lc_wizard_mt32_roms_applies(w), NULL);
    /* The Ensoniq is not on offer here — it is the `Other` family's card
     * — so asking for it must leave the machine as it was. */
    size_t cards = lc_wizard_sound_count(w);
    lc_wizard_set_sound(w, cards + 4);
    check("a card past the end of the list is ignored", lc_wizard_sound_is_default(w), NULL);
    /* An MT-32 asks for ROMs this program will never ship, so picking it
     * turns the ROM field on — and saving without one is refused, which
     * `lc_wizard_submit` reports below in the machine this builds. */
    for (size_t i = 0; i < lc_wizard_music_count(w); i++) {
        char *label = lc_wizard_music_label(w, i);
        if (label && strstr(label, "MT-32") != NULL) {
            lc_wizard_set_music(w, i);
        }
        lc_string_free(label);
    }
    check("picking the MT-32 asks for its ROMs", lc_wizard_mt32_roms_applies(w), NULL);
    check("...and stops asking for a bank", !lc_wizard_soundfont_applies(w), NULL);
    lc_wizard_reset_music(w);
    check("\"Default\" puts the port back", lc_wizard_music_is_default(w), NULL);

    /* The memory range is per family and a value outside it is clamped
     * here rather than refused at save time. */
    uint32_t min = 0, max = 0;
    lc_wizard_ram_range(w, &min, &max);
    lc_wizard_choose_ram_mb(w, max + 4096);
    check("memory is clamped to the family's ceiling", lc_wizard_ram_mb(w) == max, NULL);
    lc_wizard_reset_ram(w);
    check("...and \"Default\" puts it back", lc_wizard_ram_is_default(w), NULL);

    /* Our own emulator fast paths: everything on except the one that
     * ships off, a checkbox that changes the count, and "All defaults"
     * that puts it back — the section of the form a third front end has
     * to be able to draw as well as the two Rust ones. */
    size_t opt_count = 0;
    for (char *l; (l = lc_wizard_label(LC_LABEL_OPTIMIZATION, opt_count)); opt_count++) {
        lc_string_free(l);
    }
    check("the form offers some optimizations", opt_count > 0, NULL);
    check("...all at their shipped setting", lc_wizard_optimizations_are_default(w), NULL);
    char *opt_note = lc_wizard_label(LC_LABEL_OPTIMIZATION_NOTE, 0);
    check("...each with a sentence under it", opt_note && strlen(opt_note) > 0, opt_note);
    lc_string_free(opt_note);
    char *before_summary = lc_wizard_optimizations_summary(w);
    check("...and a count of what is on", before_summary && strchr(before_summary, ' ') != NULL, before_summary);
    check("the first one is on", lc_wizard_optimization_enabled(w, 0), NULL);
    lc_wizard_choose_optimization(w, 0, false);
    check("...turning it off takes", !lc_wizard_optimization_enabled(w, 0), NULL);
    check("...and is not the default any more", !lc_wizard_optimizations_are_default(w), NULL);
    char *after_summary = lc_wizard_optimizations_summary(w);
    check("...which the count says", after_summary && strcmp(after_summary, before_summary) != 0, after_summary);
    lc_string_free(before_summary);
    lc_string_free(after_summary);
    lc_wizard_reset_optimizations(w);
    check("\"All defaults\" puts every one back", lc_wizard_optimizations_are_default(w), NULL);
    /* The two shortcuts. "All off" is the control run -- every one of our
     * additions out of the guest's path in one click, which is what
     * answers "is one of ours what broke this" -- and it must really be
     * every one, so this asks each switch rather than trusting the flag.
     * "All on" is not the same as the defaults: pinned-regs ships off. */
    lc_wizard_disable_all_optimizations(w);
    check("\"Turn all off\" says so", lc_wizard_optimizations_all_off(w), NULL);
    int still_on = 0;
    for (size_t i = 0; i < opt_count; i++) {
        if (lc_wizard_optimization_enabled(w, i)) still_on++;
    }
    check("...and every switch really is off", still_on == 0, NULL);
    check("...which is not the shipped setting", !lc_wizard_optimizations_are_default(w), NULL);
    lc_wizard_enable_all_optimizations(w);
    check("\"Turn all on\" says so", lc_wizard_optimizations_all_on(w), NULL);
    int still_off = 0;
    for (size_t i = 0; i < opt_count; i++) {
        if (!lc_wizard_optimization_enabled(w, i)) still_off++;
    }
    check("...and every switch really is on", still_off == 0, NULL);
    check("...and that is not the defaults either (one ships off)",
          !lc_wizard_optimizations_are_default(w), NULL);
    lc_wizard_reset_optimizations(w);
    check("...and the defaults come back from there",
          lc_wizard_optimizations_are_default(w), NULL);

    lc_wizard_set(w, "name", "capi dos");
    lc_wizard_set_flag(w, "existing_disk", true);
    lc_wizard_set(w, "disk_path", "/dev/null");
    check("submit", lc_wizard_submit(w, library_dir), NULL);

    char *saved = lc_wizard_saved_path(w);
    check("...wrote a bundle", saved && strlen(saved) > 0, saved);
    char bundle[4096];
    snprintf(bundle, sizeof bundle, "%s", saved ? saved : "");
    lc_string_free(saved);
    lc_wizard_free(w);

    printf("== the library\n");
    LcMachines *m = lc_machines_new();
    lc_machines_refresh(m);
    size_t found = (size_t)-1;
    for (size_t i = 0; i < lc_machines_count(m); i++) {
        char *name = lc_machines_name(m, i);
        if (name && strcmp(name, "capi dos") == 0) found = i;
        lc_string_free(name);
    }
    check("the machine is in the library", found != (size_t)-1, NULL);
    if (found != (size_t)-1) {
        check_str("its family", lc_machines_family(m, found), "DOS");
        check_str("its shader", lc_machines_shader_label(m, found), "(default)");
        check("nothing is running", !lc_machines_is_running(m, found), NULL);
    }

    printf("== the disc shelf\n");
    char *shelf_path = lc_machines_disc_library_path(m);
    LcShelf *s = lc_shelf_new();
    lc_shelf_open_for(s, bundle, shelf_path);
    check("the shelf opened for a machine", lc_shelf_for_machine(s), NULL);
    check_str("with an empty tray", lc_shelf_boot_label(s), "(empty tray)");
    size_t before = lc_shelf_count(s);
    lc_shelf_add(s, disc);
    lc_shelf_flush(s);
    check("a disc went on", lc_shelf_count(s) == before + 1, NULL);
    check("...and the shelf was written", lc_shelf_take_saved(s), NULL);
    check("...only once", !lc_shelf_take_saved(s), NULL);
    size_t row = shelf_row(s, disc);
    check("the new disc is on a row", row != (size_t)-1, NULL);
    check("the new disc boots nothing yet", !lc_shelf_is_boot(s, row), NULL);
    check("make it the boot disc", lc_shelf_set_boot(s, row), NULL);
    check("...it is", lc_shelf_is_boot(s, row), NULL);
    lc_shelf_set_label(s, row, "a renamed disc");
    lc_shelf_flush(s);
    row = shelf_row(s, disc);
    check_str("a label is editable", lc_shelf_label(s, row), "a renamed disc");
    /* The shelf is ordered by label, not by the order discs were added:
     * one list, which every front end shows and which the in-guest
     * CDSHELF program reads by slot number off the flat file written
     * from it. Numbers in a label sort as numbers, because disc sets are
     * numbered and `disc 10` does not come between `disc 1` and `disc 2`
     * on anybody's shelf. (Paths that don't exist are still discs — the
     * shelf records what it was given.) */
    lc_shelf_add(s, "zulu.iso");
    lc_shelf_add(s, "disc 10.iso");
    lc_shelf_add(s, "disc 2.iso");
    lc_shelf_flush(s);
    check("four discs on the shelf", lc_shelf_count(s) == before + 4, NULL);
    check("...the numbered ones in number order",
          shelf_row(s, "disc 2.iso") < shelf_row(s, "disc 10.iso"), NULL);
    check("...and zulu last, wherever it was added",
          shelf_row(s, "zulu.iso") == lc_shelf_count(s) - 1, NULL);
    check("...with the renamed disc ahead of both, by its new name",
          shelf_row(s, disc) < shelf_row(s, "disc 2.iso"), NULL);
    lc_shelf_free(s);
    lc_string_free(shelf_path);

    printf("== snapshots on a machine that is not running\n");
    LcSnapshots *snaps = lc_snapshots_new();
    lc_snapshots_open_for(snaps, bundle, false);
    /* Not running, so this went at the disk with qemu-img rather than
     * through a monitor. An image with no snapshot table has none — that
     * is not an error, and a front end must not show one. */
    char *err = lc_snapshots_error(snaps);
    check("a machine with no snapshots is not an error", err && strlen(err) == 0, err);
    lc_string_free(err);
    check("...and lists nothing", lc_snapshots_count(snaps) == 0, NULL);
    check("...with no job in flight", !lc_snapshots_job_pending(snaps), NULL);
    /* A bundle that isn't there is: a failure has to arrive as a message,
     * which is the part worth checking across a C boundary. */
    lc_snapshots_open_for(snaps, "/nonexistent/machine.toml", false);
    err = lc_snapshots_error(snaps);
    check("a bundle that isn't there reports why", err && strlen(err) > 0, err);
    lc_string_free(err);
    lc_snapshots_free(snaps);

    printf("== the shader profile editor\n");
    LcEditor *e = lc_editor_new();
    lc_editor_new_profile(e);
    check("a new profile has no parameters yet", lc_editor_param_count(e) == 0, NULL);
    check("...and nothing to render", !lc_editor_renderable(e), NULL);
    check("...so nothing to redraw on a timer either", lc_editor_frame_interval_ms(e) == 0, NULL);
    check("saving it without a name is refused", !lc_editor_save(e, NULL), NULL);
    char *save_err = lc_editor_error(e);
    check("...and says so", save_err && strcmp(save_err, "a name is required") == 0, save_err);
    lc_string_free(save_err);
    lc_editor_free(e);

    lc_machines_free(m);
    printf("\ncapi smoke: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
