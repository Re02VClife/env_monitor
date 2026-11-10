#!/usr/bin/env python3
"""
显示素材生成器 — 为直接帧缓冲绘制生成 4bpp 灰度位图
输出: components/display_assets/display_assets.h + .c
用法: python generate_display_assets.py
"""
import os
from PIL import Image, ImageFont, ImageDraw

OUTPUT_DIR = "components/display_assets"
OUTPUT_H   = os.path.join(OUTPUT_DIR, "display_assets.h")
OUTPUT_C   = os.path.join(OUTPUT_DIR, "display_assets.c")

FONT_PATH = "C:/Windows/Fonts/simhei.ttf"

# ============================================================
# 素材定义: (变量名, 文本, 字号)
# ============================================================
ASSETS = [
    # ---- 标题 ----
    ("title",         "干湿提醒",   32),

    # ---- 温湿度标签 ----
    ("label_temp",    "温度",       28),
    ("label_humi",    "湿度",       28),
    ("label_degc",    "°C",         24),
    ("label_pct",     "%",          24),

    # ---- 底部辅助信息 ----
    ("label_dew",     "露点",       22),
    ("label_abs",     "绝对湿度",   22),
    ("unit_gm3",      "g/m3",       22),

    # ---- 预计干衣时间 ----
    ("label_eta",     "预计",       22),
    ("unit_hour",     "小时",       22),
    ("unit_min",      "分钟",       22),

    # ---- 干湿状态（大字） ----
    ("status_dry",    "干燥",       32),
    ("status_normal", "适宜",       32),
    ("status_damp",   "偏潮",       32),
    ("status_wet",    "潮湿",       32),

    # ---- 联网状态 ----
    ("net_online",    "已联网",     20),
    ("net_offline",   "未联网",     20),
    ("net_prov",      "配网中",     20),

    # ---- 提示 ----
    ("hint_cold",     "低温慢干",   20),
    ("hint_none",     "--",         22),

    # ---- 大号数字 (48px) ----
    ("digit_0", "0", 48),
    ("digit_1", "1", 48),
    ("digit_2", "2", 48),
    ("digit_3", "3", 48),
    ("digit_4", "4", 48),
    ("digit_5", "5", 48),
    ("digit_6", "6", 48),
    ("digit_7", "7", 48),
    ("digit_8", "8", 48),
    ("digit_9", "9", 48),
    ("digit_dot", ".", 48),
    ("digit_minus", "-", 48),

    # ---- 小号数字 (24px)，用于底部辅助信息 ----
    ("sdigit_0", "0", 24),
    ("sdigit_1", "1", 24),
    ("sdigit_2", "2", 24),
    ("sdigit_3", "3", 24),
    ("sdigit_4", "4", 24),
    ("sdigit_5", "5", 24),
    ("sdigit_6", "6", 24),
    ("sdigit_7", "7", 24),
    ("sdigit_8", "8", 24),
    ("sdigit_9", "9", 24),
    ("sdigit_dot", ".", 24),
    ("sdigit_minus", "-", 24),
]


def render_text(text, size):
    """渲染文本为 4bpp 灰度位图，返回 (width, height, bytes)"""
    font = ImageFont.truetype(FONT_PATH, size)
    bbox = font.getbbox(text)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    if tw <= 0 or th <= 0:
        return 1, 1, b'\x00'

    img = Image.new('L', (tw, th), 0)
    draw = ImageDraw.Draw(img)
    draw.text((-bbox[0], -bbox[1]), text, font=font, fill=255)

    pixels = list(img.getdata())
    buf = bytearray()
    row_bytes = (tw + 1) // 2          # 每行字节数 (2 像素/字节)
    for y in range(th):
        row = bytearray(row_bytes)
        for x in range(tw):
            level = (pixels[y * tw + x] * 15 + 127) // 255   # 0-255 -> 0-15
            if x & 1:
                row[x // 2] |= level
            else:
                row[x // 2] = level << 4
        buf.extend(row)
    return tw, th, bytes(buf)


def generate():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    assets_data = []
    for name, text, size in ASSETS:
        w, h, data = render_text(text, size)
        assets_data.append((name, w, h, data))
        print(f"  {name}: '{text}' {w}x{h}  {len(data)} bytes")

    # === 生成头文件 ===
    with open(OUTPUT_H, 'w', encoding='utf-8') as f:
        f.write('// 自动生成 — 显示素材位图 (4bpp 灰度)\n')
        f.write('// 由 generate_display_assets.py 生成，请勿手工修改\n')
        f.write('#pragma once\n')
        f.write('#include <stdint.h>\n\n')
        f.write('typedef struct {\n')
        f.write('    const uint8_t *data;\n')
        f.write('    uint16_t w;\n')
        f.write('    uint16_t h;\n')
        f.write('} asset_bitmap_t;\n\n')
        for name, w, h, _ in assets_data:
            f.write(f'extern const asset_bitmap_t bmp_{name};\n')
        f.write('\n')
        f.write('// 便捷宏\n')
        f.write('#define BMP(name) (&bmp_##name)\n')

    # === 生成 C 文件 ===
    with open(OUTPUT_C, 'w', encoding='utf-8') as f:
        f.write('// 自动生成 — 显示素材位图 (4bpp 灰度)\n')
        f.write('// 由 generate_display_assets.py 生成，请勿手工修改\n')
        f.write('#include "display_assets.h"\n\n')
        for name, w, h, data in assets_data:
            f.write(f'static const uint8_t _data_{name}[] = {{\n')
            for i in range(0, len(data), 16):
                f.write('    ' + ','.join(f'0x{b:02X}' for b in data[i:i+16]) + ',\n')
            f.write('};\n\n')
            f.write(f'const asset_bitmap_t bmp_{name} = {{\n')
            f.write(f'    .data = _data_{name},\n')
            f.write(f'    .w    = {w},\n')
            f.write(f'    .h    = {h},\n')
            f.write('};\n\n')

    total = sum(len(d) for _, _, _, d in assets_data)
    print(f"\nDone! {len(assets_data)} assets, {total} bytes total")
    print(f"  {OUTPUT_H}")
    print(f"  {OUTPUT_C}")


if __name__ == '__main__':
    generate()
