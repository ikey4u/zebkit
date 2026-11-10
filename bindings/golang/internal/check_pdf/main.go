// The Go binding's checks of the PDF, canvas, text and services APIs
// against a real engine. Exit 0 when every check passes.
//
//	go run ./internal/check_pdf [--resources=DIR] --no-sandbox --headless
//
// DIR is PDFium's testing/resources (hello_world.pdf, bookmarks.pdf,
// text_form.pdf, fonts/ahem/Ahem.ttf); by default the checkout's.
package main

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

var checks, failures int

func check(ok bool, what string, detail ...any) {
	checks++
	if ok {
		fmt.Println("ok   " + what)
		return
	}
	failures++
	fmt.Println("FAIL "+what, detail)
}

func must[T any](value T, err error) T {
	if err != nil {
		fmt.Println("FAIL", err)
		failures++
	}
	return value
}

func decode(text string) any {
	var value any
	_ = json.Unmarshal([]byte(text), &value)
	return value
}

func list(text string) []any {
	value, _ := decode(text).([]any)
	return value
}

func object(text string) map[string]any {
	value, _ := decode(text).(map[string]any)
	return value
}

// pixel is (r, g, b, a) at (x, y) of an image.
func pixel(image *zebkit.Pixels, x, y int) [4]byte {
	i := (y*image.Width + x) * 4
	return [4]byte{image.RGBA[i], image.RGBA[i+1], image.RGBA[i+2], image.RGBA[i+3]}
}

func near(p [4]byte, r, g, b byte) bool {
	d := func(a, b byte) int {
		if a > b {
			return int(a - b)
		}
		return int(b - a)
	}
	return d(p[0], r) < 40 && d(p[1], g) < 40 && d(p[2], b) < 40
}

func resourcesDir(args []string) (string, []string) {
	var rest []string
	dir := ""
	for _, arg := range args {
		if strings.HasPrefix(arg, "--resources=") {
			dir = strings.TrimPrefix(arg, "--resources=")
		} else {
			rest = append(rest, arg)
		}
	}
	if dir == "" {
		// The module is bindings/golang of the checkout.
		wd, _ := os.Getwd()
		for d := wd; d != filepath.Dir(d); d = filepath.Dir(d) {
			candidate := filepath.Join(d, "upstream/chromium/src/third_party/pdfium/testing/resources")
			if _, err := os.Stat(candidate); err == nil {
				dir = candidate
				break
			}
		}
	}
	return dir, rest
}

func read(dir, name string) []byte {
	data, err := os.ReadFile(filepath.Join(dir, name))
	if err != nil {
		fmt.Println("FAIL reading", name, err)
		os.Exit(1)
	}
	return data
}

func main() {
	dir, args := resourcesDir(os.Args[1:])
	if dir == "" {
		fmt.Println("FAIL no PDFium resources: --resources=DIR")
		os.Exit(1)
	}
	font := read(dir, "fonts/ahem/Ahem.ttf")
	hello := read(dir, "hello_world.pdf")

	if err := zebkit.Init(args...); err != nil {
		fmt.Println("FAIL zebkit.Init:", err)
		os.Exit(1)
	}
	checkServices()
	checkText(font)
	checkCanvas(font)
	checkPDF(dir, hello, font)
	checkHooks(hello, font)
	zebkit.Shutdown()
	fmt.Printf("%d of %d checks passed\n", checks-failures, checks)
	if failures > 0 {
		os.Exit(1)
	}
}

func checkServices() {
	payload := bytes.Repeat([]byte("zebkit "), 1000)
	for _, format := range []zebkit.Compression{zebkit.CompressGzip, zebkit.CompressZlib, zebkit.CompressDeflateRaw,
		zebkit.CompressBrotli, zebkit.CompressZstd} {
		packed, err := zebkit.Compress(format, payload, -1)
		unpacked, err2 := zebkit.Decompress(format, packed, 0)
		check(err == nil && err2 == nil && len(packed) < len(payload) && bytes.Equal(unpacked, payload),
			fmt.Sprintf("compress and decompress, format %d", format), err, err2)
	}
	_, err := zebkit.Decompress(zebkit.CompressZstd, payload, 0)
	check(err != nil, "decompress refuses what is not compressed")
	check(hex.EncodeToString(zebkit.ComputeDigest(zebkit.SHA256, []byte("abc"))) ==
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc")
	check(zebkit.SHA512.Size() == 64 && len(zebkit.ComputeDigest(zebkit.SHA1, nil)) == 20, "digest sizes")
	check(hex.EncodeToString(zebkit.HMAC(zebkit.SHA256, []byte("key"), []byte("The quick brown fox jumps over the lazy dog"))) ==
		"f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8", "HMAC-SHA256")
	random, err := zebkit.RandomBytes(32)
	check(err == nil && len(random) == 32 && !bytes.Equal(random, make([]byte, 32)), "random bytes")
	key, nonce := bytes.Repeat([]byte{7}, 32), bytes.Repeat([]byte{9}, 12)
	sealed, err := zebkit.AESGCMSeal(key, nonce, []byte("ad"), []byte("secret"))
	opened, err2 := zebkit.AESGCMOpen(key, nonce, []byte("ad"), sealed)
	check(err == nil && err2 == nil && len(sealed) == 6+16 && string(opened) == "secret", "AES-GCM seal and open")
	sealed[0] ^= 1
	_, err = zebkit.AESGCMOpen(key, nonce, []byte("ad"), sealed)
	check(err != nil, "AES-GCM refuses what was changed")
	host, ok := zebkit.URLGet("../b?q=1#f", "https://User@Example.COM:443/a/x", zebkit.URLHost)
	path, _ := zebkit.URLGet("../b?q=1#f", "https://User@Example.COM:443/a/x", zebkit.URLPath)
	origin, _ := zebkit.URLGet("https://example.com:8443/x", "", zebkit.URLOrigin)
	_, invalid := zebkit.URLGet("not a url", "", zebkit.URLSpec)
	check(ok && host == "example.com" && path == "/b" && origin == "https://example.com:8443" && !invalid, "URL parts",
		host, path, origin)
	domain, ok := zebkit.RegistrableDomain("a.b.example.co.uk")
	none, ok2 := zebkit.RegistrableDomain("127.0.0.1")
	check(ok && domain == "example.co.uk" && ok2 && none == "", "registrable domain", domain, none)
	image := &zebkit.Pixels{Width: 2, Height: 2, RGBA: []byte{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 128}}
	png, err := zebkit.EncodeImage(zebkit.ImagePNG, image, 0)
	back, err2 := zebkit.DecodeImage(png)
	check(err == nil && err2 == nil && back.Width == 2 && bytes.Equal(back.RGBA, image.RGBA), "PNG encode and decode")
	jpeg, err := zebkit.EncodeImage(zebkit.ImageJPEG, image, 90)
	check(err == nil && len(jpeg) > 2 && jpeg[0] == 0xff && jpeg[1] == 0xd8, "JPEG encode")
	_, err = zebkit.DecodeImage([]byte("no image"))
	check(err != nil, "decode refuses what is not an image")
	check(zebkit.ICUInit(), "ICU")

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err == nil {
		go http.Serve(listener, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			body := make([]byte, 0)
			if r.Body != nil {
				body, _ = readAll(r)
			}
			w.Header().Set("X-Method", r.Method)
			fmt.Fprintf(w, "got %s", body)
		}))
		var response *zebkit.FetchResponse
		zebkit.Fetch("POST", fmt.Sprintf("http://%s/", listener.Addr()), "Content-Type: text/plain", []byte("hi"),
			func(r *zebkit.FetchResponse) { response = r })
		end := time.Now().Add(15 * time.Second)
		for response == nil && time.Now().Before(end) {
			zebkit.Pump()
			time.Sleep(5 * time.Millisecond)
		}
		check(response != nil && response.Err == nil && response.Status == 200 && string(response.Body) == "got hi" &&
			strings.Contains(strings.ToLower(response.Headers), "x-method: post"), "Fetch through the engine's network stack", response)
	}
}

func readAll(r *http.Request) ([]byte, error) {
	var buffer bytes.Buffer
	_, err := buffer.ReadFrom(r.Body)
	return buffer.Bytes(), err
}

func checkText(font []byte) {
	shaped, err := zebkit.ShapeText(font, 20, "AB", "en", "")
	glyphs, _ := object(shaped)["glyphs"].([]any)
	check(err == nil && len(glyphs) == 2, "shape text", shaped)
	rtl, _ := zebkit.ShapeText(font, 20, "AB", "", "rtl")
	glyphs, _ = object(rtl)["glyphs"].([]any)
	first, _ := glyphs[0].(map[string]any)
	check(len(glyphs) == 2 && first["cluster"] == 1.0, "shape right to left")
	_, err = zebkit.ShapeText([]byte("no"), 20, "a", "", "")
	check(err != nil, "shaping refuses what is not a font")
}

func checkCanvas(font []byte) {
	canvas, err := zebkit.NewCanvas(100, 50)
	if err != nil {
		check(false, "a canvas", err)
		return
	}
	defer canvas.Close()
	check(canvas.Width() == 100 && canvas.Height() == 50, "canvas size")
	canvas.SetFillColor(0xff0000ff)
	canvas.Rect(0, 0, 50, 50)
	canvas.Fill(false)
	canvas.Save()
	canvas.Translate(50, 0)
	check(canvas.SetLinearGradient(0, 0, 50, 0, []float64{0, 1}, []uint32{0x0000ffff, 0x00ff00ff}), "a linear gradient")
	canvas.BeginPath()
	canvas.Rect(0, 0, 50, 25)
	canvas.Fill(false)
	canvas.Restore()
	check(!canvas.SetRadialGradient(0, 0, 1, []float64{0}, []uint32{0}), "a gradient refuses one stop")
	canvas.SetStrokeColor(0x000000ff)
	canvas.SetLineWidth(2)
	canvas.SetLineCap(zebkit.CapRound)
	canvas.SetLineJoin(zebkit.JoinBevel)
	canvas.SetMiterLimit(4)
	canvas.SetDash([]float64{3, 2}, 0)
	canvas.BeginPath()
	canvas.MoveTo(60, 40)
	canvas.LineTo(90, 40)
	canvas.QuadTo(95, 45, 90, 48)
	canvas.CurveTo(80, 49, 70, 49, 60, 48)
	canvas.Arc(75, 40, 5, 0, 180, false)
	canvas.ClosePath()
	canvas.Stroke()
	canvas.SetDash(nil, 0)
	check(canvas.SetBlend("Multiply") && !canvas.SetBlend("Nope"), "blend modes")
	canvas.SetBlend("Normal")
	canvas.SetAlpha(1)
	pixels, err := canvas.Pixels()
	check(err == nil && near(pixel(pixels, 10, 10), 255, 0, 0) && pixel(pixels, 99, 30)[3] == 0, "fills, in place",
		pixel(pixels, 10, 10))
	right := pixel(pixels, 98, 5)
	check(right[1] > right[2], "the gradient goes from blue to green", right)

	f, err := zebkit.NewFont(font)
	if err == nil {
		defer f.Close()
		canvas.SetFont(f, 20)
		canvas.SetFillColor(0x000000ff)
		advance, ok := canvas.MeasureText("AB")
		check(ok && advance == 40, "measure text in Ahem", advance)
		check(canvas.FillText("A", 2, 40) == nil, "fill text")
		pixels, _ = canvas.Pixels()
		check(near(pixel(pixels, 10, 30), 0, 0, 0), "text drawn in the font's boxes", pixel(pixels, 10, 30))
		canvas.BeginPath()
		check(canvas.TextPath("B", 2, 40) == nil, "text as a path")
	}
	_, err = zebkit.NewFont([]byte("nope"))
	check(err != nil, "a font refuses what is not one")

	small := &zebkit.Pixels{Width: 1, Height: 1, RGBA: []byte{0, 0, 255, 255}}
	check(canvas.DrawImage(small, 90, 0, 10, 10), "draw an image")
	canvas.Save()
	canvas.BeginPath()
	canvas.Rect(0, 0, 5, 5)
	canvas.Clip(false)
	canvas.Clear(0x00ff00ff)
	canvas.Restore()
	canvas.SetMatrix(1, 0, 0, 1, 0, 0)
	canvas.Transform(1, 0, 0, 1, 0, 0)
	canvas.Scale(1, 1)
	canvas.Rotate(0)
	check(canvas.Run(`[["beginPath"], ["fillColor", 4294967295], ["rect", 0, 45, 5, 5], ["fill", false]]`) == nil, "run operations")
	check(canvas.Run(`[["nope"]]`) != nil, "run refuses an unknown operation")
	pixels, _ = canvas.Pixels()
	check(near(pixel(pixels, 95, 5), 0, 0, 255) && near(pixel(pixels, 2, 2), 0, 255, 0) && near(pixel(pixels, 2, 47), 255, 255, 255),
		"image, clip and clear, operations", pixel(pixels, 95, 5), pixel(pixels, 2, 2), pixel(pixels, 2, 47))
	png, err := canvas.PNG()
	decoded, err2 := zebkit.DecodeImage(png)
	check(err == nil && err2 == nil && decoded.Width == 100, "the canvas as PNG")

	rgba := make([]byte, 4*4*4)
	err = zebkit.OnPixels(rgba, 4, 4, 0, zebkit.CanvasUnpremultiplied, func(c *zebkit.Canvas) {
		c.SetFillColor(0x00ff00ff)
		c.Rect(0, 0, 4, 4)
		c.Fill(false)
	})
	check(err == nil && rgba[1] == 255 && rgba[3] == 255, "a canvas over the caller's pixels", rgba[:4])
}

func checkPDF(dir string, hello, font []byte) {
	pdf, err := zebkit.OpenPDF(hello, "")
	if err != nil {
		check(false, "open a PDF", err)
		return
	}
	defer pdf.Close()
	check(pdf.PageCount() == 1 && object(pdf.Info())["pages"] == 1.0, "page count and info")
	page := object(must(pdf.Page(0)))
	check(page["width"].(float64) > 0, "a page's size", page)
	image, err := pdf.Render(0, nil)
	check(err == nil && image.Width > 0 && len(image.RGBA) == image.Width*image.Height*4, "render a page")
	tile, err := pdf.Render(0, &zebkit.PDFRenderOptions{Scale: 2, Width: 40, Height: 30, Flags: zebkit.PDFRenderGrayscale})
	check(err == nil && tile.Width == 40 && tile.Height == 30, "render a tile at scale 2")
	text := must(pdf.Text(0))
	check(strings.Contains(text, "Hello"), "the page's text", text)
	layout := object(must(pdf.TextLayout(0)))
	chars, _ := layout["chars"].([]any)
	check(len(chars) > 5, "text layout")
	check(len(list(must(pdf.TextRects(0, 0, 5)))) > 0 && len(list(must(pdf.TextLines(0)))) > 0, "text rects and lines")
	first, _ := chars[0].(map[string]any)
	box, _ := first["box"].([]any)
	if len(box) == 4 {
		x := (box[0].(float64) + box[2].(float64)) / 2
		y := (box[1].(float64) + box[3].(float64)) / 2
		check(pdf.CharAt(0, x, y, 1) == 0 && pdf.CharAt(0, -100, -100, 1) == -1, "the character at a point")
	}
	matches := list(must(pdf.Search("world", 0, 0, 0)))
	check(len(matches) == 2, "search: \"world\" twice", len(matches))
	check(len(list(must(pdf.Search("WORLD", zebkit.PDFSearchCase|zebkit.PDFSearchWholeWord, 0, 0)))) == 0, "search, matching case")
	check(list(must(pdf.Signatures())) != nil, "signatures, none")
	_, err = pdf.Links(0)
	check(err == nil, "links")
	check(pdf.SetMetadata(`{"title": "From Go"}`) == nil && strings.Contains(pdf.Info(), "From Go"), "set metadata")

	bookmarks, err := zebkit.OpenPDF(read(dir, "bookmarks.pdf"), "")
	if err == nil {
		outline := list(must(bookmarks.Outline()))
		check(len(outline) > 0, "the outline", len(outline))
		check(bookmarks.SetOutline(`[{"title": "Only", "page": 0}]`) == nil && len(list(must(bookmarks.Outline()))) == 1, "set the outline")
		bookmarks.Close()
	}

	// Annotations.
	index, err := pdf.AddAnnotation(0, `{"type": "square", "rect": [10, 10, 60, 60], "color": [255, 0, 0], "contents": "a box"}`)
	check(err == nil && index == 0, "add an annotation", err)
	_, err = pdf.AddAnnotation(0, `{"type": "nope", "rect": [0, 0, 1, 1]}`)
	check(err != nil, "an annotation of no such type is refused")
	check(pdf.UpdateAnnotation(0, 0, `{"contents": "changed"}`) == nil, "update an annotation")
	annotations := list(must(pdf.Annotations(0)))
	square, _ := annotations[0].(map[string]any)
	check(len(annotations) == 1 && square["contents"] == "changed" && square["type"] == "square", "list annotations", square["type"])
	at, err := pdf.AnnotationAt(0, 35, 10.5, 2)
	check(err == nil && at == 0, "the annotation at a point", at)
	shown, err := pdf.RenderAnnotation(0, 0, 0, nil)
	check(err == nil && shown.Width > 0, "render an annotation alone", err)
	stamp, _ := pdf.AddAnnotation(0, `{"type": "stamp", "rect": [100, 100, 140, 140]}`)
	check(pdf.SetAnnotationImage(0, stamp, &zebkit.Pixels{Width: 1, Height: 1, RGBA: []byte{0, 0, 255, 255}}) == nil, "an image stamp")
	check(pdf.DrawAnnotation(0, 0, `[["fillColor", 4278190335], ["rect", 0, 0, 10, 10], ["fill", false]]`) == nil, "draw an appearance")
	check(pdf.MoveAnnotation(0, 1, 0) == nil && list(must(pdf.Annotations(0)))[0].(map[string]any)["type"] == "stamp", "move an annotation")
	action := object(must(pdf.AnnotationAction(0, 0)))
	check(action["kind"] == "none", "an annotation's action", action)
	link, _ := pdf.AddAnnotation(0, `{"type": "link", "rect": [0, 0, 5, 5], "url": "https://example.com/"}`)
	check(object(must(pdf.AnnotationAction(0, link)))["kind"] == "uri", "a link's action")
	attachment, err := pdf.AddAnnotation(0, `{"type": "fileAttachment", "rect": [200, 200, 220, 220]}`)
	check(err == nil && pdf.AttachFile(0, attachment, "note.txt", []byte("hello")) == nil, "attach a file to an annotation", err)
	name, file, err := pdf.AnnotationFile(0, attachment)
	check(err == nil && name == "note.txt" && string(file) == "hello", "the annotation's file", name)

	other := must(zebkit.NewPDF())
	if other != nil {
		check(other.InsertPage(0, 200, 200) == nil, "a new document's page")
		// The stamp, first since it was moved there.
		copied, err := pdf.CopyAnnotation(0, 0, other, 0)
		check(err == nil && copied == 0 && len(list(must(other.Annotations(0)))) == 1, "copy an annotation to another document", err)
		check(other.StampFromPage(0, 0, pdf, 0) == nil, "a page as a stamp's appearance")
		check(other.FlattenAnnotation(0, 0) == nil && len(list(must(other.Annotations(0)))) == 0, "flatten an annotation")
		images := list(must(other.Images(0)))
		check(images != nil, "images of a page")
		if len(images) > 0 {
			data, _, err := other.ImageData(0, 0)
			check(err == nil && len(data) > 0, "an image's data")
		}
		other.Close()
	}
	xfdf, err := pdf.ExportAnnotations(zebkit.XFDF, -1, -1)
	check(err == nil && bytes.Contains(xfdf, []byte("<xfdf")), "export XFDF")
	fdf, err := pdf.ExportAnnotations(zebkit.FDF, 0, -1)
	check(err == nil && bytes.HasPrefix(fdf, []byte("%FDF")), "export FDF")
	fresh := must(zebkit.OpenPDF(hello, ""))
	if fresh != nil {
		count, err := fresh.ImportAnnotations(zebkit.XFDF, xfdf)
		check(err == nil && count >= 2, "import XFDF", count, err)
		redacted, err := fresh.Redact(0, "[[0, 0, 600, 800]]", 0x000000ff)
		check(err == nil && redacted == 1 && !strings.Contains(must(fresh.Text(0)), "Hello"), "redact")
		check(fresh.Flatten(0, false) == nil, "flatten a page")
		fresh.Close()
	}
	check(pdf.RemoveAnnotation(0, link) == nil, "remove an annotation")

	// Fonts for free text.
	check(pdf.AddFont("Ahem", font) == nil, "a font for free text")
	_, err = pdf.AddAnnotation(0, `{"type": "freeText", "rect": [10, 200, 200, 250], "contents": "X", "font": "Ahem", "fontSize": 20}`)
	check(err == nil, "free text in a given font", err)

	// Attachments.
	check(pdf.Attach("data.csv", []byte("a,b")) == nil, "attach a file")
	attachments := list(must(pdf.Attachments()))
	data, err := pdf.Attachment(0)
	check(len(attachments) == 1 && err == nil && string(data) == "a,b", "attachments")
	check(pdf.Detach(0) == nil && len(list(must(pdf.Attachments()))) == 0, "detach")

	// Pages.
	check(pdf.InsertPage(1, 300, 400) == nil && pdf.PageCount() == 2, "insert a page")
	check(pdf.RotatePage(1, 90) == nil && object(must(pdf.Page(1)))["rotation"] == 90.0, "rotate a page")
	check(pdf.MovePages([]int{1}, 0) == nil && object(must(pdf.Page(0)))["rotation"] == 90.0, "move pages")
	check(pdf.DeletePages([]int{0}) == nil && pdf.PageCount() == 1, "delete pages")
	check(pdf.ImportPages(must(zebkit.OpenPDF(hello, "")), nil, 1) == nil && pdf.PageCount() == 2, "import pages")
	check(pdf.RotatePage(0, 45) != nil, "a rotation that is not a right angle is refused")

	// Save and open again.
	saved, err := pdf.Save(0)
	again, err2 := zebkit.OpenPDF(saved, "")
	check(err == nil && err2 == nil && again.PageCount() == 2 && len(list(must(again.Annotations(0)))) >= 3, "save and open again", err, err2)
	if again != nil {
		again.Close()
	}
	incremental, err := pdf.Save(zebkit.PDFSaveIncremental)
	check(err == nil && len(incremental) > len(hello), "save incrementally")
	path := filepath.Join(os.TempDir(), fmt.Sprintf("zebkit-go-%d.pdf", os.Getpid()))
	if os.WriteFile(path, saved, 0o600) == nil {
		fromFile, err := zebkit.OpenPDFFile(path, "")
		check(err == nil && fromFile.PageCount() == 2, "open a file")
		if fromFile != nil {
			fromFile.Close()
		}
		os.Remove(path)
	}
	_, err = zebkit.OpenPDF([]byte("not a pdf"), "")
	check(err != nil, "open refuses what is not a PDF")

	// Forms.
	form, err := zebkit.OpenPDF(read(dir, "text_form.pdf"), "")
	if err == nil {
		fields := list(must(form.FormFields()))
		check(len(fields) == 1, "form fields", len(fields))
		if len(fields) == 1 {
			name := fields[0].(map[string]any)["name"].(string)
			check(form.SetField(name, "from Go") == nil, "set a field")
			value := list(must(form.FormFields()))[0].(map[string]any)["value"]
			check(value == "from Go", "the field's value", value)
			check(form.ResetForm() == nil && list(must(form.FormFields()))[0].(map[string]any)["value"] != "from Go", "reset the form")
		}
		check(form.SetField("No such field", "x") != nil, "a field of no such name is refused")
		form.Close()
	}

	points, err := zebkit.FilterInput("ink", "[[0, 0], [10, 0.1], [20, 0]]", `{"simplify": 1}`)
	check(err == nil && len(list(points)) == 2, "filter a tool's points", points)
	check(len(list(must(pdf.PageData(0, "None_")))) == 0, "page data, none")
}

func checkHooks(hello, font []byte) {
	pdf, err := zebkit.OpenPDF(hello, "")
	if err != nil {
		return
	}
	defer pdf.Close()

	refuse, err := zebkit.HookBefore(pdf, "annotation.add", func(doc *zebkit.PDF, operation string) zebkit.PDFDecision {
		if strings.Contains(operation, "forbidden") {
			return zebkit.PDFRefuse("not that")
		}
		return zebkit.PDFContinue()
	})
	check(err == nil, "a before hook")
	_, err = pdf.AddAnnotation(0, `{"type": "square", "rect": [0, 0, 10, 10], "contents": "forbidden"}`)
	check(err != nil && strings.Contains(err.Error(), "not that"), "a before hook refuses, with its reason", err)
	change, _ := zebkit.HookBefore(pdf, "annotation.add", func(doc *zebkit.PDF, operation string) zebkit.PDFDecision {
		op := object(operation)
		annotation, _ := op["annotation"].(map[string]any)
		if annotation["contents"] == "change me" {
			annotation["contents"] = "changed by Go"
			text, _ := json.Marshal(op)
			return zebkit.PDFChange(string(text))
		}
		return zebkit.PDFContinue()
	})
	var inverse string
	after, _ := zebkit.HookAfter(pdf, "annotation.add", func(doc *zebkit.PDF, operation string) {
		if value, ok := object(operation)["inverse"]; ok {
			text, _ := json.Marshal(value)
			inverse = string(text)
		}
	})
	index, err := pdf.AddAnnotation(0, `{"type": "square", "rect": [0, 0, 10, 10], "contents": "change me"}`)
	annotations := list(must(pdf.Annotations(0)))
	check(err == nil && len(annotations) == 1 && annotations[0].(map[string]any)["contents"] == "changed by Go", "a before hook changes the operation")
	check(inverse != "" && pdf.Apply(inverse) == nil && len(list(must(pdf.Annotations(0)))) == 0, "an after hook's inverse undoes it", index)
	check(refuse.Remove() && change.Remove() && after.Remove() && !after.Remove(), "remove hooks")

	drawn := 0
	appearance, err := zebkit.HookAppearance(pdf, "stamp", func(c *zebkit.Canvas, annotation string, width, height float64) bool {
		drawn++
		c.SetFillColor(0x00ff00ff)
		c.Rect(0, 0, width, height)
		c.Fill(false)
		return true
	})
	stamp, _ := pdf.AddAnnotation(0, `{"type": "stamp", "rect": [20, 20, 60, 60]}`)
	image, _ := pdf.RenderAnnotation(0, stamp, 0, nil)
	check(err == nil && drawn > 0 && image != nil && near(pixel(image, image.Width/2, image.Height/2), 0, 255, 0), "an appearance hook draws", drawn)
	appearance.Remove()

	kind, err := zebkit.HookKind(pdf, zebkit.PDFKind{
		Name: "GoTest_Sticker", Prefix: "GoTest_",
		Schema: `{"mood": {"type": "string", "enum": ["happy", "sad"], "required": true}}`,
		Check: func(data string) string {
			if strings.Contains(data, "sad") {
				return "no sad stickers"
			}
			return ""
		},
		Draw: func(c *zebkit.Canvas, annotation string, width, height float64) bool {
			c.SetFillColor(0x0000ffff)
			c.Rect(0, 0, width, height)
			c.Fill(false)
			return true
		},
		HitTest: func(doc *zebkit.PDF, page, index int, annotation string, x, y float64) bool { return x < 110 },
	})
	check(err == nil, "a kind of the application's", err)
	sticker, err := pdf.AddAnnotation(0, `{"type": "GoTest_Sticker", "rect": [100, 100, 120, 120], "data": {"mood": "happy"}}`)
	check(err == nil, "an annotation of that kind", err)
	_, err = pdf.AddAnnotation(0, `{"type": "GoTest_Sticker", "rect": [100, 100, 120, 120], "data": {"mood": "sad"}}`)
	check(err != nil && strings.Contains(err.Error(), "no sad stickers"), "the kind's check refuses", err)
	_, err = pdf.AddAnnotation(0, `{"type": "GoTest_Sticker", "rect": [100, 100, 120, 120], "data": {"size": 1}}`)
	check(err != nil, "the kind's schema refuses")
	hitLeft, _ := pdf.AnnotationAt(0, 105, 110, 0)
	hitRight, _ := pdf.AnnotationAt(0, 115, 110, 0)
	check(hitLeft == sticker && hitRight != sticker, "the kind's hit test", hitLeft, hitRight)
	check(pdf.Flatten(0, false) == nil && len(list(must(pdf.PageData(0, "GoTest_")))) == 1, "flatten keeps the kind's data")
	kind.Remove()

	layer, _ := zebkit.HookLayer(pdf, true, func(c *zebkit.Canvas, page int, info zebkit.PDFLayerInfo) {
		c.SetFillColor(0xff0000ff)
		c.Rect(0, 0, 5, 5)
		c.Fill(false)
	})
	rendered, _ := pdf.Render(0, nil)
	check(rendered != nil && near(pixel(rendered, 1, rendered.Height-2), 255, 0, 0), "a layer above the page")
	layer.Remove()

	blank := must(zebkit.NewPDF())
	if blank != nil {
		blank.InsertPage(0, 100, 100)
		asked := 0
		ocr, _ := zebkit.HookOCR(blank, func(page int, image *zebkit.Pixels, scale float64) (string, bool) {
			asked++
			return `[{"text": "seen by Go", "box": [10, 10, 90, 20]}]`, image.Width > 0
		})
		check(strings.Contains(must(blank.Text(0)), "seen by Go") && asked == 1, "OCR for a page without text", asked)
		ocr.Remove()

		fontHook, _ := zebkit.HookFont(blank, func(family string) []byte {
			if family == "Boxes" {
				return font
			}
			return nil
		})
		_, err := blank.AddAnnotation(0, `{"type": "freeText", "rect": [0, 0, 100, 50], "contents": "X", "font": "Boxes", "fontSize": 20}`)
		check(err == nil, "a font from a hook", err)
		fontHook.Remove()

		trust, err := zebkit.HookTrust(blank, func(signature string, contents []byte) zebkit.PDFTrust { return zebkit.PDFTrusted })
		check(err == nil && trust.Remove(), "a trust hook")

		handled := ""
		action, _ := zebkit.HookAction(blank, func(action string) bool {
			handled = action
			return true
		})
		link, _ := blank.AddAnnotation(0, `{"type": "link", "rect": [0, 0, 5, 5], "url": "https://example.com/go"}`)
		result := object(must(blank.AnnotationAction(0, link)))
		check(result["handled"] == true && strings.Contains(handled, "example.com/go"), "an action hook takes it", result)
		action.Remove()

		input, _ := zebkit.HookInput(blank, "pen", func(tool, points string) (string, bool) {
			return "[[1, 1]]", tool == "pen"
		})
		filtered, err := blank.FilterInput("pen", "[[0, 0], [5, 5]]", "")
		check(err == nil && len(list(filtered)) == 1, "an input hook", filtered)
		input.Remove()
		blank.Close()
	}
	seen := 0
	all, err := zebkit.HookBefore(nil, "page.insert", func(doc *zebkit.PDF, operation string) zebkit.PDFDecision {
		seen++
		return zebkit.PDFContinue()
	})
	pdf.InsertPage(0, 100, 100)
	check(err == nil && seen == 1 && all.Remove(), "a hook for all documents", seen)
}
