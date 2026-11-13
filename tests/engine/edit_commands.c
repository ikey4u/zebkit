#include "harness.h"
#include <zebkit/offscreen.h>
#include <stdio.h>
#include <string.h>

static zebkit_view *view;
static int stage, retries, initialized;
static void poll(void *data);
static void send_commands(void) {
    zebkit_key_event key = {0};
    key.type = ZEBKIT_KEY_DOWN;
    if (stage == 0) {
        key.key_code = 8;
        const zebkit_edit_command command = {"DeleteWordBackward", NULL};
        check(zebkit_view_send_key_with_commands(view, &key, &command, 1), "word-delete key accepted", NULL);
    } else if (stage == 1) {
        const zebkit_edit_command commands[] = {{"SelectAll", NULL}, {"InsertText", "Ω"}};
        check(zebkit_view_send_key_with_commands(view, &key, commands, 2), "batched select and Unicode insertion accepted", NULL);
    } else if (stage == 2) {
        key.key_code = 8;
        const zebkit_edit_command command = {"DeleteBackward", NULL};
        check(zebkit_view_send_key_with_commands(view, &key, &command, 1), "cancelled editing key accepted", NULL);
    } else {
        const zebkit_edit_command command = {"ZebkitUnknownEditingCommand", NULL};
        check(zebkit_view_send_key_with_commands(view, &key, &command, 1), "unknown command safely accepted", NULL);
    }
    retries = 0;
    zebkit_post_delayed_task(poll, NULL, 20);
}
static void unblocked(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v; (void)json; (void)data;
    check(!error, "install key cancellation", NULL);
    send_commands();
}
static void answer(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v; (void)data;
    const char *expected[] = {"[\"alpha \",1]", "[\"Ω\",2]", "[\"Ω\",3]", "[\"Ω\",4]"};
    if ((error || !json || strcmp(json, expected[stage])) && ++retries < 50) {
        zebkit_post_delayed_task(poll, NULL, 20);
        return;
    }
    const char *names[] = {"native word deletion executes once with one keydown",
        "two commands share one keydown and preserve Unicode", "preventDefault blocks native editing",
        "unknown editing command leaves the document unchanged"};
    check(!error && json && !strcmp(json, expected[stage]), names[stage], NULL);
    if (error || !json || strcmp(json, expected[stage])) fprintf(stderr, "received: %s (%s)\n", json ? json : "null", error ? error : "");
    if (++stage == 4) { zebkit_quit(); return; }
    if (stage == 2) zebkit_view_evaluate(view, "window.blockKeys=true", unblocked, NULL);
    else send_commands();
}
static void poll(void *data) {
    (void)data;
    zebkit_view_evaluate(view, "[document.querySelector('textarea').value,window.keys]", answer, NULL);
}
static void prepared(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v; (void)json; (void)data;
    check(!error, "focus the editable element", NULL);
    if (error) fprintf(stderr, "focus error: %s\n", error);
    zebkit_key_event key = {0};
    key.type = ZEBKIT_KEY_DOWN;
    const zebkit_edit_command valid = {"SelectAll", NULL}, invalid = {NULL, NULL};
    check(!zebkit_view_send_key_with_commands(NULL, &key, &valid, 1), "reject NULL view", NULL);
    check(!zebkit_view_send_key_with_commands(view, NULL, &valid, 1), "reject NULL event", NULL);
    check(!zebkit_view_send_key_with_commands(view, &key, NULL, 1), "reject missing command array", NULL);
    check(!zebkit_view_send_key_with_commands(view, &key, &invalid, 1), "reject missing command name", NULL);
    check(!zebkit_view_send_key_with_commands(view, &key, &valid, 129), "reject excessive command count before reading array", NULL);
    key.type = ZEBKIT_KEY_UP;
    check(!zebkit_view_send_key_with_commands(view, &key, &valid, 1), "reject commands on keyup", NULL);
    check(zebkit_view_send_key_with_commands(view, &key, NULL, 0), "ordinary keyup accepted", NULL);
    send_commands();
}
static void loaded(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)url; (void)data;
    if (event != ZEBKIT_LOAD_FINISHED || initialized || !url || strncmp(url, "data:", 5)) return;
    initialized = 1;
    zebkit_view_evaluate(v, "(()=>{window.keys=0;window.blockKeys=false;document.addEventListener('keydown',e=>{keys++;if(blockKeys)e.preventDefault()});const t=document.querySelector('textarea');t.focus();t.setSelectionRange(t.value.length,t.value.length);return true})()", prepared, NULL);
}
static void timeout(void *data) { (void)data; check(0, "editing commands completed before deadline", NULL); zebkit_quit(); }
static void ready(void *data) {
    (void)data;
    view = zebkit_view_new_offscreen(NULL, 500, 300, 1);
    zebkit_view_set_focus(view, true);
    zebkit_view_on_load(view, loaded, NULL);
    zebkit_view_load_url(view, "data:text/html,<textarea>alpha beta</textarea>");
    zebkit_post_delayed_task(timeout, NULL, 8000);
}
int main(int argc, char **argv) {
    int code = zebkit_main(argc, argv, ready, NULL);
    check(code == 0 && stage == 4, "every stage ran, and zebkit_main returned 0", "stage %d, code %d", stage, code);
    return summary();
}
