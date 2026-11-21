"""star.png (логотип Nexi) → nexi_star.h: картинка LVGL 9 (RGB565 + альфа) для экрана колонки."""
import sys
from pathlib import Path
from PIL import Image

src = Path(sys.argv[1]); out = Path(sys.argv[2]); size = int(sys.argv[3]) if len(sys.argv) > 3 else 120
im = Image.open(src).convert("RGBA").resize((size, size), Image.LANCZOS)
rgb, alpha = bytearray(), bytearray()
for r, g, b, a in im.getdata():
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    rgb += bytes((v & 0xFF, v >> 8))
    alpha.append(a)
data = rgb + alpha
lines = [", ".join(f"0x{x:02x}" for x in data[i:i + 24]) for i in range(0, len(data), 24)]
out.write_text(f"""// Сгенерировано station/make_star.py из star.png — звезда Nexi {size}x{size}, LVGL RGB565A8
#pragma once
#include <lvgl.h>

static const uint8_t nexi_star_map[] = {{
{",\n".join(lines)}
}};

static const lv_image_dsc_t nexi_star = {{
    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8, .w = {size}, .h = {size}, .stride = {size * 2}}},
    .data_size = sizeof(nexi_star_map),
    .data = nexi_star_map,
}};
""", encoding="utf-8")
print("ok", out, len(data))
