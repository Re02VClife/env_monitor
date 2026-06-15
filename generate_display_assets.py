#!/usr/bin/env python3
"""
显示素材生成器 — 为直接帧缓冲绘制生成位图数据
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
# 素材定义: (变量名, 文本, 字号, 字色, 是否抗锯齿)
# ============================================================
ASSETS = [
    # 标签（中文+英文）
    ("label_co2",  "CO2",          24,  True),
    ("label_temp", "温度",         28,  True),
    ("label_humi", "湿度",         28,  True),
    ("label_ppm",  "ppm",          24,  True),
    ("label_degc", "°C",           24,  True),
    ("label_pct",  "%",            24,  True),
    ("label_good", "良好",         24,  True),
    ("label_warn", "一般",         24,  True),
    ("label_bad",  "差",           24,  True),
    # 数字 (48px，统一用于所有数值显示)
    ("digit_0", "0", 48, True),
    ("digit_1", "1", 48, True),
    ("digit_2", "2", 48, True),
    ("digit_3", "3", 48, True),
    ("digit_4", "4", 48, True),
    ("digit_5", "5", 48, True),
    ("digit_6", "6", 48, True),
    ("digit_7", "7", 48, True),
    ("digit_8", "8", 48, True),
    ("digit_9", "9", 48, True),
    ("digit_dot", ".", 48, True),
]


def render_text(text, size, antialias):
    """渲染文本为 4bpp 灰度位图，返回 (width, height, bytes)"""
    font = ImageFont.truetype(FONT_PATH, size)
    # 计算实际边界
    bbox = font.getbbox(text)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    if tw <= 0 or th <= 0:
        return 1, 1, b'\x00'

    # 创建画布 (4bpp = 16 灰度级)
    img = Image.new('L', (tw, th), 0)
    draw = ImageDraw.Draw(img)
    draw.text((-bbox[0], -bbox[1]), text, font=font, fill=255)

    # 量化为 4bpp: 0-15
    pixels = list(img.getdata())
    buf = bytearray()
    row_bytes = (tw + 1) // 2  # 每行字节数 (2像素/字节)
    for y in range(th):
        row = bytearray(row_bytes)
        for x in range(tw):
            px = pixels[y * tw + x]
            level = (px * 15 + 127) // 255  # 0-255 -> 0-15
            if x & 1:
                row[x // 2] |= level
            else:
                row[x // 2] = level << 4
        buf.extend(row)
    return tw, th, bytes(buf)


def generate():
    font = ImageFont.truetype(FONT_PATH, 16)
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    assets_data = []
    for name, text, size, aa in ASSETS:
        w, h, data = render_text(text, size, aa)
        assets_data.append((name, w, h, data))
        print(f"  {name}: '{text}' {w}x{h}  {len(data)} bytes")

    # === 生成头文件 ===
    with open(OUTPUT_H, 'w', encoding='utf-8') as f:
        f.write('// 自动生成 — 显示素材位图 (4bpp 灰度)\n')
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
        # 便捷宏：按名称获取位图
        f.write('// 便捷宏\n')
        f.write('#define BMP(name) (&bmp_##name)\n')

    # === 生成 C 文件 ===
    with open(OUTPUT_C, 'w', encoding='utf-8') as f:
        f.write('// 自动生成 — 显示素材位图 (4bpp 灰度)\n')
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
