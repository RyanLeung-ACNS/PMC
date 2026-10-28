# Builds an 8x16 font from LVGL's unscii 8 (public domain) by doubling each row: the tall
# text-mode look, for the start-up screen (0.13.10). Usage, from the repo root:
#   python3 tools/fonts/mkdos.py InitialTesting/lvgl_940/src/font/lv_font_unscii_8.c InitialTesting/ui_font_dos16.c
import re, sys
src = open(sys.argv[1]).read()
bm = src[src.index('glyph_bitmap[] = {'):src.index('};', src.index('glyph_bitmap[] = {'))]
data = [int(h, 16) for h in re.findall(r'0x([0-9a-fA-F]+)', bm)]
dsc = re.findall(r'\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}', src)
out_bm, out_dsc = [], []
for idx, adv, w, h, ox, oy in dsc:
    idx, adv, w, h, ox, oy = map(int, (idx, adv, w, h, ox, oy))
    bits = [(data[idx + i // 8] >> (7 - i % 8)) & 1 for i in range(w * h)] if w * h else []
    rows = [bits[r * w:(r + 1) * w] for r in range(h)]
    nb = [b for r in rows for b in r + r]
    start = len(out_bm)
    for i in range(0, len(nb), 8):
        chunk = nb[i:i + 8] + [0] * (8 - len(nb[i:i + 8]))
        out_bm.append(sum(b << (7 - j) for j, b in enumerate(chunk)))
    out_dsc.append((start if w * h else 0, adv, w, h * 2, ox, oy * 2))
cm = re.search(r'static const lv_font_fmt_txt_cmap_t cmaps\[\] =\s*\{(.*?)\};', src, re.S).group(1)
o = []
o.append('/* 8x16 text-mode font: LVGL\'s unscii 8 (public domain) with every row doubled\n'
         ' * (built by tools/fonts/mkdos.py). ASCII 0x20-0x7F, 1 bpp. */\n')
o.append('#include "lvgl.h"\n\nstatic LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n')
for i in range(0, len(out_bm), 16):
    o.append('    ' + ', '.join('0x%02x' % b for b in out_bm[i:i + 16]) + ',\n')
o.append('};\n\nstatic const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n')
for d in out_dsc:
    o.append('    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d},\n' % d)
o.append('};\n\nstatic const lv_font_fmt_txt_cmap_t cmaps[] = {' + cm + '};\n\n')
o.append('''static const lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
};

const lv_font_t ui_font_dos16 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 18,
    .base_line = 0,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = 0,
    .underline_thickness = 0,
    .dsc = &font_dsc,
};
''')
open(sys.argv[2], 'w').write(''.join(o))
print(len(out_bm), 'bytes of bitmap,', len(out_dsc), 'glyphs')
