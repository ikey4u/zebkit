/*
 * A plugin of <zebkit/pdf.h>'s hooks: a kind of annotation of its own (a
 * "sticker", with a mood, drawn by itself), an undo history kept from the
 * operations hooks are told of, and text for pages without any. The same is in
 * plugin.mjs for zebkit.pdf.use(), and scripts/engine-test.sh checks both.
 */

#ifndef ZEBKIT_EXAMPLE_PDF_PLUGIN_H
#define ZEBKIT_EXAMPLE_PDF_PLUGIN_H

#include <zebkit/pdf.h>

#define PLUGIN_STICKER "Sample_Sticker"

typedef struct plugin plugin;

plugin *plugin_install(zebkit_pdf *pdf);
void plugin_remove(plugin *self);

/* How many operations can be undone, and undoing and redoing the last. */
int plugin_history(const plugin *self);
bool plugin_undo(plugin *self, zebkit_pdf *pdf);
bool plugin_redo(plugin *self, zebkit_pdf *pdf);

#endif /* ZEBKIT_EXAMPLE_PDF_PLUGIN_H */
