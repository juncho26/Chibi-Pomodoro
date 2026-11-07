#include "textfit.h"
#include <string.h>
#include "src/misc/lv_text_private.h"

extern const lv_font_t font_jp13, font_jp16, font_jp19, font_jp22;
const lv_font_t *const TEXTFIT_FONTS[4] = { &font_jp13, &font_jp16, &font_jp19, &font_jp22 };

// Mirrors lv_label's LV_LABEL_LONG_DOT path (refr_text + lv_label_get_letter_on) for a
// one-line, left-aligned label with no letter or line spacing.
int textfit_keep(int lvl, const char *txt)
{
    if (lvl < 0) lvl = 0;
    if (lvl > 3) lvl = 3;
    const lv_font_t *f = TEXTFIT_FONTS[lvl];
    const int32_t w = textfit_width(lvl), lh = lv_font_get_line_height(f);
    lv_point_t size;
    lv_text_get_size(&size, txt, f, 0, 0, w, LV_TEXT_FLAG_NONE);
    if (size.y <= lh) return -1;                                   // one line: no dots
    if (lv_text_get_encoded_length(txt) <= 3) return -1;           // never all dots

    const int32_t px = w - lv_font_get_glyph_width(f, '.', '.') * 3;
    uint32_t nls = lv_text_get_next_line(txt, f, 0, w, NULL, LV_TEXT_FLAG_BREAK_ALL);
    { uint32_t t = nls; uint32_t c = lv_text_encoded_prev(txt, &t); if (c != '\n' && txt[nls] == '\0') nls++; }
    uint32_t i = 0, i_act = 0; int32_t x = 0;
    while (i < nls) {
        uint32_t letter, next;
        lv_text_encoded_letter_next_2(txt, &letter, &next, &i);
        int32_t gw = lv_font_get_glyph_width(f, letter, next);
        if (px < x + gw || i == nls || txt[i_act] == '\0') { i = i_act; break; }
        x += gw; i_act = i;
    }
    uint32_t letter_id = lv_text_encoded_get_char_id(txt, i);
    size_t len = strlen(txt);
    uint32_t byte_id = lv_text_encoded_get_byte_id(txt, letter_id);
    while (byte_id + 3 > len) { lv_text_encoded_prev(txt, &byte_id); letter_id--; }
    return (int)letter_id;
}
