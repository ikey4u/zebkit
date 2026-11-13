/* <zebkit/pdf.h> without the engine running: a document made here, opened,
 * read, rendered, searched, changed, saved and opened again. Prints one
 * line per check; exit 0 when all pass. */

#include "harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/pdf.h>
#include <zebkit/services.h>


/* Two pages: "Hello zebkit" and a link out, and "Second page zebkit". */
static const char document[] =
    "%PDF-1.7\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R /Lang (en-US) >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Contents 4 0 R"
    " /Resources << /Font << /F1 5 0 R >> >> /Annots [8 0 R] >> endobj\n"
    "4 0 obj << >> stream\n"
    "BT /F1 24 Tf 20 120 Td (Hello zebkit) Tj ET\n"
    "endstream endobj\n"
    "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
    "6 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Rotate 90 /Contents 7 0 R"
    " /Resources << /Font << /F1 5 0 R >> >> >> endobj\n"
    "7 0 obj << >> stream\n"
    "BT /F1 18 Tf 20 120 Td (Second page zebkit) Tj ET\n"
    "endstream endobj\n"
    "8 0 obj << /Type /Annot /Subtype /Link /Rect [20 20 120 40]"
    " /A << /S /URI /URI (https://example.com/) >> >> endobj\n"
    "trailer << /Root 1 0 R >>\n"
    "%%EOF\n";

/* "two-" at the end of a line and "dimensional" at the start of the next,
 * which PDFium joins with a hyphen of its own (U+0002 inside it). */
static const char hyphenated[] =
    "%PDF-1.7\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Contents 4 0 R"
    " /Resources << /Font << /F1 5 0 R >> >> >> endobj\n"
    "4 0 obj << >> stream\n"
    "BT /F1 12 Tf 20 150 Td (the one two-) Tj 0 -14 Td (dimensional array) Tj ET\n"
    "endstream endobj\n"
    "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
    "trailer << /Root 1 0 R >>\n"
    "%%EOF\n";

/* A page that draws a 2 by 2 RGB image over (50, 50)-(150, 150). */
static const char picture[] =
    "%PDF-1.7\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R"
    " /Resources << /XObject << /Im1 5 0 R >> >> >> endobj\n"
    "4 0 obj << /Length 32 >> stream\nq 100 0 0 100 50 50 cm /Im1 Do Q\nendstream endobj\n"
    "5 0 obj << /Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB"
    " /BitsPerComponent 8 /Length 12 >> stream\n"
    "\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\xff"
    "\nendstream endobj\n"
    "trailer << /Root 1 0 R >>\n"
    "%%EOF\n";

/* A form whose signature field is nested ("Signatures.Approval") and takes
 * /FT /Sig from its parent, holding a detached PKCS#7 by "zebkit test
 * signer" (serial 1234). It is read, not verified: its byte range is made
 * up. */
static const char signed_form[] =
    "%PDF-1.7\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [5 0 R] /SigFlags 3 >> >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots [6 0 R] >> endobj\n"
    "5 0 obj << /T (Signatures) /FT /Sig /Kids [6 0 R] >> endobj\n"
    "6 0 obj << /Type /Annot /Subtype /Widget /T (Approval) /Parent 5 0 R /Rect [0 0 0 0] /P 3 0 R"
    " /V 7 0 R >> endobj\n"
    "7 0 obj << /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached /Reason (Approved)"
    " /M (D:20261021120000Z) /ByteRange [0 100 200 300] /Contents <"
    "3082024706092a864886f70d010702a082023830820234020101310d300b0609608648016503"
    "040201300b06092a864886f70d010701a08201813082017d30820123a0030201020202123430"
    "0a06082a8648ce3d040302301d311b301906035504030c127a65626b69742074657374207369"
    "676e6572301e170d3236313032313138323335335a170d3336313031383138323335335a301d"
    "311b301906035504030c127a65626b69742074657374207369676e65723059301306072a8648"
    "ce3d020106082a8648ce3d03010703420004c15b0ce1abe03a69f1bcdceb9a8c83bdbb996c8b"
    "d98ce891da7647058833b9e7289496db00be938af9beec316cd2e400fcccbc653df51d53d6f3"
    "986a11652ec6a3533051301d0603551d0e04160414fdb9a5b35a61f2729dd842a3812395c490"
    "99fa76301f0603551d23041830168014fdb9a5b35a61f2729dd842a3812395c49099fa76300f"
    "0603551d130101ff040530030101ff300a06082a8648ce3d040302034800304502205787a508"
    "83bf4aecde44154b2302122511ed358ad87e5ae449d115c823a1913b022100b343fcb75a93b7"
    "a94f10f168b60c518477412e0a5715b67e392a3ca5a1de842231818d30818a0201013023301d"
    "311b301906035504030c127a65626b69742074657374207369676e657202021234300b060960"
    "8648016503040201300a06082a8648ce3d04030204473045022064f927c54ed44be5f7bdde50"
    "9d96f81e86f6a11cfa1b0aa0ddd03a3737d69a25022100c0266d3fda7fcf1ae705d1bf84d2dc"
    "92a4bf7926cfb8a3b2b78c82d8b9466db8"
    "> >> endobj\n"
    "trailer << /Root 1 0 R >>\n"
    "%%EOF\n";

/* A form signed for real: its signature, by "zebkit test signer", covers
 * every byte of the file but itself (made with openssl cms). */
static const char really_signed[] =
    "%PDF-1.7\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] /SigFlags 3 >> >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots [4 0 R] >> endobj\n"
    "4 0 obj << /Type /Annot /Subtype /Widget /FT /Sig /T (Approval) /Rect [0 0 0 0] /P 3 0 R /V 5 0 R >>"
    " endobj\n"
    "5 0 obj << /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached /Reason (Approved) /M ("
    "D:20261021120000Z) /ByteRange [0 531        4629       43        ] /Contents <308202b206092a864886f7"
    "0d010702a08202a33082029f020101310d300b0609608648016503040201300b06092a864886f70d010701a0820181308201"
    "7d30820123a00302010202021234300a06082a8648ce3d040302301d311b301906035504030c127a65626b69742074657374"
    "207369676e6572301e170d3236313032313138323335335a170d3336313031383138323335335a301d311b30190603550403"
    "0c127a65626b69742074657374207369676e65723059301306072a8648ce3d020106082a8648ce3d03010703420004c15b0c"
    "e1abe03a69f1bcdceb9a8c83bdbb996c8bd98ce891da7647058833b9e7289496db00be938af9beec316cd2e400fcccbc653d"
    "f51d53d6f3986a11652ec6a3533051301d0603551d0e04160414fdb9a5b35a61f2729dd842a3812395c49099fa76301f0603"
    "551d23041830168014fdb9a5b35a61f2729dd842a3812395c49099fa76300f0603551d130101ff040530030101ff300a0608"
    "2a8648ce3d040302034800304502205787a50883bf4aecde44154b2302122511ed358ad87e5ae449d115c823a1913b022100"
    "b343fcb75a93b7a94f10f168b60c518477412e0a5715b67e392a3ca5a1de84223181f83081f50201013023301d311b301906"
    "035504030c127a65626b69742074657374207369676e657202021234300b0609608648016503040201a069301806092a8648"
    "86f70d010903310b06092a864886f70d010701301c06092a864886f70d010905310f170d3236313032313139313931385a30"
    "2f06092a864886f70d01090431220420efe93f184bf704616557259c93cb34f7f67e2e320df1a3e1a742ef85e8c53ecc300a"
    "06082a8648ce3d0403020447304502200f1a6574ffb584d5290d74535cb2c36c123a63a4522fa49b6c920ef4aa88828f0221"
    "00dbf64edd9d650b750a47e98c0e1d1e04cc4b7cad99f9ae0816393831da667a1b0000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000000000000000000000000000000000000000000>\n"
    ">> endobj\n"
    "trailer << /Root 1 0 R >>\n"
    "%%EOF\n";

/* The same, its signer named by its key identifier (openssl cms -keyid). */
static const char signed_by_key_id[] =
    "%PDF-1.7\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] /SigFlags 3 >> >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots [4 0 R] >> endobj\n"
    "4 0 obj << /Type /Annot /Subtype /Widget /FT /Sig /T (Approval) /Rect [0 0 0 0] /P 3 0 R /V 5 0 R >>"
    " endobj\n"
    "5 0 obj << /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached /Reason (Approved) /M ("
    "D:20261021120000Z) /ByteRange [0 531        4629       43        ] /Contents <308202a206092a864886f7"
    "0d010702a08202933082028f020103310d300b0609608648016503040201300b06092a864886f70d010701a0820181308201"
    "7d30820123a00302010202021234300a06082a8648ce3d040302301d311b301906035504030c127a65626b69742074657374"
    "207369676e6572301e170d3236313032313138323335335a170d3336313031383138323335335a301d311b30190603550403"
    "0c127a65626b69742074657374207369676e65723059301306072a8648ce3d020106082a8648ce3d03010703420004c15b0c"
    "e1abe03a69f1bcdceb9a8c83bdbb996c8bd98ce891da7647058833b9e7289496db00be938af9beec316cd2e400fcccbc653d"
    "f51d53d6f3986a11652ec6a3533051301d0603551d0e04160414fdb9a5b35a61f2729dd842a3812395c49099fa76301f0603"
    "551d23041830168014fdb9a5b35a61f2729dd842a3812395c49099fa76300f0603551d130101ff040530030101ff300a0608"
    "2a8648ce3d040302034800304502205787a50883bf4aecde44154b2302122511ed358ad87e5ae449d115c823a1913b022100"
    "b343fcb75a93b7a94f10f168b60c518477412e0a5715b67e392a3ca5a1de84223181e83081e50201038014fdb9a5b35a61f2"
    "729dd842a3812395c49099fa76300b0609608648016503040201a069301806092a864886f70d010903310b06092a864886f7"
    "0d010701301c06092a864886f70d010905310f170d3236313032313232303834385a302f06092a864886f70d010904312204"
    "20efe93f184bf704616557259c93cb34f7f67e2e320df1a3e1a742ef85e8c53ecc300a06082a8648ce3d0403020446304402"
    "20202803c161b5c96bb2b8fe24297b07a3abf095d84abcde23b25143c7e48d2c8902207eb7d103abfebefce2fbdc69c45bf1"
    "a85786dd433c69808b25193755ff85d5bd000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000000000000000000000000000000000000000000>\n"
    ">> endobj\n"
    "trailer << /Root 1 0 R >>\n"
    "%%EOF\n";

static int contains(const char *text, const char *part) {
    return text && strstr(text, part) != NULL;
}

/* A form: a text field with a value, and a check box with Yes and Off. */
static const char form[] =
    "%PDF-1.7\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R 7 0 R] /DA (/Helv 0 Tf 0 g)"
    " /DR << /Font << /Helv 8 0 R >> >> >> >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Annots [6 0 R 7 0 R] >> endobj\n"
    "6 0 obj << /Type /Annot /Subtype /Widget /FT /Tx /T (name) /V (Ada) /Rect [20 150 200 170] /P 3 0 R"
    " /DA (/Helv 12 Tf 0 g) /F 4 >> endobj\n"
    "7 0 obj << /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /V /Off /AS /Off /Rect [20 120 35 135] /P 3 0 R"
    " /F 4 /AP << /N << /Yes 9 0 R /Off 10 0 R >> >> >> endobj\n"
    "8 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
    "9 0 obj << /Type /XObject /Subtype /Form /BBox [0 0 15 15] >> stream\n0 0 0 rg 2 2 11 11 re f\nendstream endobj\n"
    "10 0 obj << /Type /XObject /Subtype /Form /BBox [0 0 15 15] >> stream\n\nendstream endobj\n"
    "trailer << /Root 1 0 R >>\n"
    "%%EOF\n";

static void test_redaction(void) {
    char *error = NULL;
    zebkit_pdf *pdf = zebkit_pdf_open(document, sizeof(document) - 1, NULL, &error);
    if (!pdf)
        return;
    /* A mark over "Hello", the first word of "Hello zebkit" (24 points, from x 20). */
    int mark = zebkit_pdf_annotation_add(
        pdf, 0, "{\"type\": \"redact\", \"rect\": [18, 112, 80, 148], \"quads\": [[18, 148, 80, 148, 18, 112, 80, 112]]}",
        &error);
    check_text(mark >= 0, "a redaction mark", error);
    int areas = zebkit_pdf_redact(pdf, 0, NULL, 0x000000ff, &error);
    char detail[64];
    snprintf(detail, sizeof(detail), "%d", areas);
    check_text(areas == 1, "zebkit_pdf_redact: the page's mark", error ? error : detail);
    char *text = zebkit_pdf_text(pdf, 0, &error);
    check_text(text && !strstr(text, "Hello") && strstr(text, "zebkit"), "the word under it is gone, the rest is there",
          text);
    zebkit_free(text);
    char *annotations = zebkit_pdf_annotations(pdf, 0, &error);
    check_text(annotations && !strstr(annotations, "redact"), "the mark is gone", annotations);
    zebkit_free(annotations);
    int width = 0, height = 0;
    uint8_t *pixels = zebkit_pdf_render(pdf, 0, NULL, &width, &height, &error);
    /* The middle of the area, in pixels from the top: x 50, y 200 - 130. */
    const uint8_t *middle = pixels ? pixels + ((size_t)(200 - 130) * width + 50) * 4 : NULL;
    check_text(middle && middle[0] < 20 && middle[1] < 20 && middle[2] < 20, "a black box over the area", NULL);
    zebkit_free(pixels);
    /* Saved and opened again, the word is not in the file's text. */
    size_t size = 0;
    uint8_t *saved = zebkit_pdf_save(pdf, 0, &size, &error);
    zebkit_pdf *again = saved ? zebkit_pdf_open(saved, size, NULL, &error) : NULL;
    text = again ? zebkit_pdf_text(again, 0, &error) : NULL;
    check_text(text && !strstr(text, "Hello") && strstr(text, "zebkit"), "and not in the saved document", text);
    zebkit_free(text);
    /* An area given, on the second page: "Second" goes, with no box. */
    areas = again ? zebkit_pdf_redact(again, 1, "[[15, 110, 80, 145]]", 0, &error) : -1;
    text = again ? zebkit_pdf_text(again, 1, &error) : NULL;
    check_text(areas == 1 && text && !strstr(text, "Second") && strstr(text, "page zebkit"), "zebkit_pdf_redact: an area",
          text);
    zebkit_free(text);
    zebkit_pdf_close(again);
    zebkit_free(saved);
    zebkit_pdf_close(pdf);
}

static void test_forms_and_pages(void) {
    char *error = NULL;
    zebkit_pdf *pdf = zebkit_pdf_open(form, sizeof(form) - 1, NULL, &error);
    check_text(pdf != NULL, "a document with a form", error);
    if (!pdf)
        return;
    char *fields = zebkit_pdf_form_fields(pdf, &error);
    check_text(contains(fields, "\"name\":\"name\"") && contains(fields, "\"type\":\"text\"") &&
              contains(fields, "\"value\":\"Ada\"") && contains(fields, "\"type\":\"checkbox\"") &&
              contains(fields, "\"exportValue\":\"Yes\"") && contains(fields, "\"checked\":false"),
          "zebkit_pdf_form_fields", fields);
    zebkit_free(fields);
    check_text(zebkit_pdf_form_set(pdf, "name", "Grace Hopper", &error), "zebkit_pdf_form_set: a text field", error);
    check_text(zebkit_pdf_form_set(pdf, "agree", "Yes", &error), "zebkit_pdf_form_set: a check box", error);
    check_text(!zebkit_pdf_form_set(pdf, "nobody", "x", &error) && contains(error, "no field"),
          "zebkit_pdf_form_set refuses a field that is not there", error);
    zebkit_free(error);
    error = NULL;
    fields = zebkit_pdf_form_fields(pdf, &error);
    check_text(contains(fields, "\"value\":\"Grace Hopper\"") && contains(fields, "\"checked\":true"),
          "the values are set", fields);
    zebkit_free(fields);
    /* The text drawn into the field's new appearance, with forms on. */
    zebkit_pdf_render_options options = {.scale = 1, .background = 0xffffffff, .flags = ZEBKIT_PDF_RENDER_FORMS};
    int width = 0, height = 0;
    uint8_t *pixels = zebkit_pdf_render(pdf, 0, &options, &width, &height, &error);
    int dark = 0;
    for (int y = 200 - 170; pixels && y < 200 - 150; y++)
        for (int x = 20; x < 200; x++)
            dark += pixels[(y * width + x) * 4] < 128;
    char detail[64];
    snprintf(detail, sizeof(detail), "%d dark pixels in the field", dark);
    check_text(dark > 30, "the field draws its new value", detail);
    zebkit_free(pixels);
    size_t size = 0;
    uint8_t *saved = zebkit_pdf_save(pdf, 0, &size, &error);
    check_text(zebkit_pdf_form_reset(pdf, &error), "zebkit_pdf_form_reset", error);
    fields = zebkit_pdf_form_fields(pdf, &error);
    check_text(!contains(fields, "Grace") && contains(fields, "\"checked\":false"), "the values are reset", fields);
    zebkit_free(fields);
    zebkit_pdf_close(pdf);
    zebkit_pdf *again = saved ? zebkit_pdf_open(saved, size, NULL, &error) : NULL;
    fields = again ? zebkit_pdf_form_fields(again, &error) : NULL;
    check_text(contains(fields, "Grace Hopper") && contains(fields, "\"checked\":true"), "the saved form keeps its values",
          fields);
    zebkit_free(fields);
    zebkit_free(saved);

    /* Pages: one blank page inserted, turned, moved, others copied in. */
    check_text(zebkit_pdf_page_insert(again, 1, 100, 50, &error), "zebkit_pdf_page_insert", error);
    check_text(zebkit_pdf_page_rotate(again, 1, 90, &error), "zebkit_pdf_page_rotate", error);
    char *page = zebkit_pdf_page(again, 1, &error);
    check_text(contains(page, "\"rotation\":90") && contains(page, "\"width\":50.0"), "the new page, turned", page);
    zebkit_free(page);
    int first[] = {1};
    check_text(zebkit_pdf_pages_move(again, first, 1, 0, &error), "zebkit_pdf_pages_move", error);
    page = zebkit_pdf_page(again, 0, &error);
    check_text(contains(page, "\"rotation\":90"), "the moved page is first", page);
    zebkit_free(page);
    zebkit_pdf *merged = zebkit_pdf_new(&error);
    check_text(merged && zebkit_pdf_pages_import(merged, again, NULL, 0, 0, &error) &&
              zebkit_pdf_page_count(merged) == 2,
          "zebkit_pdf_new and zebkit_pdf_pages_import, all pages", error);
    int second[] = {1};
    check_text(merged && zebkit_pdf_pages_import(merged, again, second, 1, 2, &error) &&
              zebkit_pdf_page_count(merged) == 3,
          "zebkit_pdf_pages_import, one page", error);
    int both[] = {0, 2};
    check_text(merged && zebkit_pdf_pages_delete(merged, both, 2, &error) && zebkit_pdf_page_count(merged) == 1,
          "zebkit_pdf_pages_delete", error);
    int bad[] = {5};
    check_text(!zebkit_pdf_pages_delete(merged, bad, 1, &error), "zebkit_pdf_pages_delete refuses a page not there",
          error);
    zebkit_free(error);
    error = NULL;
    saved = merged ? zebkit_pdf_save(merged, 0, &size, &error) : NULL;
    zebkit_pdf *built = saved ? zebkit_pdf_open(saved, size, NULL, &error) : NULL;
    check_text(built && zebkit_pdf_page_count(built) == 1, "the built document saved and opened", error);
    zebkit_pdf_close(built);
    zebkit_free(saved);
    zebkit_pdf_close(merged);
    zebkit_pdf_close(again);
}

int main(void) {
    char *error = NULL;
    zebkit_pdf *pdf = zebkit_pdf_open("not a pdf", 9, NULL, &error);
    check_text(pdf == NULL && error != NULL, "zebkit_pdf_open refuses what is not a PDF", error);
    zebkit_free(error);
    error = NULL;

    pdf = zebkit_pdf_open(document, sizeof(document) - 1, NULL, &error);
    check_text(pdf != NULL, "zebkit_pdf_open", error);
    if (!pdf)
        return 1;

    char *info = zebkit_pdf_info(pdf);
    check_text(contains(info, "\"pages\":2") && contains(info, "\"language\":\"en-US\"") &&
              contains(info, "\"encrypted\":false") && contains(info, "\"permissionBits\":"),
          "zebkit_pdf_info", info);
    zebkit_free(info);
    check_text(zebkit_pdf_page_count(pdf) == 2, "zebkit_pdf_page_count", NULL);

    char *page = zebkit_pdf_page(pdf, 1, &error);
    check_text(contains(page, "\"rotation\":90") && contains(page, "\"width\":200.0") && contains(page, "\"height\":300.0") &&
              contains(page, "\"mediaBox\":[0.0,0.0,300.0,200.0]") && contains(page, "\"objectNumber\":") &&
              !contains(page, "\"objectNumber\":0"),
          "zebkit_pdf_page", page);
    zebkit_free(page);
    page = zebkit_pdf_page(pdf, 2, &error);
    check_text(page == NULL && error != NULL, "zebkit_pdf_page refuses a page that is not there", error);
    zebkit_free(error);
    error = NULL;

    int width = 0, height = 0;
    uint8_t *pixels = zebkit_pdf_render(pdf, 0, NULL, &width, &height, &error);
    int dark = 0;
    for (int i = 0; pixels && i < width * height; i++)
        if (pixels[i * 4] < 128 && pixels[i * 4 + 3] == 255)
            dark++;
    char detail[96];
    snprintf(detail, sizeof(detail), "%dx%d, %d dark pixels", width, height, dark);
    check_text(pixels && width == 300 && height == 200 && dark > 200, "zebkit_pdf_render", detail);
    zebkit_free(pixels);

    /* Rotated by the page's own 90 degrees, at twice the size, on red. */
    zebkit_pdf_render_options options = {.scale = 2, .background = 0xff0000ff};
    pixels = zebkit_pdf_render(pdf, 1, &options, &width, &height, &error);
    snprintf(detail, sizeof(detail), "%dx%d, first pixel %d,%d,%d,%d", width, height, pixels ? pixels[0] : -1,
             pixels ? pixels[1] : -1, pixels ? pixels[2] : -1, pixels ? pixels[3] : -1);
    check_text(pixels && width == 400 && height == 600 && pixels[0] == 255 && pixels[1] == 0 && pixels[2] == 0 &&
              pixels[3] == 255,
          "zebkit_pdf_render: rotation, scale, background", detail);
    zebkit_free(pixels);

    /* A tile: the top left 50 by 40 pixels. */
    zebkit_pdf_render_options tile = {.scale = 1, .background = 0xffffffff, .x = 10, .y = 20, .width = 50, .height = 40};
    pixels = zebkit_pdf_render(pdf, 0, &tile, &width, &height, &error);
    check_text(pixels && width == 50 && height == 40, "zebkit_pdf_render: a tile", NULL);
    if (pixels) {
        size_t size = 0;
        uint8_t *png = zebkit_image_encode(ZEBKIT_IMAGE_PNG, pixels, width, height, 0, &size, NULL);
        check_text(png && size > 8, "the tile as PNG (zebkit_image_encode)", NULL);
        zebkit_free(png);
    }
    zebkit_free(pixels);

    char *text = zebkit_pdf_text(pdf, 0, &error);
    check_text(contains(text, "Hello zebkit"), "zebkit_pdf_text", text);
    zebkit_free(text);
    char *layout = zebkit_pdf_text_layout(pdf, 0, &error);
    check_text(contains(layout, "\"c\":\"H\"") && contains(layout, "\"font\":\"Helvetica\"") &&
              contains(layout, "\"size\":24.0") && contains(layout, "\"loose\":[") &&
              contains(layout, "\"object\":0") && contains(layout, "\"objects\":[{") &&
              contains(layout, "\"family\":\"") && contains(layout, "\"color\":[0,0,0,255]"),
          "zebkit_pdf_text_layout", layout);
    zebkit_free(layout);
    char *rects = zebkit_pdf_text_rects(pdf, 0, 0, 5, &error);
    check_text(rects && rects[0] == '[' && rects[1] == '[', "zebkit_pdf_text_rects", rects);
    zebkit_free(rects);
    int at = zebkit_pdf_char_at(pdf, 0, 25, 128, 2);
    snprintf(detail, sizeof(detail), "%d", at);
    check_text(at == 0, "zebkit_pdf_char_at: the H", detail);
    check_text(zebkit_pdf_char_at(pdf, 0, 280, 10, 1) == -1, "zebkit_pdf_char_at: nothing there", NULL);

    char *found = zebkit_pdf_search(pdf, "ZEBKIT", 0, 0, 0, &error);
    check_text(contains(found, "\"page\":0") && contains(found, "\"page\":1") && contains(found, "\"count\":6") &&
              contains(found, "Second page"),
          "zebkit_pdf_search, both pages", found);
    zebkit_free(found);
    found = zebkit_pdf_search(pdf, "ZEBKIT", ZEBKIT_PDF_SEARCH_CASE, 0, 0, &error);
    check_text(found && strcmp(found, "[]") == 0, "zebkit_pdf_search, matching case", found);
    zebkit_free(found);

    char *links = zebkit_pdf_links(pdf, 0, &error);
    check_text(contains(links, "\"url\":\"https://example.com/\"") && contains(links, "[20.0,20.0,120.0,40.0]"),
          "zebkit_pdf_links", links);
    zebkit_free(links);
    char *outline = zebkit_pdf_outline(pdf, &error);
    check_text(outline && strcmp(outline, "[]") == 0, "zebkit_pdf_outline, none", outline);
    zebkit_free(outline);

    /* Annotations: the link the document has, then one of each kind made. */
    char *annotations = zebkit_pdf_annotations(pdf, 0, &error);
    check_text(contains(annotations, "\"type\":\"link\"") && contains(annotations, "https://example.com/"),
          "zebkit_pdf_annotations: the link", annotations);
    zebkit_free(annotations);
    static const struct {
        const char *json;
        const char *what;
    } kinds[] = {
        {"{\"type\": \"highlight\", \"rect\": [18, 115, 160, 145], \"color\": [255, 230, 0],"
         " \"quads\": [[18, 145, 160, 145, 18, 115, 160, 115]], \"contents\": \"a note\", \"author\": \"zebkit\"}",
         "highlight"},
        {"{\"type\": \"ink\", \"rect\": [200, 100, 280, 180], \"color\": [0, 0, 255], \"border\": 3,"
         " \"ink\": [[[210, 110], [270, 170]], [[210, 170], [270, 110]]]}",
         "ink"},
        {"{\"type\": \"line\", \"rect\": [0, 0, 1, 1], \"color\": [255, 0, 0], \"border\": 2,"
         " \"line\": [20, 60, 280, 60]}",
         "line"},
        {"{\"type\": \"polygon\", \"rect\": [0, 0, 1, 1], \"color\": [0, 128, 0], \"interiorColor\": [0, 255, 0],"
         " \"border\": 1, \"vertices\": [[150, 10], [190, 10], [170, 45]]}",
         "polygon"},
        {"{\"type\": \"square\", \"rect\": [230, 10, 290, 50], \"color\": [0, 0, 0], \"border\": 1,"
         " \"opacity\": 0.5}",
         "square"},
        {"{\"type\": \"circle\", \"rect\": [100, 150, 130, 180], \"color\": [0, 0, 0], \"border\": 1}", "circle"},
        {"{\"type\": \"freeText\", \"rect\": [150, 150, 290, 190], \"contents\": \"free text\"}", "free text"},
        {"{\"type\": \"redact\", \"rect\": [20, 115, 80, 145],"
         " \"quads\": [[20, 145, 80, 145, 20, 115, 80, 115]]}",
         "redaction mark"},
        {"{\"type\": \"text\", \"rect\": [5, 170, 25, 190], \"contents\": \"sticky\"}", "note"},
    };
    int last = -1;
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        int index = zebkit_pdf_annotation_add(pdf, 0, kinds[i].json, &error);
        char what[64];
        snprintf(what, sizeof(what), "zebkit_pdf_annotation_add: %s", kinds[i].what);
        check_text(index > last, what, error);
        zebkit_free(error);
        error = NULL;
        last = index;
    }
    check_text(zebkit_pdf_annotation_add(pdf, 0, "{\"type\": \"sound\", \"rect\": [0, 0, 1, 1]}", &error) == -1 &&
              contains(error, "sound"),
          "zebkit_pdf_annotation_add refuses a sound", error);
    zebkit_free(error);
    error = NULL;
    annotations = zebkit_pdf_annotations(pdf, 0, &error);
    check_text(contains(annotations, "\"type\":\"highlight\"") && contains(annotations, "\"color\":[255,230,0]") &&
              contains(annotations, "\"contents\":\"a note\"") && contains(annotations, "\"author\":\"zebkit\"") &&
              contains(annotations, "\"quads\":[[18.0,145.0,160.0,145.0,18.0,115.0,160.0,115.0]]") &&
              contains(annotations, "\"ink\":[[[210.0,110.0],[270.0,170.0]]") &&
              contains(annotations, "\"line\":[20.0,60.0,280.0,60.0]") &&
              contains(annotations, "\"vertices\":[[150.0,10.0],[190.0,10.0],[170.0,45.0]]") &&
              contains(annotations, "\"opacity\":0.5") && contains(annotations, "\"modified\":\"D:") &&
              contains(annotations, "\"type\":\"circle\"") && contains(annotations, "\"contents\":\"free text\"") &&
              contains(annotations, "\"type\":\"redact\""),
          "zebkit_pdf_annotations: what was made", annotations);
    zebkit_free(annotations);

    /* What each draws: yellow under the text, blue ink, a red line, a
     * green polygon. */
    pixels = zebkit_pdf_render(pdf, 0, NULL, &width, &height, &error);
#define PIXEL(x, y) (pixels + ((size_t)(height - (y)) * width + (x)) * 4)
    int yellow = 0, blue = 0, red = 0, green = 0;
    for (int y = 1; pixels && y < height; y++)
        for (int x = 0; x < width; x++) {
            const uint8_t *p = PIXEL(x, y);
            if (p[0] > 200 && p[1] > 180 && p[2] < 80)
                yellow += y > 115 && y < 145;
            if (p[0] < 60 && p[1] < 60 && p[2] > 200)
                blue += x > 200 && y > 100;
            if (p[0] > 200 && p[1] < 60 && p[2] < 60)
                red += y >= 57 && y <= 63;
            if (p[0] < 60 && p[1] > 200 && p[2] < 60)
                green += y < 50 && x > 140 && x < 200;
        }
    snprintf(detail, sizeof(detail), "yellow %d, blue %d, red %d, green %d", yellow, blue, red, green);
    check_text(yellow > 1000 && blue > 50 && red > 200 && green > 200, "the annotations are drawn", detail);
    zebkit_free(pixels);

    /* Each one's appearance alone, as an annotation layer draws it. */
    annotations = zebkit_pdf_annotations(pdf, 0, &error);
    check_text(contains(annotations, "\"appearances\":[\"normal\"]") &&
              contains(annotations, "\"Subtype\":\"/Highlight\"") && contains(annotations, "\"QuadPoints\":["),
          "zebkit_pdf_annotations: appearances and entries", annotations);
    zebkit_free(annotations);
    zebkit_pdf_render_options twice = {.scale = 2};
    int w1 = 0, h1 = 0, w2 = 0, h2 = 0, opaque = 0;
    pixels = zebkit_pdf_render_annotation(pdf, 0, 1, 0, &twice, &w1, &h1, &error);
    for (size_t i = 0; pixels && i < (size_t)w1 * h1; i++)
        opaque += pixels[i * 4 + 3] > 0;
    zebkit_free(pixels);
    twice.rotation = 90;
    pixels = zebkit_pdf_render_annotation(pdf, 0, 1, 0, &twice, &w2, &h2, &error);
    zebkit_free(pixels);
    snprintf(detail, sizeof(detail), "%dx%d, turned %dx%d, %d drawn", w1, h1, w2, h2, opaque);
    check_text(w1 > 200 && h1 > 40 && w2 == h1 && h2 == w1 && opaque > w1 * h1 / 4,
          "zebkit_pdf_render_annotation", detail);
    pixels = zebkit_pdf_render_annotation(pdf, 0, 1, 2, NULL, &w1, &h1, &error);
    check_text(pixels == NULL && contains(error, "no appearance"),
          "zebkit_pdf_render_annotation: none for a mode it has not", error);
    zebkit_free(error);
    error = NULL;

    check_text(zebkit_pdf_annotation_update(pdf, 0, 1,
                                       "{\"entries\": {\"IT\": \"/HighlightNote\", \"ZNote\": \"//slash\","
                                       " \"ZList\": [1, 2.5, true, {\"ref\": 3}], \"ZFont\": {\"ref\": 5}}}",
                                       &error),
          "zebkit_pdf_annotation_update: entries", error);
    annotations = zebkit_pdf_annotations(pdf, 0, &error);
    check_text(contains(annotations, "\"IT\":\"/HighlightNote\"") && contains(annotations, "\"ZNote\":\"//slash\"") &&
              contains(annotations, "\"ZList\":[1,2.5,true,{\"ref\":3}]") &&
              contains(annotations, "\"ZFont\":{\"BaseFont\":\"/Helvetica\"") &&
              contains(annotations, "\"object\":8"),
          "the entries are there: a page referred to, a font followed", annotations);
    zebkit_free(annotations);
    check_text(zebkit_pdf_annotation_update(pdf, 0, 1, "{\"entries\": {\"ZNote\": null, \"ZList\": null, \"ZFont\": null}}", &error),
          "zebkit_pdf_annotation_update: entries removed", error);
    check_text(!zebkit_pdf_annotation_update(pdf, 0, 1, "{\"entries\": {\"AP\": null}}", &error) && contains(error, "AP"),
          "zebkit_pdf_annotation_update refuses /AP", error);
    zebkit_free(error);
    error = NULL;

    check_text(zebkit_pdf_annotation_update(pdf, 0, 1, "{\"color\": [255, 0, 255], \"contents\": null}", &error),
          "zebkit_pdf_annotation_update", error);
    annotations = zebkit_pdf_annotations(pdf, 0, &error);
    check_text(contains(annotations, "\"color\":[255,0,255]") && !contains(annotations, "a note"),
          "the change is there", NULL);
    zebkit_free(annotations);
    check_text(!zebkit_pdf_annotation_update(pdf, 0, 1, "{\"ink\": [[[0, 0], [1, 1]]]}", &error) && error,
          "zebkit_pdf_annotation_update refuses ink on a highlight", error);
    zebkit_free(error);
    error = NULL;
    check_text(zebkit_pdf_annotation_remove(pdf, 0, last, &error), "zebkit_pdf_annotation_remove", error);
    annotations = zebkit_pdf_annotations(pdf, 0, &error);
    check_text(!contains(annotations, "sticky"), "the note is gone", NULL);
    zebkit_free(annotations);

    /* An image as a stamp's appearance: red, over its rectangle. */
    int stamp = zebkit_pdf_annotation_add(pdf, 0, "{\"type\": \"stamp\", \"rect\": [230, 150, 290, 190]}", &error);
    check_text(stamp >= 0, "zebkit_pdf_annotation_add: stamp", error);
    static const uint8_t stamp_pixels[16] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    check_text(zebkit_pdf_annotation_set_image(pdf, 0, stamp, stamp_pixels, 2, 2, &error), "zebkit_pdf_annotation_set_image",
          error);
    pixels = zebkit_pdf_render(pdf, 0, NULL, &width, &height, &error);
    /* (260, 170) in the page, from the top left of 300 by 200. */
    const uint8_t *there = pixels ? pixels + (30 * width + 260) * 4 : NULL;
    snprintf(detail, sizeof(detail), "%d,%d,%d", there ? there[0] : -1, there ? there[1] : -1, there ? there[2] : -1);
    check_text(there && there[0] > 200 && there[1] < 60 && there[2] < 60, "the stamp draws its image", detail);
    zebkit_free(pixels);
    check_text(!zebkit_pdf_annotation_set_image(pdf, 0, 0, stamp_pixels, 2, 2, &error) && contains(error, "stamp"),
          "zebkit_pdf_annotation_set_image refuses what is not a stamp", error);
    zebkit_free(error);
    error = NULL;

    char *lines = zebkit_pdf_text_lines(pdf, 0, &error);
    check_text(contains(lines, "\"text\":\"Hello zebkit\"") && contains(lines, "\"box\":[") &&
              contains(lines, "\"unmapped\":0"),
          "zebkit_pdf_text_lines", lines);
    zebkit_free(lines);
    char *images = zebkit_pdf_images(pdf, 0, &error);
    check_text(images && strcmp(images, "[]") == 0, "zebkit_pdf_images: a page of text has none", images);
    zebkit_free(images);

    /* PDFium's mark for a broken word's hyphen, given as a hyphen. */
    zebkit_pdf *broken = zebkit_pdf_open(hyphenated, sizeof(hyphenated) - 1, NULL, &error);
    char *broken_text = broken ? zebkit_pdf_text(broken, 0, &error) : NULL;
    char *broken_lines = broken ? zebkit_pdf_text_lines(broken, 0, &error) : NULL;
    check_text(broken_text && !strchr(broken_text, 0x2) && strstr(broken_text, "two-") && broken_lines &&
              !strstr(broken_lines, "\\u0002") && strstr(broken_lines, "two-"),
          "zebkit_pdf_text and _text_lines: a hyphen, not U+0002", broken_lines);
    zebkit_free(broken_text);
    zebkit_free(broken_lines);
    zebkit_pdf_close(broken);

    /* A page with a 2 by 2 picture, red, green, blue and white. */
    zebkit_pdf *pictured = zebkit_pdf_open(picture, sizeof(picture) - 1, NULL, &error);
    images = pictured ? zebkit_pdf_images(pictured, 0, &error) : NULL;
    check_text(contains(images, "\"width\":2") && contains(images, "\"height\":2") &&
              contains(images, "\"jpeg\":false") && contains(images, "\"rect\":[50.0,50.0,150.0,150.0]"),
          "zebkit_pdf_images", images);
    zebkit_free(images);
    size_t png_size = 0;
    bool jpeg = true;
    uint8_t *png = pictured ? zebkit_pdf_image_data(pictured, 0, 0, &png_size, &jpeg, &error) : NULL;
    int png_width = 0, png_height = 0;
    uint8_t *decoded = png ? zebkit_image_decode(png, png_size, &png_width, &png_height, &error) : NULL;
    snprintf(detail, sizeof(detail), "%dx%d, first pixel %d,%d,%d", png_width, png_height,
             decoded ? decoded[0] : -1, decoded ? decoded[1] : -1, decoded ? decoded[2] : -1);
    check_text(decoded && !jpeg && png_width == 2 && png_height == 2 && decoded[0] == 255 && decoded[1] == 0 &&
              decoded[4] == 0 && decoded[5] == 255,
          "zebkit_pdf_image_data: its pixels as PNG", detail);
    zebkit_free(decoded);
    zebkit_free(png);
    check_text(pictured && !zebkit_pdf_image_data(pictured, 0, 1, &png_size, &jpeg, &error) && error,
          "zebkit_pdf_image_data refuses an image not there", error);
    zebkit_free(error);
    error = NULL;
    zebkit_pdf_close(pictured);

    zebkit_pdf *signed_pdf = zebkit_pdf_open(signed_form, sizeof(signed_form) - 1, NULL, &error);
    char *signatures = signed_pdf ? zebkit_pdf_signatures(signed_pdf, &error) : NULL;
    check_text(contains(signatures, "\"field\":\"Signatures.Approval\"") &&
              contains(signatures, "\"subFilter\":\"adbe.pkcs7.detached\"") &&
              contains(signatures, "\"reason\":\"Approved\"") &&
              contains(signatures, "\"time\":\"D:20261021120000Z\"") &&
              contains(signatures, "\"byteRange\":[0,100,200,300]") &&
              contains(signatures, "\"name\":\"zebkit test signer\"") &&
              contains(signatures, "\"serial\":\"1234\"") && contains(signatures, "\"notAfter\":\"2036-"),
          "zebkit_pdf_signatures: a nested field, its signer", signatures);
    zebkit_free(signatures);
    size_t signature_size = 0;
    uint8_t *signature = signed_pdf ? zebkit_pdf_signature_contents(signed_pdf, 0, &signature_size, &error) : NULL;
    check_text(signature && signature_size >= 587 && signature[0] == 0x30, "zebkit_pdf_signature_contents: the DER",
          error);
    zebkit_free(signature);
    signatures = zebkit_pdf_signatures(pdf, &error);
    check_text(signatures && strcmp(signatures, "[]") == 0, "zebkit_pdf_signatures: none in an unsigned document",
          signatures);
    zebkit_free(signatures);
    zebkit_pdf_close(signed_pdf);

    /* Checked against the bytes: intact, then with one signed byte changed. */
    zebkit_pdf *real = zebkit_pdf_open(really_signed, sizeof(really_signed) - 1, NULL, &error);
    signatures = real ? zebkit_pdf_signatures(real, &error) : NULL;
    check_text(contains(signatures, "\"intact\":true") && contains(signatures, "\"coversDocument\":true") &&
              contains(signatures, "\"digest\":\"SHA256\""),
          "zebkit_pdf_signatures: a signature checked, intact", signatures);
    zebkit_free(signatures);
    zebkit_pdf_close(real);
    real = zebkit_pdf_open(signed_by_key_id, sizeof(signed_by_key_id) - 1, NULL, &error);
    signatures = real ? zebkit_pdf_signatures(real, &error) : NULL;
    check_text(contains(signatures, "\"intact\":true"), "zebkit_pdf_signatures: a signer named by its key", signatures);
    zebkit_free(signatures);
    zebkit_pdf_close(real);
    char *changed = malloc(sizeof(really_signed));
    memcpy(changed, really_signed, sizeof(really_signed));
    char *reason = strstr(changed, "(Approved)");
    if (reason)
        reason[1] = 'a';
    real = zebkit_pdf_open(changed, sizeof(really_signed) - 1, NULL, &error);
    signatures = real ? zebkit_pdf_signatures(real, &error) : NULL;
    check_text(contains(signatures, "\"intact\":false") && contains(signatures, "changed after it was signed"),
          "zebkit_pdf_signatures: a byte changed after signing", signatures);
    zebkit_free(signatures);
    zebkit_pdf_close(real);
    free(changed);

    check_text(zebkit_pdf_attach(pdf, "data.csv", "a,b\n1,2\n", 8, &error), "zebkit_pdf_attach", error);
    char *attachments = zebkit_pdf_attachments(pdf, &error);
    check_text(contains(attachments, "\"name\":\"data.csv\"") && contains(attachments, "\"size\":8"),
          "zebkit_pdf_attachments", attachments);
    zebkit_free(attachments);
    size_t size = 0;
    uint8_t *file = zebkit_pdf_attachment(pdf, 0, &size, &error);
    check_text(file && size == 8 && memcmp(file, "a,b\n1,2\n", 8) == 0, "zebkit_pdf_attachment", NULL);
    zebkit_free(file);

    check_text(zebkit_pdf_set_metadata(pdf, "{\"title\": \"Résumé 履历\", \"author\": \"zebkit\"}", &error),
          "zebkit_pdf_set_metadata", error);
    check_text(!zebkit_pdf_set_metadata(pdf, "{\"colour\": \"red\"}", &error) && contains(error, "colour"),
          "zebkit_pdf_set_metadata refuses an unknown entry", error);
    zebkit_free(error);
    error = NULL;

    uint8_t *saved = zebkit_pdf_save(pdf, 0, &size, &error);
    check_text(saved && size > 100, "zebkit_pdf_save", error);
    zebkit_pdf_close(pdf);

    zebkit_pdf *again = saved ? zebkit_pdf_open(saved, size, NULL, &error) : NULL;
    info = again ? zebkit_pdf_info(again) : NULL;
    check_text(contains(info, "\"title\":\"Résumé 履历\"") && contains(info, "\"author\":\"zebkit\""),
          "the saved document has its new metadata", info);
    zebkit_free(info);
    attachments = again ? zebkit_pdf_attachments(again, &error) : NULL;
    check_text(contains(attachments, "data.csv"), "and its attachment", attachments);
    zebkit_free(attachments);
    annotations = again ? zebkit_pdf_annotations(again, 0, &error) : NULL;
    check_text(contains(annotations, "\"type\":\"polygon\"") && contains(annotations, "\"color\":[255,0,255]"),
          "and its annotations", NULL);
    zebkit_free(annotations);
    check_text(again && zebkit_pdf_flatten(again, 0, false, &error), "zebkit_pdf_flatten", error);
    annotations = again ? zebkit_pdf_annotations(again, 0, &error) : NULL;
    check_text(annotations && !contains(annotations, "polygon"), "the flattened page has no polygon annotation",
          annotations);
    zebkit_free(annotations);
    check_text(again && zebkit_pdf_detach(again, 0, &error), "zebkit_pdf_detach", error);
    zebkit_pdf_close(again);
    zebkit_free(saved);

    test_forms_and_pages();
    test_redaction();
    return summary();
}
