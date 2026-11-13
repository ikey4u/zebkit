/* Annotations as a full editor has them (<zebkit/pdf.h>): every appearance
 * checked in pixels, every entry through a save, malformed input refused.
 * Takes a TrueType font's path (Ahem: every glyph a box of one em). */

#include "pdf_support.h"

#if defined(__linux__)
#include <sys/resource.h>
#endif

static size_t font_size;
static char *font_bytes;

static int add(zebkit_pdf *pdf, int page, const char *json) {
    char *error = NULL;
    const int index = zebkit_pdf_annotation_add(pdf, page, json, &error);
    if (index < 0) {
        check_text(0, json, error);
        zebkit_free(error);
    }
    return index;
}

static char *listing(zebkit_pdf *pdf, int page) {
    return zebkit_pdf_annotations(pdf, page, NULL);
}

static int listed(zebkit_pdf *pdf, int page, const char *what) {
    char *json = listing(pdf, page);
    const int found = json && strstr(json, what);
    zebkit_free(json);
    return found;
}

static int count_annotations(zebkit_pdf *pdf, int page) {
    char *json = listing(pdf, page);
    int count = 0;
    for (const char *at = json; at && (at = strstr(at, "\"index\":")); at += 8)
        count++;
    zebkit_free(json);
    return count;
}

static void test_line_endings(void) {
    /* Each ending draws something, and something of its own. */
    const char *names[] = {"openArrow", "closedArrow", "rOpenArrow", "rClosedArrow", "circle",
                           "square",    "diamond",     "butt",       "slash"};
    enum { kNames = sizeof(names) / sizeof(names[0]) };
    shot shots[kNames];
    shot plain;
    {
        zebkit_pdf *pdf = blank(300, 200);
        add(pdf, 0, "{\"type\":\"line\",\"line\":[50,100,250,100],\"color\":[255,0,0],\"border\":2}");
        plain = render(pdf, 0);
        zebkit_pdf_close(pdf);
    }
    check_text(near_color(at(&plain, 150, 100), 255, 0, 0) && white(at(&plain, 150, 106)), "a line", NULL);
    for (int i = 0; i < kNames; ++i) {
        char json[256];
        snprintf(json, sizeof(json),
                 "{\"type\":\"line\",\"line\":[50,100,250,100],\"color\":[255,0,0],\"border\":2,"
                 "\"interiorColor\":[0,0,255],\"lineEndings\":[\"%s\",\"%s\"]}",
                 names[i], names[i]);
        zebkit_pdf *pdf = blank(300, 200);
        add(pdf, 0, json);
        shots[i] = render(pdf, 0);
        check_text(inked(&shots[i], 40, 102, 60, 112) > 0 && inked(&shots[i], 240, 102, 260, 112) > 0, names[i],
              "reaches beside the line, at both ends");
        zebkit_pdf *again = reopened(pdf);
        shot after = render(again, 0);
        check_text(same_pixels(&shots[i], &after) && listed(again, 0, names[i]), "the same after a save", names[i]);
        done(&after);
        zebkit_pdf_close(again);
        zebkit_pdf_close(pdf);
    }
    for (int i = 0; i < kNames; ++i) {
        for (int j = i + 1; j < kNames; ++j) {
            if (same_pixels(&shots[i], &shots[j]))
                check_text(0, "two endings look the same", names[i]);
        }
    }
    check_text(near_color(at(&shots[1], 243, 100), 0, 0, 255), "a closed arrow is filled with interiorColor", NULL);
    for (int i = 0; i < kNames; ++i)
        done(&shots[i]);
    done(&plain);
}

static void test_leader_and_caption(void) {
    zebkit_pdf *pdf = blank(300, 300);
    add(pdf, 0,
        "{\"type\":\"line\",\"line\":[50,100,250,100],\"color\":[255,0,0],\"border\":2,"
        "\"leader\":{\"length\":40,\"extension\":5,\"offset\":3},\"contents\":\"12.5\","
        "\"caption\":{\"show\":true,\"position\":\"inline\"}}");
    shot s = render(pdf, 0);
    /* Leader lines rise from the ends; the line is drawn at their height. */
    check_text(near_color(at(&s, 50, 120), 255, 0, 0) && near_color(at(&s, 250, 120), 255, 0, 0), "leader lines", NULL);
    check_text(near_color(at(&s, 80, 143), 255, 0, 0) && white(at(&s, 80, 100)), "the line at the leaders' height", NULL);
    check_text(near_color(at(&s, 50, 147), 255, 0, 0), "the leaders' extension past it", NULL);
    /* An inline caption breaks the line: its middle has the text, not a line. */
    check_text(inked(&s, 130, 138, 170, 150) > 10, "the caption is drawn", NULL);
    check_text(white(at(&s, 137, 150)) || white(at(&s, 137, 136)), "the line is broken around it", NULL);
    check_text(listed(pdf, 0, "\"position\":\"inline\"") && listed(pdf, 0, "\"length\":40"), "read back", NULL);
    /* Rect covers all of it, so the file's own /Rect is right. */
    check_text(listed(pdf, 0, "\"rect\":[") && !listed(pdf, 0, "\"rect\":[0.0,0.0,0.0,0.0]"), "a rect from the points", NULL);
    zebkit_pdf *again = reopened(pdf);
    shot after = render(again, 0);
    check_text(same_pixels(&s, &after), "the same after a save", NULL);
    done(&after);
    zebkit_pdf_close(again);
    /* The font is in the file: PDFium would fall back to one, other readers not. */
    {
        size_t size = 0;
        uint8_t *file = zebkit_pdf_save(pdf, 0, &size, NULL);
        int found_null_reference = 0, found_font = 0;
        for (size_t i = 0; file && i + 8 < size; ++i) {
            found_null_reference |= !memcmp(file + i, " 0 0 R", 6);
            found_font |= !memcmp(file + i, "/Helvetica", 10) && i + 10 < size;
        }
        check_text(!found_null_reference && found_font, "a standard font is written into the stream's resources", NULL);
        zebkit_free(file);
    }
    /* On top of the line. */
    add(pdf, 0, "{\"type\":\"line\",\"line\":[50,200,250,200],\"color\":[0,0,255],\"border\":2,"
                "\"contents\":\"top\",\"caption\":{\"show\":true,\"position\":\"top\"}}");
    done(&s);
    s = render(pdf, 0);
    check_text(near_color(at(&s, 150, 200), 0, 0, 255) && inked(&s, 130, 203, 170, 215) > 5, "a caption above an unbroken line",
          NULL);
    done(&s);
    zebkit_pdf_close(pdf);
}

static int runs_along(const shot *s, double y, double from, double to) {
    int runs = 0, inside = 0;
    for (double x = from; x < to; x += 1) {
        const int ink = !white(at(s, x, y));
        if (ink && !inside)
            runs++;
        inside = ink;
    }
    return runs;
}

static void test_shape_borders(void) {
    /* Dashed: the edge has gaps. */
    zebkit_pdf *pdf = blank(300, 300);
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[50,50,250,250],\"color\":[0,0,0],"
                "\"borderStyle\":{\"style\":\"dashed\",\"width\":2,\"dash\":[6,6]}}");
    shot s = render(pdf, 0);
    check_text(runs_along(&s, 51, 60, 240) > 10, "a dashed square", NULL);
    done(&s);
    zebkit_pdf *again = reopened(pdf);
    s = render(again, 0);
    check_text(runs_along(&s, 51, 60, 240) > 10 && listed(again, 0, "\"style\":\"dashed\""), "the same after a save", NULL);
    done(&s);
    zebkit_pdf_close(again);
    zebkit_pdf_close(pdf);

    pdf = blank(300, 300);
    add(pdf, 0, "{\"type\":\"circle\",\"rect\":[50,50,250,250],\"color\":[0,0,0],\"interiorColor\":[255,255,0],"
                "\"borderStyle\":{\"style\":\"dashed\",\"width\":2}}");
    s = render(pdf, 0);
    check_text(near_color(at(&s, 150, 150), 255, 255, 0) && white(at(&s, 55, 55)), "a dashed circle, filled", NULL);
    done(&s);
    zebkit_pdf_close(pdf);

    /* Cloudy: bumps beyond a straight edge, inside the rectangle. */
    const char *shapes[] = {
        "{\"type\":\"square\",\"rect\":[50,50,250,250],\"color\":[0,0,0],\"border\":1,"
        "\"borderEffect\":{\"style\":\"cloudy\",\"intensity\":2}}",
        "{\"type\":\"circle\",\"rect\":[50,50,250,250],\"color\":[0,0,0],\"border\":1,"
        "\"borderEffect\":{\"style\":\"cloudy\",\"intensity\":1}}",
        "{\"type\":\"polygon\",\"rect\":[50,50,250,250],\"color\":[0,0,0],\"border\":1,"
        "\"vertices\":[[60,60],[240,60],[150,240]],\"borderEffect\":{\"style\":\"cloudy\",\"intensity\":1}}",
    };
    const char *plains[] = {
        "{\"type\":\"square\",\"rect\":[50,50,250,250],\"color\":[0,0,0],\"border\":1}",
        "{\"type\":\"circle\",\"rect\":[50,50,250,250],\"color\":[0,0,0],\"border\":1}",
        "{\"type\":\"polygon\",\"rect\":[50,50,250,250],\"color\":[0,0,0],\"border\":1,"
        "\"vertices\":[[60,60],[240,60],[150,240]]}",
    };
    for (int i = 0; i < 3; ++i) {
        pdf = blank(300, 300);
        zebkit_pdf *other = blank(300, 300);
        add(pdf, 0, shapes[i]);
        add(other, 0, plains[i]);
        shot cloud = render(pdf, 0), straight = render(other, 0);
        check_text(!same_pixels(&cloud, &straight) && inked(&cloud, 40, 40, 260, 260) > inked(&straight, 40, 40, 260, 260),
              "a cloud has more edge than a straight line", shapes[i]);
        zebkit_pdf *saved = reopened(pdf);
        shot kept = render(saved, 0);
        check_text(same_pixels(&cloud, &kept) && listed(saved, 0, "cloudy"), "the same after a save", NULL);
        done(&kept);
        zebkit_pdf_close(saved);
        done(&cloud);
        done(&straight);
        zebkit_pdf_close(pdf);
        zebkit_pdf_close(other);
    }

    /* Opacity, fill and blend. */
    pdf = blank(300, 300);
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[50,50,250,250],\"interiorColor\":[0,0,0],\"border\":0,\"opacity\":0.5}");
    s = render(pdf, 0);
    {
        const uint8_t *p = at(&s, 150, 150);
        check_text(abs(p[0] - 128) < 20 && abs(p[2] - 128) < 20, "opacity in a fill", NULL);
    }
    done(&s);
    zebkit_pdf_close(pdf);
    pdf = blank(300, 300);
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[50,50,250,250],\"interiorColor\":[255,255,0],\"border\":0}");
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[100,100,200,200],\"interiorColor\":[0,255,255],\"border\":0,\"blend\":\"Multiply\"}");
    s = render(pdf, 0);
    check_text(near_color(at(&s, 150, 150), 0, 255, 0) && near_color(at(&s, 70, 70), 255, 255, 0), "blend: Multiply over yellow", NULL);
    done(&s);
    again = reopened(pdf);
    s = render(again, 0);
    check_text(near_color(at(&s, 150, 150), 0, 255, 0) && listed(again, 0, "\"blend\":\"Multiply\""), "the same after a save", NULL);
    done(&s);
    zebkit_pdf_close(again);
    zebkit_pdf_close(pdf);
    /* Into what PDFium generates too: a highlight's multiply replaced. */
    pdf = blank(300, 300);
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[0,0,300,300],\"interiorColor\":[255,255,0],\"border\":0}");
    add(pdf, 0, "{\"type\":\"highlight\",\"rect\":[100,100,200,200],\"color\":[0,255,255],\"blend\":\"Normal\","
                "\"quads\":[[100,200,200,200,100,100,200,100]]}");
    s = render(pdf, 0);
    check_text(near_color(at(&s, 150, 150), 0, 255, 255), "blend replaced on a highlight", NULL);
    done(&s);
    zebkit_pdf_close(pdf);

    /* Rotation: a wide box turned a quarter stands tall. */
    pdf = blank(300, 300);
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[100,140,200,160],\"interiorColor\":[255,0,0],\"border\":0,\"rotation\":90}");
    s = render(pdf, 0);
    check_text(near_color(at(&s, 150, 190), 255, 0, 0) && near_color(at(&s, 150, 110), 255, 0, 0) && white(at(&s, 190, 150)),
          "rotation 90: wide became tall", NULL);
    check_text(listed(pdf, 0, "\"rotation\":90"), "rotation read back", NULL);
    /* Changing something else keeps the turned box, not a growing one. */
    zebkit_pdf_annotation_update(pdf, 0, 0, "{\"interiorColor\":[0,0,255]}", NULL);
    done(&s);
    s = render(pdf, 0);
    check_text(near_color(at(&s, 150, 190), 0, 0, 255) && white(at(&s, 190, 150)), "it stays that shape when changed", NULL);
    done(&s);
    zebkit_pdf_annotation_update(pdf, 0, 0, "{\"rotation\":null}", NULL);
    s = render(pdf, 0);
    check_text(near_color(at(&s, 190, 150), 0, 0, 255) && white(at(&s, 150, 190)), "rotation removed: its box again", NULL);
    done(&s);
    zebkit_pdf_close(pdf);
    /* Polylines end too. */
    pdf = blank(300, 300);
    add(pdf, 0, "{\"type\":\"polyline\",\"vertices\":[[50,50],[250,50],[250,250]],\"color\":[255,0,0],\"border\":2,"
                "\"lineEndings\":[\"butt\",\"closedArrow\"],\"interiorColor\":[0,255,0]}");
    s = render(pdf, 0);
    check_text(near_color(at(&s, 50, 52), 255, 0, 0) && near_color(at(&s, 250, 243), 0, 255, 0), "a polyline's endings", NULL);
    done(&s);
    zebkit_pdf_close(pdf);
}

static void test_free_text(void) {
    zebkit_pdf *pdf = blank(300, 300);
    char *error = NULL;
    /* The font as a family; Ahem's boxes make pixels certain. */
    check_text(zebkit_pdf_font_add(pdf, "Ahem", font_bytes, font_size, NULL), "a font for free text", NULL);
    check_text(!zebkit_pdf_font_add(pdf, "Bad", "nope", 4, &error) && error, "what is not a font fails", error);
    zebkit_free(error);
    error = NULL;
    add(pdf, 0, "{\"type\":\"freeText\",\"rect\":[50,200,250,280],\"contents\":\"XX\",\"font\":\"Ahem\",\"fontSize\":20,"
                "\"textColor\":[255,0,0],\"color\":[255,255,200],\"borderStyle\":{\"width\":2},\"borderColor\":[0,0,255]}");
    shot s = render(pdf, 0);
    check_text(near_color(at(&s, 60, 268), 255, 0, 0), "the font, the size, the colour", NULL);
    check_text(near_color(at(&s, 240, 220), 255, 255, 200), "the background", NULL);
    check_text(near_color(at(&s, 150, 201), 0, 0, 255), "the border's own colour", NULL);
    done(&s);
    /* Alignment. */
    add(pdf, 0, "{\"type\":\"freeText\",\"rect\":[50,100,250,140],\"contents\":\"X\",\"font\":\"Ahem\",\"fontSize\":20,"
                "\"align\":\"right\"}");
    add(pdf, 0, "{\"type\":\"freeText\",\"rect\":[50,50,250,90],\"contents\":\"X\",\"font\":\"Ahem\",\"fontSize\":20,"
                "\"align\":\"center\"}");
    s = render(pdf, 0);
    check_text(!white(at(&s, 240, 125)) && white(at(&s, 60, 125)), "aligned right", NULL);
    check_text(!white(at(&s, 150, 75)) && white(at(&s, 60, 75)) && white(at(&s, 240, 75)), "aligned to the centre", NULL);
    done(&s);
    /* It wraps at the box, and changing the text redraws it. */
    zebkit_pdf_annotation_update(pdf, 0, 0, "{\"contents\":\"XXXXXXXXX XXXXXXXXX\"}", NULL);
    s = render(pdf, 0);
    check_text(!white(at(&s, 60, 268)) && !white(at(&s, 60, 244)), "wrapped onto a second line", NULL);
    done(&s);
    zebkit_pdf *again = reopened(pdf);
    check_text(listed(again, 0, "\"font\":\"Ahem\"") && listed(again, 0, "\"fontSize\":20") && listed(again, 0, "\"align\":\"right\""),
          "read back after a save", NULL);
    s = render(again, 0);
    check_text(!white(at(&s, 240, 125)), "an embedded font after a save", NULL);
    done(&s);
    zebkit_pdf_close(again);
    zebkit_pdf_close(pdf);

    /* Rich text and the default style. */
    pdf = blank(300, 300);
    zebkit_pdf_font_add(pdf, "Ahem", font_bytes, font_size, NULL);
    add(pdf, 0,
        "{\"type\":\"freeText\",\"rect\":[20,200,280,290],\"contents\":\"ignored\","
        "\"defaultStyle\":\"font: 10pt Ahem; color: #00ff00\","
        "\"richText\":\"<?xml version=\\\"1.0\\\"?><body xmlns=\\\"http://www.w3.org/1999/xhtml\\\">"
        "<p><span style=\\\"color:#ff0000;font-size:20pt;font-family:Ahem\\\">XX</span>XX</p>"
        "<p style=\\\"text-align:right\\\"><span style=\\\"color:#0000ff;font-size:20pt;font-family:Ahem\\\">X</span></p></body>\"}");
    s = render(pdf, 0);
    check_text(near_color(at(&s, 30, 278), 255, 0, 0), "/RC: a span's colour and size", NULL);
    check_text(near_color(at(&s, 66, 271), 0, 255, 0), "/DS: the default style beside it", NULL);
    check_text(near_color(at(&s, 270, 254), 0, 0, 255), "/RC: a paragraph aligned right", NULL);
    check_text(listed(pdf, 0, "\"richText\":") && listed(pdf, 0, "\"defaultStyle\":"), "read back", NULL);
    done(&s);
    /* Malformed /RC falls to /Contents and entities stay unexpanded. */
    add(pdf, 0, "{\"type\":\"freeText\",\"rect\":[20,100,280,190],\"contents\":\"XX\",\"font\":\"Ahem\",\"fontSize\":20,"
                "\"richText\":\"<body><p>unclosed\"}");
    add(pdf, 0, "{\"type\":\"freeText\",\"rect\":[20,20,280,90],\"contents\":\"XX\",\"font\":\"Ahem\",\"fontSize\":20,"
                "\"richText\":\"<!DOCTYPE b [<!ENTITY x \\\"XXXX\\\">]><b>&x;&x;&x;</b>\"}");
    s = render(pdf, 0);
    check_text(!white(at(&s, 30, 178)), "XHTML that is broken: /Contents is drawn", NULL);
    check_text(!white(at(&s, 30, 78)), "XHTML with a DTD: not read", NULL);
    done(&s);
    zebkit_pdf_close(pdf);

    /* Callout: a line from far away to the box, with its ending. */
    pdf = blank(300, 300);
    add(pdf, 0, "{\"type\":\"freeText\",\"rect\":[150,200,280,280],\"contents\":\"t\",\"textColor\":[255,0,0],"
                "\"borderStyle\":{\"width\":1},\"callout\":[30,30,100,100,150,240],\"calloutEnding\":\"closedArrow\"}");
    s = render(pdf, 0);
    check_text(!white(at(&s, 65, 65)), "the callout's first part", NULL);
    check_text(!white(at(&s, 125, 170)), "and its second", NULL);
    check_text(inked(&s, 20, 20, 40, 40) > 8, "its ending", NULL);
    check_text(listed(pdf, 0, "\"calloutEnding\":\"closedArrow\""), "read back", NULL);
    check_text(!white(at(&s, 215, 200)), "the box where it was", NULL);
    done(&s);
    again = reopened(pdf);
    s = render(again, 0);
    check_text(!white(at(&s, 65, 65)) && listed(again, 0, "\"callout\":"), "the same after a save", NULL);
    done(&s);
    /* The whole rectangle covers both, /RD says where the box is. */
    check_text(listed(again, 0, "\"RD\":"), "/RD", NULL);
    zebkit_pdf_close(again);
    zebkit_pdf_close(pdf);

    /* Turned. */
    pdf = blank(300, 300);
    zebkit_pdf_font_add(pdf, "Ahem", font_bytes, font_size, NULL);
    add(pdf, 0, "{\"type\":\"freeText\",\"rect\":[100,140,200,160],\"contents\":\"XXXXX\",\"font\":\"Ahem\",\"fontSize\":18,"
                "\"rotation\":90,\"color\":[200,200,255]}");
    s = render(pdf, 0);
    check_text(!white(at(&s, 150, 190)) && white(at(&s, 195, 150)), "free text turned", NULL);
    done(&s);
    zebkit_pdf_close(pdf);
}

static void test_stamps_and_the_rest(void) {
    const char *standard[] = {"Approved", "Experimental", "NotApproved", "AsIs", "Expired", "NotForPublicRelease",
                              "Confidential", "Final", "Sold", "Departmental", "ForComment", "TopSecret", "Draft",
                              "ForPublicRelease"};
    shot first = {0};
    for (size_t i = 0; i < sizeof(standard) / sizeof(standard[0]); ++i) {
        zebkit_pdf *pdf = blank(300, 200);
        char json[200];
        snprintf(json, sizeof(json), "{\"type\":\"stamp\",\"rect\":[20,50,280,150],\"stamp\":\"%s\"}", standard[i]);
        add(pdf, 0, json);
        shot s = render(pdf, 0);
        /* A frame around ink, nothing beyond the rectangle. */
        check_text(inked(&s, 20, 50, 280, 150) > 500 && inked(&s, 0, 0, 15, 200) == 0 && !white(at(&s, 150, 51)),
              standard[i], "a frame and its label");
        if (!i) {
            first = s;
        } else {
            check_text(!same_pixels(&first, &s), "not the same drawing as the first", standard[i]);
            done(&s);
        }
        zebkit_pdf *again = reopened(pdf);
        check_text(listed(again, 0, "\"stamp\":\""), "read back", standard[i]);
        zebkit_pdf_close(again);
        zebkit_pdf_close(pdf);
    }
    done(&first);

    /* A stamp of any text. */
    zebkit_pdf *pdf = blank(300, 200);
    add(pdf, 0, "{\"type\":\"stamp\",\"rect\":[20,50,280,150],\"stampText\":\"PAID\",\"color\":[0,128,0],\"rotation\":10}");
    shot s = render(pdf, 0);
    check_text(inked(&s, 10, 30, 290, 170) > 500 && listed(pdf, 0, "\"stampText\":\"PAID\"") && listed(pdf, 0, "\"rotation\":10"),
          "a text stamp, turned", NULL);
    done(&s);
    zebkit_pdf_close(pdf);

    /* A stamp from another document's page, as vectors: crisp at any scale. */
    static const char page[] =
        "%PDF-1.7\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
        "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
        "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Contents 4 0 R >> endobj\n"
        "4 0 obj << >> stream\n1 0 0 rg 0 0 50 100 re f\nendstream endobj\n"
        "trailer << /Root 1 0 R >>\n%%EOF\n";
    zebkit_pdf *source = zebkit_pdf_open(page, sizeof(page) - 1, NULL, NULL);
    pdf = blank(300, 300);
    int stamp = add(pdf, 0, "{\"type\":\"stamp\",\"rect\":[50,50,250,250]}");
    char *error = NULL;
    check_text(zebkit_pdf_annotation_stamp_page(pdf, 0, stamp, source, 0, &error), "a stamp from a page", error);
    zebkit_free(error);
    error = NULL;
    refused(!zebkit_pdf_annotation_stamp_page(pdf, 0, stamp, source, 5, &error), &error, "a page it does not have");
    shot big = render_at(pdf, 0, 8);
    /* The edge at x = 150 of the page: one pixel apart at 8 pixels a point. */
    {
        const uint8_t *left = big.rgba + (size_t)(big.height / 2) * big.width * 4 + (150 * 8 - 2) * 4;
        const uint8_t *right = big.rgba + (size_t)(big.height / 2) * big.width * 4 + (150 * 8 + 1) * 4;
        check_text(near_color(left, 255, 0, 0) && white(right), "vectors: sharp when enlarged", NULL);
    }
    done(&big);
    /* The source may go; and a save keeps it. An update does not replace it. */
    zebkit_pdf_close(source);
    zebkit_pdf_annotation_update(pdf, 0, stamp, "{\"contents\":\"a note\",\"author\":\"me\"}", NULL);
    zebkit_pdf *again = reopened(pdf);
    s = render(again, 0);
    check_text(near_color(at(&s, 100, 150), 255, 0, 0) && white(at(&s, 200, 150)), "and after a save", NULL);
    done(&s);
    zebkit_pdf_close(again);
    zebkit_pdf_close(pdf);

    /* Carets, notes with every icon, attachments with every icon. */
    pdf = blank(300, 300);
    add(pdf, 0, "{\"type\":\"caret\",\"rect\":[100,100,140,140],\"color\":[255,0,0],\"caretSymbol\":\"paragraph\"}");
    s = render(pdf, 0);
    check_text(inked(&s, 100, 100, 140, 140) > 50, "a caret", NULL);
    check_text(listed(pdf, 0, "\"caretSymbol\":\"paragraph\""), "read back", NULL);
    done(&s);
    zebkit_pdf_close(pdf);
    const char *notes[] = {"Comment", "Key", "Note", "Help", "NewParagraph", "Paragraph", "Insert"};
    shot note_shots[7];
    for (int i = 0; i < 7; ++i) {
        pdf = blank(100, 100);
        char json[128];
        snprintf(json, sizeof(json), "{\"type\":\"text\",\"rect\":[10,10,90,90],\"icon\":\"%s\",\"contents\":\"hi\"}", notes[i]);
        add(pdf, 0, json);
        note_shots[i] = render(pdf, 0);
        check_text(inked(&note_shots[i], 10, 10, 90, 90) > 100, notes[i], "a note's icon");
        for (int j = 0; j < i; ++j)
            check_text(!same_pixels(&note_shots[i], &note_shots[j]), "drawn differently from another", notes[j]);
        zebkit_pdf_close(pdf);
    }
    for (int i = 0; i < 7; ++i)
        done(&note_shots[i]);

    const char *icons[] = {"Graph", "PushPin", "Paperclip", "Tag"};
    for (int i = 0; i < 4; ++i) {
        pdf = blank(100, 100);
        char json[128];
        snprintf(json, sizeof(json), "{\"type\":\"fileAttachment\",\"rect\":[10,10,90,90],\"icon\":\"%s\"}", icons[i]);
        add(pdf, 0, json);
        s = render(pdf, 0);
        check_text(inked(&s, 10, 10, 90, 90) > 50, icons[i], "an attachment's icon");
        done(&s);
        zebkit_pdf_close(pdf);
    }

    /* The file itself, through a save. */
    pdf = blank(100, 100);
    const char payload[] = "the attached file's bytes\0with a zero";
    int attachment = add(pdf, 0, "{\"type\":\"fileAttachment\",\"rect\":[10,10,30,30]}");
    error = NULL;
    check_text(zebkit_pdf_annotation_attach_file(pdf, 0, attachment, "notes.txt", payload, sizeof(payload), &error), "a file embedded", error);
    zebkit_free(error);
    error = NULL;
    refused(!zebkit_pdf_annotation_attach_file(pdf, 0, attachment, "", payload, 1, &error), &error, "a file without a name");
    int note = add(pdf, 0, "{\"type\":\"text\",\"rect\":[40,10,60,30]}");
    refused(!zebkit_pdf_annotation_attach_file(pdf, 0, note, "a", payload, 1, &error), &error, "a note takes no file");
    refused(!zebkit_pdf_annotation_file(pdf, 0, note, NULL, NULL, &error), &error, "a note has no file");
    again = reopened(pdf);
    char *name = NULL;
    size_t size = 0;
    uint8_t *back = zebkit_pdf_annotation_file(again, 0, attachment, &name, &size, NULL);
    check_text(back && size == sizeof(payload) && !memcmp(back, payload, size) && name && !strcmp(name, "notes.txt"),
          "the file after a save", name);
    zebkit_free(back);
    zebkit_free(name);
    zebkit_pdf_close(again);
    zebkit_pdf_close(pdf);
}

static void test_discussion(void) {
    zebkit_pdf *pdf = blank(300, 300);
    char *error = NULL;
    add(pdf, 0, "{\"type\":\"text\",\"rect\":[10,10,30,30],\"contents\":\"first\"}");
    add(pdf, 0, "{\"type\":\"text\",\"rect\":[40,10,60,30],\"contents\":\"answer\",\"inReplyTo\":0,\"replyType\":\"reply\"}");
    add(pdf, 0, "{\"type\":\"text\",\"rect\":[70,10,90,30],\"inReplyTo\":0,\"state\":\"accepted\",\"stateModel\":\"review\",\"flags\":18}");
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[100,100,120,120],\"inReplyTo\":0,\"replyType\":\"group\"}");
    check_text(listed(pdf, 0, "\"replies\":[1,2,3]"), "what replies to it", NULL);
    check_text(listed(pdf, 0, "\"inReplyTo\":0") && listed(pdf, 0, "\"replyType\":\"group\"") && listed(pdf, 0, "\"replyType\":\"reply\""),
          "replies and groups read", NULL);
    check_text(listed(pdf, 0, "\"state\":\"accepted\"") && listed(pdf, 0, "\"stateModel\":\"review\""), "a state read", NULL);

    /* Cycles cannot be made, by any road. */
    char *unchanged = listing(pdf, 0);
    refused(!zebkit_pdf_annotation_update(pdf, 0, 0, "{\"inReplyTo\":1}", &error), &error, "a reply to its own reply");
    refused(!zebkit_pdf_annotation_update(pdf, 0, 0, "{\"inReplyTo\":0}", &error), &error, "a reply to itself");
    char changes[128];
    char *read = listing(pdf, 0);
    const char *object_at = strstr(read, "\"index\":1");
    /* Keys are in order: "object" follows "index". */
    object_at = object_at ? strstr(object_at, "\"object\":") : NULL;
    int second_object = object_at ? atoi(object_at + 9) : 0;
    zebkit_free(read);
    check_text(second_object > 0, "an annotation's object number", NULL);
    snprintf(changes, sizeof(changes), "{\"entries\":{\"IRT\":{\"ref\":%d}}}", second_object);
    refused(!zebkit_pdf_annotation_update(pdf, 0, 0, changes, &error), &error, "and not by entries");
    read = listing(pdf, 0);
    check_text(unchanged && read && !strcmp(unchanged, read), "the document is as it was", NULL);
    zebkit_free(unchanged);
    zebkit_free(read);

    /* Not a reply. */
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"text\",\"rect\":[1,1,2,2],\"inReplyTo\":99}", &error) < 0, &error, "a reply to nothing");
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"text\",\"rect\":[1,1,2,2],\"state\":\"happy\"}", &error) < 0, &error, "no such state");
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"text\",\"rect\":[1,1,2,2],\"replyType\":\"x\"}", &error) < 0, &error, "no such reply type");
    check_text(count_annotations(pdf, 0) == 4, "nothing was added", NULL);

    /* A popup, made and read. */
    add(pdf, 0, "{\"type\":\"text\",\"rect\":[150,150,170,170],\"contents\":\"with popup\","
                "\"popup\":{\"rect\":[180,150,280,220],\"open\":true}}");
    check_text(listed(pdf, 0, "\"type\":\"popup\"") && listed(pdf, 0, "\"popup\":5"), "a popup, linked", NULL);
    zebkit_pdf *again = reopened(pdf);
    check_text(listed(again, 0, "\"replies\":[1,2,3]") && listed(again, 0, "\"popup\":5") && listed(again, 0, "\"state\":\"accepted\""),
          "the discussion after a save", NULL);
    zebkit_pdf_close(again);
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"text\",\"rect\":[1,1,2,2],\"popup\":{\"rect\":[1,2]}}", &error) < 0, &error, "a popup without a rect");
    check_text(count_annotations(pdf, 0) == 6, "still six", NULL);

    /* Removing takes the replies and the popup with it. */
    check_text(zebkit_pdf_annotation_remove(pdf, 0, 4, NULL) && count_annotations(pdf, 0) == 4, "a popup goes with its note", NULL);
    check_text(zebkit_pdf_annotation_remove(pdf, 0, 0, NULL) && count_annotations(pdf, 0) == 0, "replies go with what they answer", NULL);
    zebkit_pdf_close(pdf);

    /* A file with a cycle of /IRT reads, and can be saved and drawn. */
    static const char cycle[] =
        "%PDF-1.7\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
        "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
        "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Annots [4 0 R 5 0 R] >> endobj\n"
        "4 0 obj << /Type /Annot /Subtype /Text /Rect [1 1 20 20] /IRT 5 0 R >> endobj\n"
        "5 0 obj << /Type /Annot /Subtype /Text /Rect [30 1 50 20] /IRT 4 0 R >> endobj\n"
        "trailer << /Root 1 0 R >>\n%%EOF\n";
    pdf = zebkit_pdf_open(cycle, sizeof(cycle) - 1, NULL, NULL);
    check_text(pdf != NULL && listed(pdf, 0, "\"inReplyTo\":1") && listed(pdf, 0, "\"inReplyTo\":0"), "a file's cycle is read", NULL);
    shot s = render(pdf, 0);
    check_text(s.rgba != NULL, "and drawn", NULL);
    done(&s);
    refused(!zebkit_pdf_annotation_update(pdf, 0, 0, "{\"contents\":\"x\"}", &error), &error, "and not made worse: no changes to what loops");
    check_text(zebkit_pdf_annotation_remove(pdf, 0, 0, NULL) && count_annotations(pdf, 0) == 0, "and can be removed", NULL);
    zebkit_pdf_close(pdf);
}

static void test_measurement(void) {
    zebkit_pdf *pdf = blank(400, 400);
    char *error = NULL;
    add(pdf, 0, "{\"type\":\"line\",\"line\":[50,100,250,100],\"color\":[255,0,0],"
                "\"measure\":{\"unit\":\"mm\",\"scale\":0.5,\"precision\":1}}");
    check_text(listed(pdf, 0, "\"contents\":\"100.0 mm\"") && listed(pdf, 0, "\"text\":\"100.0 mm\"") && listed(pdf, 0, "\"value\":100"),
          "distance", NULL);
    check_text(listed(pdf, 0, "\"intent\":\"LineDimension\""), "the line's intent", NULL);
    shot s = render(pdf, 0);
    check_text(inked(&s, 130, 90, 170, 110) > 10 && white(at(&s, 137, 111)), "as the caption", NULL);
    done(&s);
    /* It follows the shape. */
    zebkit_pdf_annotation_update(pdf, 0, 0, "{\"line\":[50,100,150,100]}", NULL);
    check_text(listed(pdf, 0, "\"contents\":\"50.0 mm\""), "and follows a change", NULL);
    add(pdf, 0, "{\"type\":\"polyline\",\"vertices\":[[0,0],[100,0],[100,100]],"
                "\"measure\":{\"unit\":\"cm\",\"scale\":1,\"precision\":0}}");
    check_text(listed(pdf, 0, "\"contents\":\"200 cm\"") && listed(pdf, 0, "\"intent\":\"PolyLineDimension\""), "perimeter", NULL);
    add(pdf, 0, "{\"type\":\"polygon\",\"vertices\":[[200,200],[300,200],[300,300],[200,300]],\"color\":[0,0,255],"
                "\"measure\":{\"unit\":\"m\",\"scale\":2,\"precision\":2}}");
    check_text(listed(pdf, 0, "\"contents\":\"40000.00 m\xc2\xb2\"") && listed(pdf, 0, "\"intent\":\"PolygonDimension\""), "area", NULL);
    s = render(pdf, 0);
    check_text(inked(&s, 220, 240, 280, 260) > 10, "drawn in its middle", NULL);
    done(&s);
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[0,300,100,400],\"measure\":{\"unit\":\"u\",\"scale\":1,\"precision\":0}}");
    check_text(listed(pdf, 0, "\"contents\":\"10000 u\xc2\xb2\""), "a square's area", NULL);
    zebkit_pdf *again = reopened(pdf);
    check_text(listed(again, 0, "\"contents\":\"50.0 mm\"") && listed(again, 0, "\"unit\":\"mm\""), "after a save", NULL);
    zebkit_pdf_close(again);
    const char *wrong[] = {
        "{\"type\":\"text\",\"rect\":[1,1,2,2],\"measure\":{\"unit\":\"mm\",\"scale\":1}}",
        "{\"type\":\"line\",\"line\":[1,1,2,2],\"measure\":{\"scale\":1}}",
        "{\"type\":\"line\",\"line\":[1,1,2,2],\"measure\":{\"unit\":\"mm\",\"scale\":-1}}",
        "{\"type\":\"line\",\"line\":[1,1,2,2],\"measure\":{\"unit\":\"mm\",\"scale\":1,\"precision\":99}}",
        "{\"type\":\"line\",\"line\":[1,1,2,2],\"measure\":\"mm\"}",
    };
    for (size_t i = 0; i < sizeof(wrong) / sizeof(wrong[0]); ++i)
        refused(zebkit_pdf_annotation_add(pdf, 0, wrong[i], &error) < 0, &error, wrong[i]);
    zebkit_pdf_close(pdf);
}

static const char text_page[] =
    "%PDF-1.7\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Contents 4 0 R"
    " /Resources << /Font << /F1 5 0 R >> >> >> endobj\n"
    "4 0 obj << >> stream\nBT /F1 24 Tf 20 120 Td (Hello zebkit) Tj ET\nendstream endobj\n"
    "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
    "trailer << /Root 1 0 R >>\n%%EOF\n";

static void test_viewers(void) {
    zebkit_pdf *pdf = blank(300, 300);
    char *error = NULL;
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[50,50,150,150],\"interiorColor\":[255,0,0],\"border\":0}");
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[100,100,200,200],\"interiorColor\":[0,0,255],\"border\":0}");
    add(pdf, 0, "{\"type\":\"line\",\"line\":[0,0,300,300],\"color\":[0,255,0]}");
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[250,20,280,50],\"flags\":2}");
    check_text(zebkit_pdf_annotation_at(pdf, 0, 60, 100, 0, NULL) == 0, "the one at a point", NULL);
    check_text(zebkit_pdf_annotation_at(pdf, 0, 120, 110, 0, NULL) == 1, "the top one of two", NULL);
    check_text(zebkit_pdf_annotation_at(pdf, 0, 40, 40, 1, NULL) == 2, "a line, on it", NULL);
    check_text(zebkit_pdf_annotation_at(pdf, 0, 40, 200, 0, NULL) == -1, "a line's rectangle is not a line", NULL);
    check_text(zebkit_pdf_annotation_at(pdf, 0, 262, 30, 0, NULL) == -1, "hidden ones do not count", NULL);
    check_text(zebkit_pdf_annotation_at(pdf, 0, 10, 290, 0, NULL) == -1, "nothing there", NULL);
    refused(zebkit_pdf_annotation_at(pdf, 9, 0, 0, 0, &error) < 0, &error, "no such page");

    /* The order. */
    shot before = render(pdf, 0);
    check_text(near_color(at(&before, 130, 120), 0, 0, 255), "blue on top", NULL);
    check_text(zebkit_pdf_annotation_move(pdf, 0, 1, 0, NULL), "moved under", NULL);
    shot after = render(pdf, 0);
    check_text(near_color(at(&after, 130, 120), 255, 0, 0), "red on top now", NULL);
    done(&before);
    done(&after);
    refused(!zebkit_pdf_annotation_move(pdf, 0, 99, 0, &error), &error, "a move of nothing");
    refused(!zebkit_pdf_annotation_move(pdf, 0, 0, 99, &error), &error, "a move to nowhere");
    refused(!zebkit_pdf_annotation_move(pdf, 0, 0, -1, &error), &error, "a move to before the start");
    zebkit_pdf_close(pdf);

    /* Markup from a range of the text. */
    pdf = zebkit_pdf_open(text_page, sizeof(text_page) - 1, NULL, NULL);
    int mark = add(pdf, 0, "{\"type\":\"highlight\",\"textRange\":[0,5],\"color\":[255,255,0]}");
    check_text(mark == 0 && listed(pdf, 0, "\"quads\":[["), "a highlight of Hello", NULL);
    shot s = render(pdf, 0);
    check_text(near_color(at(&s, 40, 135), 255, 255, 0), "it is there", NULL);
    check_text(white(at(&s, 76, 135)), "and not past Hello", NULL);
    done(&s);
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"highlight\",\"textRange\":[0,5000]}", &error) < 0, &error, "a range past the text");
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"highlight\",\"textRange\":[-1,2]}", &error) < 0, &error, "a range before it");
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"highlight\",\"textRange\":\"all\"}", &error) < 0, &error, "a range of no numbers");
    zebkit_pdf_close(pdf);

    /* Copying, in the document and to another. */
    pdf = blank(300, 300);
    zebkit_pdf_page_insert(pdf, 1, 300, 300, NULL);
    zebkit_pdf *other = blank(300, 300);
    add(pdf, 0, "{\"type\":\"stamp\",\"rect\":[50,50,150,150],\"stamp\":\"Draft\",\"contents\":\"copy me\","
                "\"popup\":{\"rect\":[200,200,280,280]},\"entries\":{\"MyApp_x\":{\"y\":[1,2,{\"z\":\"/Name\"}]}}}");
    check_text(zebkit_pdf_annotation_copy(pdf, 0, 0, pdf, 1, NULL) == 0, "to another page", NULL);
    check_text(zebkit_pdf_annotation_copy(pdf, 0, 0, other, 0, NULL) == 0, "to another document", NULL);
    refused(zebkit_pdf_annotation_copy(pdf, 0, 99, other, 0, &error) < 0, &error, "a copy of nothing");
    refused(zebkit_pdf_annotation_copy(pdf, 0, 0, other, 99, &error) < 0, &error, "a copy to no page");
    refused(zebkit_pdf_annotation_copy(pdf, 0, 0, NULL, 0, &error) < 0, &error, "a copy to no document");
    shot a = render(pdf, 1), b = render(other, 0);
    check_text(inked(&a, 50, 50, 150, 150) > 300 && same_pixels(&a, &b), "the same appearance on both", NULL);
    check_text(listed(other, 0, "\"MyApp_x\":{\"y\":[1,2,{\"z\":\"/Name\"}]}") && listed(other, 0, "copy me") && !listed(other, 0, "\"popup\":"),
          "its entries, not its popup", NULL);
    /* The source gone: what was copied is the other's own. */
    zebkit_pdf_close(pdf);
    zebkit_pdf *again = reopened(other);
    shot c = render(again, 0);
    check_text(same_pixels(&b, &c), "after the first is closed, and a save", NULL);
    done(&a);
    done(&b);
    done(&c);
    zebkit_pdf_close(again);
    zebkit_pdf_close(other);

    /* One flattened, one left. */
    pdf = blank(300, 300);
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[50,50,100,100],\"interiorColor\":[255,0,0],\"border\":0}");
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[150,150,200,200],\"interiorColor\":[0,0,255],\"border\":0}");
    check_text(zebkit_pdf_annotation_flatten(pdf, 0, 0, NULL) && count_annotations(pdf, 0) == 1, "one flattened", NULL);
    shot flat = render(pdf, 0);
    check_text(near_color(at(&flat, 75, 75), 255, 0, 0) && near_color(at(&flat, 175, 175), 0, 0, 255), "drawn the same", NULL);
    zebkit_pdf_render_options plain = {0};
    plain.scale = 1;
    plain.background = 0xffffffff;
    int w, h;
    uint8_t *content = zebkit_pdf_render(pdf, 0, &plain, &w, &h, NULL);
    shot only = {content, w, h};
    check_text(near_color(at(&only, 75, 75), 255, 0, 0) && white(at(&only, 175, 175)), "as content, the other not", NULL);
    done(&only);
    done(&flat);
    again = reopened(pdf);
    content = zebkit_pdf_render(again, 0, &plain, &w, &h, NULL);
    only = (shot){content, w, h};
    check_text(near_color(at(&only, 75, 75), 255, 0, 0), "and after a save", NULL);
    done(&only);
    zebkit_pdf_close(again);
    refused(!zebkit_pdf_annotation_flatten(pdf, 0, 9, &error), &error, "flatten of nothing");
    zebkit_pdf_close(pdf);
    /* Page without an appearance: nothing to flatten. */
    pdf = blank(100, 100);
    add(pdf, 0, "{\"type\":\"link\",\"rect\":[1,1,9,9],\"url\":\"https://example.com/\"}");
    refused(!zebkit_pdf_annotation_flatten(pdf, 0, 0, &error), &error, "a link draws nothing to flatten");
    /* Its action, as data. */
    char *action = zebkit_pdf_annotation_action(pdf, 0, 0, NULL);
    check_text(action && strstr(action, "\"kind\":\"uri\"") && strstr(action, "\"handled\":false"), "an action read", action);
    zebkit_free(action);
    refused(!zebkit_pdf_annotation_action(pdf, 0, 5, &error), &error, "an action of nothing");
    zebkit_pdf_close(pdf);
}

static const char *const hostile_xfdf[] = {
    "not xml at all",
    "<other/>",
    /* A DTD at all: not read. */
    "<?xml version=\"1.0\"?><!DOCTYPE lolz [<!ENTITY lol \"lol\"><!ENTITY lol2 \"&lol;&lol;&lol;\">"
    "<!ENTITY lol3 \"&lol2;&lol2;&lol2;\">]><xfdf><annots><text page=\"0\" rect=\"1,1,2,2\"><contents>&lol3;</contents>"
    "</text></annots></xfdf>",
    "<?xml version=\"1.0\"?><!DOCTYPE x [<!ENTITY e SYSTEM \"file:///etc/passwd\">]><xfdf><annots>"
    "<text page=\"0\" rect=\"1,1,2,2\"><contents>&e;</contents></text></annots></xfdf>",
    "<!DOCTYPE xfdf SYSTEM \"http://127.0.0.1:1/x.dtd\"><xfdf/>",
    /* Wrong values. */
    "<xfdf><annots><text page=\"99\" rect=\"1,1,2,2\"/></annots></xfdf>",
    "<xfdf><annots><text page=\"-1\" rect=\"1,1,2,2\"/></annots></xfdf>",
    "<xfdf><annots><text page=\"x\" rect=\"1,1,2,2\"/></annots></xfdf>",
    "<xfdf><annots><text page=\"0\"/></annots></xfdf>",
    "<xfdf><annots><text page=\"0\" rect=\"a,b,c,d\"/></annots></xfdf>",
    "<xfdf><annots><text page=\"0\" rect=\"1,1,2\"/></annots></xfdf>",
    "<xfdf><annots><text page=\"0\" rect=\"1,1,2,2\" color=\"red\"/></annots></xfdf>",
    "<xfdf><annots><nonsense page=\"0\" rect=\"1,1,2,2\"/></annots></xfdf>",
    "<xfdf><annots><text page=\"0\" rect=\"1,1,2,2\" flags=\"nope\"/></annots></xfdf>",
    "<xfdf><annots><highlight page=\"0\" rect=\"1,1,2,2\" coords=\"1,2,3\"/></annots></xfdf>",
    "<xfdf><annots><line page=\"0\" rect=\"1,1,2,2\" start=\"1\" end=\"2,2\"/></annots></xfdf>",
    "<xfdf><annots><text page=\"0\" rect=\"1,1,2,2\"><zebkit-entries>not json</zebkit-entries></text></annots></xfdf>",
    "<xfdf><annots><text page=\"0\" rect=\"1,1,2,2\"><zebkit-entries>{\"AP\":{}}</zebkit-entries></text></annots></xfdf>",
    "<xfdf><annots><text page=\"0\" rect=\"1,1,2,2\"><zebkit-entries>{\"Popup\":{\"ref\":1}}</zebkit-entries></text></annots></xfdf>",
    "<xfdf><annots><text page=\"0\" rect=\"1,1,2,2\"><zebkit-entries>{\"FS\":{\"F\":\"x\"}}</zebkit-entries></text></annots></xfdf>",
    "<xfdf><annots><ink page=\"0\" rect=\"1,1,2,2\"><inklist><gesture>1,2,3</gesture></inklist></ink></annots></xfdf>",
    /* A cycle of replies. */
    "<xfdf><annots><text page=\"0\" rect=\"1,1,2,2\" name=\"a\" inreplyto=\"b\"/>"
    "<text page=\"0\" rect=\"1,1,2,2\" name=\"b\" inreplyto=\"a\"/></annots></xfdf>",
    "<xfdf><annots><text page=\"0\" rect=\"1,1,2,2\" name=\"a\" inreplyto=\"a\"/></annots></xfdf>",
    /* A good one before a bad one: all or nothing. */
    "<xfdf><annots><text page=\"0\" rect=\"1,1,2,2\"/><text page=\"99\" rect=\"1,1,2,2\"/></annots></xfdf>",
    "<xfdf><annots><text page=\"0\" rect=\"1,1,2,2\"/><stamp page=\"0\" rect=\"1,1,2,2\" icon=\"NoSuch\"/></annots></xfdf>",
    /* No kind of that name. */
    "<xfdf><annots><zebkit-annot kind=\"Nobody\" page=\"0\" rect=\"1,1,2,2\"/></annots></xfdf>",
};

static void test_interchange(void) {
    zebkit_pdf *pdf = blank(300, 300);
    zebkit_pdf_page_insert(pdf, 1, 300, 300, NULL);
    char *error = NULL;
    add(pdf, 0, "{\"type\":\"highlight\",\"rect\":[10,10,100,30],\"color\":[255,255,0],\"opacity\":0.5,"
                "\"quads\":[[10,30,100,30,10,10,100,10]],\"contents\":\"mark <&> \\\"this\\\"\",\"author\":\"An Author\",\"name\":\"h1\"}");
    add(pdf, 0, "{\"type\":\"text\",\"rect\":[120,10,140,30],\"icon\":\"Comment\",\"contents\":\"reply\",\"inReplyTo\":0,"
                "\"replyType\":\"reply\",\"name\":\"t1\",\"state\":\"accepted\",\"stateModel\":\"review\"}");
    add(pdf, 0, "{\"type\":\"line\",\"line\":[10,50,200,50],\"color\":[255,0,0],\"border\":2,\"lineEndings\":[\"none\",\"closedArrow\"],"
                "\"leader\":{\"length\":10,\"extension\":1,\"offset\":1},\"contents\":\"cap\",\"caption\":{\"show\":true,\"position\":\"top\"}}");
    add(pdf, 0, "{\"type\":\"square\",\"rect\":[10,100,100,190],\"color\":[0,0,255],\"interiorColor\":[0,255,0],"
                "\"borderStyle\":{\"style\":\"dashed\",\"width\":3,\"dash\":[4,2]},\"rotation\":90}");
    add(pdf, 0, "{\"type\":\"circle\",\"rect\":[120,100,200,190],\"color\":[0,0,255],\"borderEffect\":{\"style\":\"cloudy\",\"intensity\":1}}");
    add(pdf, 0, "{\"type\":\"polygon\",\"vertices\":[[210,100],[290,100],[250,190]],\"color\":[0,0,0],\"interiorColor\":[200,0,0]}");
    add(pdf, 0, "{\"type\":\"ink\",\"rect\":[10,200,100,290],\"color\":[0,0,0],\"border\":2,\"ink\":[[[10,200],[100,290]],[[10,290],[100,200]]]}");
    add(pdf, 0, "{\"type\":\"freeText\",\"rect\":[120,200,290,290],\"contents\":\"said\",\"fontSize\":14,\"textColor\":[255,0,0],"
                "\"align\":\"center\",\"callout\":[110,195,120,240],\"calloutEnding\":\"openArrow\"}");
    add(pdf, 1, "{\"type\":\"stamp\",\"rect\":[10,10,200,100],\"stamp\":\"Approved\","
                "\"popup\":{\"rect\":[210,10,290,100],\"open\":true}}");
    add(pdf, 1, "{\"type\":\"caret\",\"rect\":[10,200,30,220],\"caretSymbol\":\"paragraph\"}");
    add(pdf, 1, "{\"type\":\"line\",\"line\":[50,200,250,200],\"measure\":{\"unit\":\"mm\",\"scale\":0.5,\"precision\":1}}");

    shot page0 = render(pdf, 0), page1 = render(pdf, 1);
    /* Through each format, onto an empty document of the same pages. */
    for (int round = 0; round < 2; ++round) {
        const zebkit_pdf_interchange format = round ? ZEBKIT_PDF_FDF : ZEBKIT_PDF_XFDF;
        const char *name = round ? "FDF" : "XFDF";
        size_t size = 0;
        uint8_t *file = zebkit_pdf_annotations_export(pdf, format, -1, -1, &size, &error);
        check_text(file && size > 100, name, error ? error : "written");
        zebkit_free(error);
        error = NULL;
        zebkit_pdf *copy = blank(300, 300);
        zebkit_pdf_page_insert(copy, 1, 300, 300, NULL);
        const int count = zebkit_pdf_annotations_import(copy, format, file, size, &error);
        check_text(count == 11, name, error ? error : "read: every annotation, not the popups");
        zebkit_free(error);
        error = NULL;
        shot back0 = render(copy, 0), back1 = render(copy, 1);
        /* Same places and colours; the drawing is the same generator's. */
        int differ = 0;
        for (int i = 0; i < back0.width * back0.height * 4; ++i) {
            if (abs(back0.rgba[i] - page0.rgba[i]) > 40)
                differ++;
        }
        char detail[80];
        snprintf(detail, sizeof(detail), "%s: %d channels differ", name, differ);
        check_text(differ == 0, "the first page's drawing comes back", detail);
        differ = 0;
        for (int i = 0; i < back1.width * back1.height * 4; ++i) {
            if (abs(back1.rgba[i] - page1.rgba[i]) > 40)
                differ++;
        }
        snprintf(detail, sizeof(detail), "%s: %d channels differ", name, differ);
        check_text(differ == 0, "the second page's drawing comes back", detail);
        done(&back0);
        done(&back1);
        check_text(listed(copy, 0, "mark \\u003C&> \\\"this\\\"") && listed(copy, 0, "\"author\":\"An Author\"") && listed(copy, 0, "\"name\":\"h1\""),
              "text and names, with what XML escapes", name);
        check_text(listed(copy, 0, "\"replies\":[1]") && listed(copy, 0, "\"state\":\"accepted\"") && listed(copy, 0, "\"icon\":\"Comment\""),
              "the discussion", name);
        check_text(listed(copy, 1, "\"unit\":\"mm\""), "a measurement", name);
        if (!round)
            check_text(listed(copy, 1, "\"popup\":"), "a popup", name);
        check_text(listed(copy, 0, "\"rotation\":90"), "rotation", name);
        zebkit_pdf *saved = reopened(copy);
        check_text(saved && listed(saved, 0, "\"name\":\"h1\""), "and a save", name);
        zebkit_pdf_close(saved);
        zebkit_free(file);
        /* One annotation, and a page. */
        file = zebkit_pdf_annotations_export(pdf, format, 1, 2, &size, NULL);
        zebkit_pdf *one = blank(300, 300);
        zebkit_pdf_page_insert(one, 1, 300, 300, NULL);
        check_text(file && zebkit_pdf_annotations_import(one, format, file, size, NULL) == 1 && count_annotations(one, 1) == 1 &&
                  listed(one, 1, "\"type\":\"caret\""),
              "one annotation exported", name);
        zebkit_free(file);
        zebkit_pdf_close(one);
        zebkit_pdf_close(copy);
    }
    refused(!zebkit_pdf_annotations_export(pdf, ZEBKIT_PDF_XFDF, 99, -1, NULL, &error), &error, "export of no page");
    refused(!zebkit_pdf_annotations_export(pdf, ZEBKIT_PDF_XFDF, 0, 99, NULL, &error), &error, "export of no annotation");
    refused(!zebkit_pdf_annotations_export(pdf, ZEBKIT_PDF_XFDF, -1, 0, NULL, &error), &error, "an annotation of no page");
    done(&page0);
    done(&page1);
    zebkit_pdf_close(pdf);

    /* What another writer's XFDF looks like, in its own namespace. */
    pdf = blank(300, 300);
    static const char acrobat[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">"
        "<annots><square color=\"#FF0000\" flags=\"print\" name=\"a1\" page=\"0\" rect=\"10,10,110,110\" title=\"Reader\" "
        "width=\"2\" interior-color=\"#00FF00\"><contents>hello</contents></square>"
        "<text page=\"0\" rect=\"200,200,220,220\" icon=\"Help\" inreplyto=\"a1\" replyType=\"reply\"/></annots>"
        "<f href=\"x.pdf\"/></xfdf>";
    check_text(zebkit_pdf_annotations_import(pdf, ZEBKIT_PDF_XFDF, acrobat, sizeof(acrobat) - 1, &error) == 2, "another writer's XFDF", error);
    zebkit_free(error);
    error = NULL;
    shot s = render(pdf, 0);
    check_text(near_color(at(&s, 60, 60), 0, 255, 0) && near_color(at(&s, 11, 60), 255, 0, 0), "drawn", NULL);
    check_text(listed(pdf, 0, "\"replies\":[1]"), "the reply by name", NULL);
    done(&s);
    zebkit_pdf_close(pdf);

    /* Hostile files fail, and leave nothing. */
    pdf = blank(300, 300);
    for (size_t i = 0; i < sizeof(hostile_xfdf) / sizeof(hostile_xfdf[0]); ++i) {
        const int count = zebkit_pdf_annotations_import(pdf, ZEBKIT_PDF_XFDF, hostile_xfdf[i], strlen(hostile_xfdf[i]), &error);
        refused(count < 0, &error, hostile_xfdf[i]);
        check_text(count_annotations(pdf, 0) == 0, "and nothing is left", NULL);
    }
    static const char *const hostile_fdf[] = {
        "garbage",
        "%FDF-1.2\n",
        "%FDF-1.2\n1 0 obj\n<</FDF <</Annots 7>>>>\nendobj\ntrailer\n<</Root 1 0 R>>\n%%EOF\n",
        "%FDF-1.2\n1 0 obj\n<</FDF <</Annots [2 0 R]>>>>\nendobj\n2 0 obj\n5\nendobj\ntrailer\n<</Root 1 0 R>>\n%%EOF\n",
        "%FDF-1.2\n1 0 obj\n<</FDF <</Annots [2 0 R]>>>>\nendobj\n2 0 obj\n<</Subtype/Nonsense/Rect[1 1 2 2]/Page 0>>\nendobj\ntrailer\n<</Root 1 0 R>>\n%%EOF\n",
        "%FDF-1.2\n1 0 obj\n<</FDF <</Annots [2 0 R]>>>>\nendobj\n2 0 obj\n<</Subtype/Text/Rect[1 1 2 2]/Page 99>>\nendobj\ntrailer\n<</Root 1 0 R>>\n%%EOF\n",
        "%FDF-1.2\n1 0 obj\n<</FDF <</Annots [2 0 R]>>>>\nendobj\n2 0 obj\n<</Subtype/Text/Rect 5/Page 0>>\nendobj\ntrailer\n<</Root 1 0 R>>\n%%EOF\n",
        /* A reference to itself: a cycle in the objects. */
        "%FDF-1.2\n1 0 obj\n<</FDF <</Annots [2 0 R]>>>>\nendobj\n2 0 obj\n<</Subtype/Text/Page 0/Rect[1 1 2 2]/IRT 2 0 R>>\nendobj\ntrailer\n<</Root 1 0 R>>\n%%EOF\n",
    };
    for (size_t i = 0; i < sizeof(hostile_fdf) / sizeof(hostile_fdf[0]); ++i) {
        const int count = zebkit_pdf_annotations_import(pdf, ZEBKIT_PDF_FDF, hostile_fdf[i], strlen(hostile_fdf[i]), &error);
        refused(count < 0, &error, hostile_fdf[i]);
        check_text(count_annotations(pdf, 0) == 0, "and nothing is left", NULL);
    }
    /* An action in FDF is not taken over. */
    static const char with_action[] =
        "%FDF-1.2\n1 0 obj\n<</FDF <</Annots [2 0 R]>>>>\nendobj\n2 0 obj\n<</Subtype/Text/Page 0/Rect[1 1 20 20]"
        "/A <</S/Launch/F(calc.exe)>>/AA <</O <</S/JavaScript/JS(app.alert)>>>>>>\nendobj\ntrailer\n<</Root 1 0 R>>\n%%EOF\n";
    check_text(zebkit_pdf_annotations_import(pdf, ZEBKIT_PDF_FDF, with_action, sizeof(with_action) - 1, NULL) == 1 &&
              !listed(pdf, 0, "calc.exe") && !listed(pdf, 0, "app.alert"),
          "FDF: actions left behind", NULL);
    /* Too large, empty. */
    refused(zebkit_pdf_annotations_import(pdf, ZEBKIT_PDF_XFDF, "", 0, &error) < 0, &error, "an empty file");
    {
        const size_t huge = (64u << 20) + 1;
        char *big = calloc(1, huge);
        refused(zebkit_pdf_annotations_import(pdf, ZEBKIT_PDF_XFDF, big, huge, &error) < 0, &error, "over 64 MiB");
        free(big);
    }
    zebkit_pdf_close(pdf);
}

static void test_malformed(void) {
    zebkit_pdf *pdf = blank(300, 300);
    add(pdf, 0, "{\"type\":\"line\",\"line\":[10,10,20,20]}");
    add(pdf, 0, "{\"type\":\"freeText\",\"rect\":[50,50,100,100],\"contents\":\"x\"}");
    add(pdf, 0, "{\"type\":\"stamp\",\"rect\":[150,150,200,200],\"stamp\":\"Draft\"}");
    char *before = listing(pdf, 0);
    static const struct {
        int index;
        const char *json;
    } wrong[] = {
        {0, "{\"lineEndings\":\"openArrow\"}"},
        {0, "{\"lineEndings\":[\"openArrow\"]}"},
        {0, "{\"lineEndings\":[\"openArrow\",\"nope\"]}"},
        {0, "{\"lineEndings\":[1,2]}"},
        {1, "{\"lineEndings\":[\"none\",\"none\"]}"},
        {0, "{\"leader\":5}"},
        {0, "{\"leader\":{\"length\":\"far\"}}"},
        {0, "{\"leader\":{\"extension\":-1}}"},
        {0, "{\"leader\":{\"nothing\":1}}"},
        {1, "{\"leader\":{\"length\":1}}"},
        {0, "{\"caption\":true}"},
        {0, "{\"caption\":{\"position\":\"under\"}}"},
        {0, "{\"caption\":{\"offset\":[1]}}"},
        {0, "{\"borderStyle\":\"dashed\"}"},
        {0, "{\"borderStyle\":{\"style\":\"wavy\"}}"},
        {0, "{\"borderStyle\":{\"width\":-1}}"},
        {0, "{\"borderStyle\":{\"width\":1e9}}"},
        {0, "{\"borderStyle\":{\"dash\":[]}}"},
        {0, "{\"borderStyle\":{\"dash\":[-1,2]}}"},
        {0, "{\"borderEffect\":{\"style\":\"fuzzy\"}}"},
        {0, "{\"borderEffect\":{\"style\":\"cloudy\",\"intensity\":99}}"},
        {0, "{\"blend\":\"Nope\"}"},
        {0, "{\"blend\":5}"},
        {0, "{\"rotation\":\"left\"}"},
        {0, "{\"rotation\":1e300}"},
        {0, "{\"font\":\"Helvetica\"}"},
        {1, "{\"font\":5}"},
        {1, "{\"fontSize\":-2}"},
        {1, "{\"fontSize\":100000}"},
        {1, "{\"textColor\":\"red\"}"},
        {1, "{\"textColor\":[1,2]}"},
        {1, "{\"align\":\"middle\"}"},
        {1, "{\"richText\":5}"},
        {1, "{\"callout\":[1,2,3]}"},
        {1, "{\"callout\":\"x\"}"},
        {1, "{\"calloutEnding\":\"x\"}"},
        {0, "{\"callout\":[1,2,3,4]}"},
        {2, "{\"stamp\":\"NoSuch\"}"},
        {2, "{\"stamp\":5}"},
        {2, "{\"stampText\":5}"},
        {0, "{\"stamp\":\"Draft\"}"},
        {0, "{\"icon\":\"Key\"}"},
        {0, "{\"caretSymbol\":\"none\"}"},
        {0, "{\"intent\":\"a b\"}"},
        {0, "{\"inReplyTo\":\"x\"}"},
        {0, "{\"popup\":\"x\"}"},
        {0, "{\"data\":{}}"},
        {0, "{\"rect\":[1,2,3]}"},
        {0, "{\"rect\":[1e30,1,2,3]}"},
        {0, "{\"nothing\":1}"},
        {0, "{\"entries\":{\"AP\":{}}}"},
        {0, "{\"entries\":{\"Subtype\":\"/Text\"}}"},
        {0, "{\"entries\":{\"IRT\":{\"ref\":999999}}}"},
    };
    for (size_t i = 0; i < sizeof(wrong) / sizeof(wrong[0]); ++i) {
        char *error = NULL;
        const bool ok = zebkit_pdf_annotation_update(pdf, 0, wrong[i].index, wrong[i].json, &error);
        refused(!ok, &error, wrong[i].json);
    }
    char *after = listing(pdf, 0);
    check_text(after && before && !strcmp(before, after), "failed changes leave it as it was", NULL);
    zebkit_free(before);
    zebkit_free(after);
    char *error = NULL;
    refused(zebkit_pdf_annotation_add(pdf, 0, "not json", &error) < 0, &error, "not JSON");
    refused(zebkit_pdf_annotation_add(pdf, 0, "[]", &error) < 0, &error, "not an object");
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":5,\"rect\":[1,1,2,2]}", &error) < 0, &error, "a type that is no string");
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"widget\",\"rect\":[1,1,2,2]}", &error) < 0, &error, "a type not made");
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\"}", &error) < 0, &error, "no rect");
    refused(zebkit_pdf_annotation_add(pdf, 9, "{\"type\":\"square\",\"rect\":[1,1,2,2]}", &error) < 0, &error, "no page");
    refused(zebkit_pdf_annotation_add(pdf, -1, "{\"type\":\"square\",\"rect\":[1,1,2,2]}", &error) < 0, &error, "a negative page");
    check_text(count_annotations(pdf, 0) == 3, "nothing was added", NULL);
    refused(!zebkit_pdf_annotation_update(pdf, 0, 99, "{}", &error), &error, "no such annotation");
    refused(!zebkit_pdf_annotation_remove(pdf, 0, 99, &error), &error, "no such annotation to remove");
    refused(!zebkit_pdf_annotation_draw(pdf, 0, 2, "[[\"nothing\"]]", &error), &error, "a wrong drawing");
    refused(!zebkit_pdf_annotation_draw(pdf, 0, 99, "[]", &error), &error, "a drawing of nothing");
    zebkit_pdf_close(pdf);
}

/* A file of one page with @annots in its /Annots, and the objects @more. */
static size_t page_with(char *out, size_t room, const char *annots, const char *more) {
    return (size_t)snprintf(out, room,
                            "%%PDF-1.7\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
                            "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
                            "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Annots [%s] >> endobj\n"
                            "%strailer << /Root 1 0 R >>\n%%%%EOF\n",
                            annots, more);
}

static void test_hostile(void) {
    char *error = NULL;
    /* A measurement too long to write. */
    zebkit_pdf *pdf = blank(100, 100);
    add(pdf, 0, "{\"type\":\"line\",\"rect\":[0,0,9,9],\"entries\":{\"L\":[0,0,3e38,0],"
                "\"Measure\":{\"X\":[{\"C\":3e38,\"U\":\"m\"}]}}}");
    check_text(listed(pdf, 0, "\"text\":\"1000000000000000000.00 m\""), "a number too long to write", NULL);

    /* What a file says in /CO is not known to be numbers. */
    add(pdf, 0, "{\"type\":\"line\",\"line\":[1,1,9,9],\"entries\":{\"CO\":[\"/x\",\"(y\"]}}");
    size_t size = 0;
    zebkit_free(zebkit_pdf_annotations_export(pdf, ZEBKIT_PDF_XFDF, 0, -1, &size, NULL));
    check_text(size > 0, "an offset that is no numbers", NULL);
    zebkit_pdf_close(pdf);

    /* /RC as Acrobat writes it goes through XFDF. */
    pdf = blank(100, 100);
    add(pdf, 0, "{\"type\":\"freeText\",\"rect\":[1,1,99,99],\"contents\":\"hi\",\"richText\":\"<?xml version=\\\"1.0\\\"?>"
                "<body xmlns=\\\"http://www.w3.org/1999/xhtml\\\"><p>hi</p></body>\"}");
    uint8_t *xfdf = zebkit_pdf_annotations_export(pdf, ZEBKIT_PDF_XFDF, -1, -1, &size, NULL);
    zebkit_pdf *other = blank(100, 100);
    const int read = zebkit_pdf_annotations_import(other, ZEBKIT_PDF_XFDF, xfdf, size, &error);
    check_text(read == 1 && listed(other, 0, "\\u003Cp>hi\\u003C/p>"), "rich text with its declaration, read back", error);
    zebkit_free(error);
    error = NULL;
    zebkit_free(xfdf);
    zebkit_pdf_close(other);
    zebkit_pdf_close(pdf);

    /* The longest chain of replies a file can have, ending in a cycle. */
    enum { kChain = 60000 };
    char *chain = malloc((size_t)kChain * 64 + 64);
    size_t length = (size_t)sprintf(chain, "<xfdf><annots>");
    for (int i = 0; i < kChain; ++i) {
        length += (size_t)sprintf(chain + length, "<text page=\"0\" rect=\"1,1,2,2\" name=\"n%d\" inreplyto=\"n%d\"/>",
                                  i, i + 1 < kChain ? i + 1 : kChain - 2);
    }
    length += (size_t)sprintf(chain + length, "</annots></xfdf>");
    pdf = blank(100, 100);
    check_text(zebkit_pdf_annotations_import(pdf, ZEBKIT_PDF_XFDF, chain, length, &error) < 0 && error && strstr(error, "cycle"),
          "a long chain is judged at once", error);
    zebkit_free(error);
    error = NULL;
    free(chain);

    /* A file's references are to its own objects, not this document's: 1 is its catalog. */
    static const char fdf[] =
        "%FDF-1.2\n1 0 obj\n<</FDF <</Annots [2 0 R]>>>>\nendobj\n"
        "2 0 obj\n<</Subtype /Text /Rect [1 1 2 2] /Page 0 /Measure 1 0 R /Foo [1 0 R]>>\nendobj\n"
        "trailer\n<</Root 1 0 R>>\n%%EOF\n";
    check_text(zebkit_pdf_annotations_import(pdf, ZEBKIT_PDF_FDF, fdf, sizeof(fdf) - 1, &error) == 1, "an FDF with references", error);
    zebkit_free(error);
    error = NULL;
    check_text(!listed(pdf, 0, "Catalog") && !listed(pdf, 0, "\"ref\""), "they do not reach this document", NULL);
    static const char with_ref[] =
        "<xfdf><annots><text page=\"0\" rect=\"1,1,2,2\"><zebkit-entries>{\"Measure\":{\"ref\":1}}</zebkit-entries></text></annots></xfdf>";
    check_text(zebkit_pdf_annotations_import(pdf, ZEBKIT_PDF_XFDF, with_ref, sizeof(with_ref) - 1, NULL) == 1 && !listed(pdf, 0, "Catalog"),
          "nor from XFDF", NULL);
    zebkit_pdf_close(pdf);

    /* Arrays of references to arrays of references: small to write, 200^8 to read. */
    enum { kWidth = 200 };
    char *objects = malloc(64 * 1024);
    length = 0;
    for (int level = 0; level < 8; ++level) {
        length += (size_t)sprintf(objects + length, "%d 0 obj [", 10 + level);
        for (int i = 0; i < kWidth; ++i)
            length += (size_t)sprintf(objects + length, "%d 0 R ", 11 + level);
        length += (size_t)sprintf(objects + length, "] endobj\n");
    }
    length += (size_t)sprintf(objects + length, "18 0 obj 1 endobj\n"
                                                "4 0 obj << /Type /Annot /Subtype /Square /Rect [1 1 9 9] /Foo 10 0 R >> endobj\n");
    char *file = malloc(128 * 1024);
    size_t file_size = page_with(file, 128 * 1024, "4 0 R", objects);
    pdf = zebkit_pdf_open(file, file_size, NULL, &error);
    check_text(pdf != NULL, "the small file opens", error);
    zebkit_free(error);
    error = NULL;
    char *json = zebkit_pdf_annotations(pdf, 0, &error);
    check_text(json && strstr(json, "\"Foo\":[") && strstr(json, "\"ref\":"), "read, as far as is reasonable", error);
    zebkit_free(json);
    zebkit_free(error);
    error = NULL;
#if defined(__linux__)
    /* The whole test so far, this file's reading the largest of it: not tens of GiB. */
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    char peak[64];
    snprintf(peak, sizeof(peak), "peak %ld MiB", usage.ru_maxrss / 1024);
    check_text(usage.ru_maxrss / 1024 < 2048, "and within its memory", peak);
#endif
    zebkit_pdf_close(pdf);
    free(file);
    free(objects);

    /* A chain of replies against the order of the page: each round finds one. */
    enum { kReplies = 1500 };
    objects = malloc((size_t)kReplies * 128);
    char *annots = malloc((size_t)kReplies * 16);
    length = 0;
    size_t annots_length = 0;
    for (int i = 0; i < kReplies; ++i) {
        annots_length += (size_t)sprintf(annots + annots_length, "%d 0 R ", 10 + i);
        length += (size_t)sprintf(objects + length, "%d 0 obj << /Type /Annot /Subtype /Text /Rect [1 1 2 2]", 10 + i);
        if (i + 1 < kReplies)
            length += (size_t)sprintf(objects + length, " /IRT %d 0 R", 11 + i);
        length += (size_t)sprintf(objects + length, " >> endobj\n");
    }
    file = malloc((size_t)kReplies * 160 + 512);
    file_size = page_with(file, (size_t)kReplies * 160 + 512, annots, objects);
    pdf = zebkit_pdf_open(file, file_size, NULL, NULL);
    check_text(zebkit_pdf_annotation_remove(pdf, 0, kReplies - 1, &error) && count_annotations(pdf, 0) == 0, "the whole thread goes", error);
    zebkit_free(error);
    error = NULL;
    zebkit_pdf_close(pdf);
    free(file);
    free(annots);
    free(objects);

    /* A change that fails leaves what it cannot describe: the file, and no popup. */
    pdf = blank(100, 100);
    const int attached = add(pdf, 0, "{\"type\":\"fileAttachment\",\"rect\":[1,1,9,9]}");
    check_text(zebkit_pdf_annotation_attach_file(pdf, 0, attached, "a.txt", "file", 4, NULL), "attached", NULL);
    refused(!zebkit_pdf_annotation_update(pdf, 0, attached, "{\"popup\":{\"rect\":[1,1,2,2]},\"zz\":1}", &error), &error, "an unknown key");
    check_text(count_annotations(pdf, 0) == 1, "no popup stays", NULL);
    size_t got = 0;
    uint8_t *bytes = zebkit_pdf_annotation_file(pdf, 0, attached, NULL, &got, NULL);
    check_text(bytes && got == 4 && !memcmp(bytes, "file", 4), "the file stays after a change that failed", NULL);
    zebkit_free(bytes);
    zebkit_pdf_close(pdf);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: pdf_annotations <font.ttf>\n");
        return 2;
    }
#if defined(__linux__)
    /* A file that asks for too much ends this test, not the machine. */
    const struct rlimit data = {(rlim_t)4 << 30, (rlim_t)4 << 30};
    setrlimit(RLIMIT_DATA, &data);
#endif
    font_bytes = read_file(argv[1], &font_size);
    check_text(font_bytes != NULL, "the font is there", argv[1]);
    test_line_endings();
    test_leader_and_caption();
    test_shape_borders();
    test_free_text();
    test_stamps_and_the_rest();
    test_discussion();
    test_measurement();
    test_viewers();
    test_interchange();
    test_malformed();
    test_hostile();
    free(font_bytes);
    return summary();
}
