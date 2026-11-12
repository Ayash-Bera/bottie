//go:build linux && cgo

package textmask

/*
#cgo pkg-config: pangocairo pangofc
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>

typedef struct { PangoFontMap *map; PangoContext *context; } TextEngine;

static int text_freetype_version(void) {
    FT_Library library;
    FT_Int major, minor, patch;
    if (FT_Init_FreeType(&library)) return 0;
    FT_Library_Version(library, &major, &minor, &patch);
    FT_Done_FreeType(library);
    return major * 1000000 + minor * 1000 + patch;
}

static TextEngine *text_engine_new(const char *bengali, const char *japanese) {
    FcConfig *config = FcConfigCreate();
    if (!config) return NULL;
    if (!FcConfigAppFontAddFile(config, (const FcChar8 *)bengali) ||
        !FcConfigAppFontAddFile(config, (const FcChar8 *)japanese)) {
        FcConfigDestroy(config); return NULL;
    }
    TextEngine *e = g_new0(TextEngine, 1);
    e->map = pango_cairo_font_map_new();
    pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(e->map), config);
    FcConfigDestroy(config);
    pango_cairo_font_map_set_resolution(PANGO_CAIRO_FONT_MAP(e->map), 72);
    e->context = pango_font_map_create_context(e->map);
    pango_context_set_round_glyph_positions(e->context, TRUE);
    cairo_font_options_t *options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_ON);
    pango_cairo_context_set_font_options(e->context, options);
    cairo_font_options_destroy(options);
    return e;
}

static void text_engine_free(TextEngine *e) {
    if (!e) return;
    g_object_unref(e->context); g_object_unref(e->map); g_free(e);
}

static void text_candidate(char *out, const char *value, int cut, int length) {
    memcpy(out, value, cut);
    out[cut] = 0;
    if (cut < length) memcpy(out + cut, "\xe2\x80\xa6", 4);
}

static int text_render(TextEngine *e, const char *input, int width, int height, int tail,
                       unsigned char *output, int *retained, int *font_size) {
    int result = 3;
    char *value = g_utf8_normalize(input, -1, G_NORMALIZE_DEFAULT_COMPOSE);
    if (!value) return result;
    int length = strlen(value);
    if (length > 1024) { g_free(value); return result; }
    PangoLayout *layout = pango_layout_new(e->context);
    PangoFontDescription *font = pango_font_description_new();
    gboolean bengali = FALSE;
    for (const char *p = value; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (c >= 0x0980 && c <= 0x09ff) bengali = TRUE;
    }
    pango_font_description_set_family(font, bengali ? "Noto Sans Bengali,Noto Sans CJK JP" : "Noto Sans CJK JP,Noto Sans Bengali");
    pango_context_set_language(pango_layout_get_context(layout), pango_language_from_string(bengali ? "bn" : "ja"));
    pango_layout_set_auto_dir(layout, FALSE);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_set_text(layout, value, length);
    pango_font_description_set_absolute_size(font, 15 * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    if (pango_layout_get_unknown_glyphs_count(layout)) { result = 1; goto done; }

    // Keep boundaries that are both cursor graphemes and shaped glyph clusters.
    unsigned char clusters[1025] = {0};
    clusters[0] = clusters[length] = 1;
    for (GSList *line = pango_layout_get_lines_readonly(layout); line; line = line->next) {
        for (GSList *run = ((PangoLayoutLine *)line->data)->runs; run; run = run->next) {
            PangoGlyphItem *item = run->data;
            for (int n = 0; n < item->glyphs->num_glyphs; n++) {
                int offset = item->item->offset + item->glyphs->log_clusters[n];
                if (offset >= 0 && offset <= length) clusters[offset] = 1;
            }
        }
    }
    int attr_count = 0, cuts[1025], count = 0;
    const PangoLogAttr *attrs = pango_layout_get_log_attrs_readonly(layout, &attr_count);
    const char *cursor = value;
    for (int n = 0; n < attr_count; n++) {
        int offset = cursor - value;
        if (attrs[n].is_cursor_position && clusters[offset]) cuts[count++] = offset;
        if (!*cursor) break;
        cursor = g_utf8_next_char(cursor);
    }
    if (!count || cuts[0] != 0 || cuts[count - 1] != length) goto done;
    if (tail && count > 1) count--;
    char candidate[1028];
    PangoRectangle ink;
    int selected = -1;
    for (int pixels = height == 26 ? 21 : 15; pixels >= 11 && selected < 0; pixels--) {
        pango_font_description_set_absolute_size(font, pixels * PANGO_SCALE);
        pango_layout_set_font_description(layout, font);
        for (int n = count - 1; n >= 0; n--) {
            text_candidate(candidate, value, cuts[n], length);
            pango_layout_set_text(layout, candidate, -1);
            pango_layout_get_pixel_extents(layout, &ink, NULL);
            if (ink.height > height) break;
            if (ink.width <= width) { selected = cuts[n]; *font_size = pixels; break; }
        }
    }
    if (selected < 0) { result = 2; goto done; }
    *retained = selected;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_move_to(cr, (width - ink.width) / 2 - ink.x, (height - ink.height) / 2 - ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_surface_flush(surface);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS && cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
        unsigned char *pixels = cairo_image_surface_get_data(surface);
        int stride = cairo_image_surface_get_stride(surface);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x += 2)
                output[(y * width + x) / 2] = ((pixels[y * stride + x] + 8) / 17 << 4) | ((pixels[y * stride + x + 1] + 8) / 17);
        result = 0;
    }
    cairo_destroy(cr); cairo_surface_destroy(surface);
done:
    pango_font_description_free(font); g_object_unref(layout); g_free(value);
    return result;
}
*/
import "C"

import (
	"crypto/sha256"
	"fmt"
	"os"
	"strings"
	"unicode/utf8"
	"unsafe"
)

const BengaliPath = "/usr/share/fonts/noto/NotoSansBengali-Regular.ttf"
const JapanesePath = "/usr/share/fonts/NotoSansCJK/NotoSansCJK-Regular.ttc"
const BengaliHash = "b55c62ee531e3214da6c0701daecea89a52ba42db7d8206b92e6b51f397a3193"
const JapaneseHash = "b76b0433203017ca80401b2ee0dd69350349871c4b19d504c34dbdd80541690a"

type nativeRenderer struct {
	engine   *C.TextEngine
	identity string
}
type lineInfo struct{ RetainedBytes, FontPixels int }

func newRenderer() (renderer, error) {
	for _, font := range []struct{ path, digest string }{{BengaliPath, BengaliHash}, {JapanesePath, JapaneseHash}} {
		info, err := os.Stat(font.path)
		if err != nil || !info.Mode().IsRegular() || info.Size() > 20<<20 {
			return nil, fmt.Errorf("pinned metadata font missing or oversized at %s", font.path)
		}
		data, err := os.ReadFile(font.path)
		if err != nil {
			return nil, fmt.Errorf("read pinned metadata font %s: %w", font.path, err)
		}
		if fmt.Sprintf("%x", sha256.Sum256(data)) != font.digest {
			return nil, fmt.Errorf("pinned metadata font digest differs at %s; see protocol/text-mask.md", font.path)
		}
	}
	bn, ja := C.CString(BengaliPath), C.CString(JapanesePath)
	defer C.free(unsafe.Pointer(bn))
	defer C.free(unsafe.Pointer(ja))
	r := &nativeRenderer{engine: C.text_engine_new(bn, ja)}
	if r.engine == nil {
		return nil, fmt.Errorf("cannot initialize pinned local metadata fonts")
	}
	r.identity = fmt.Sprintf("%s/%s/pango%s/hb%s/cairo%s/fc%d/ft%d", BengaliHash, JapaneseHash, C.GoString(C.pango_version_string()), C.GoString(C.hb_version_string()), C.GoString(C.cairo_version_string()), int(C.FcGetVersion()), int(C.text_freetype_version()))
	for _, sample := range []string{"ক্ষিতিজ শ্রদ্ধা কৃষ্ণ", "夜に駆ける カタカナ"} {
		if _, err := r.Render(sample, Slots[0]); err != nil {
			r.Close()
			return nil, fmt.Errorf("verify installed font coverage: %w", err)
		}
	}
	return r, nil
}

func (r *nativeRenderer) Close() { C.text_engine_free(r.engine); r.engine = nil }
func (r *nativeRenderer) Identity() string {
	return r.identity
}
func (r *nativeRenderer) Render(value string, slot Slot) ([]byte, error) {
	data, _, err := r.renderLine(value, slot)
	return data, err
}

func (r *nativeRenderer) renderLine(value string, slot Slot) ([]byte, lineInfo, error) {
	if len(value) > 256 || !utf8.ValidString(value) || strings.IndexByte(value, 0) >= 0 || slot.Width <= 0 || slot.Width > 216 || slot.Width%2 != 0 || slot.Height <= 0 || slot.Height > 26 {
		return nil, lineInfo{}, fmt.Errorf("invalid metadata text or slot bounds")
	}
	value = strings.Map(func(c rune) rune {
		if c >= 'A' && c <= 'Z' {
			return c + ('a' - 'A')
		}
		return c
	}, value)
	text := C.CString(value)
	defer C.free(unsafe.Pointer(text))
	data := make([]byte, slot.Width*slot.Height/2)
	var cut, pixels C.int
	tail := C.int(0)
	if len(value) >= 253 {
		tail = 1
	}
	result := C.text_render(r.engine, text, C.int(slot.Width), C.int(slot.Height), tail, (*C.uchar)(unsafe.Pointer(&data[0])), &cut, &pixels)
	info := lineInfo{RetainedBytes: int(cut), FontPixels: int(pixels)}
	if result == 1 {
		return nil, info, ErrCoverage
	}
	if result != 0 {
		return nil, info, fmt.Errorf("metadata ink cannot fit supported bounds")
	}
	return data, info, nil
}
