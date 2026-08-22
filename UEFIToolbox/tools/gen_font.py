#!/usr/bin/env python3
"""gen_font.py — 从 unifont 提取 16x16 中文字符点阵 → include/chinese_font.h

格式与 MWOS chinese_font.h 一致：glyph16_t { unicode, bitmap[32] }，
每行 2 字节、MSB 在前（16x16 位图）。
"""
from PIL import Image, ImageDraw, ImageFont

FONT = '/usr/share/fonts/truetype/unifont/unifont.ttf'
TEXT = (
    "UEFI工具箱系统信息启动项内存测试磁盘关机重启返回确定固件版本分辨率"
    "按上下键选择回车确认ESC返回未检测到字节块设备PCI写入读取通过失败"
    "正在测试地址扇区大小个数设备启动顺序项名称架构频率核心缓存网络"
    "接口错误代码进度完成帮助关于电源状态温度电压模式工具内存大小总数"
    "可用已用容量读取测试通过率点击运行当前模式刷新进度条驱动列表"
)

chars = sorted(set(c for c in TEXT if ord(c) > 0x7F))
font = ImageFont.truetype(FONT, 16)

lines = []
lines.append("#ifndef UEFI_TOOLBOX_CHINESE_FONT_H")
lines.append("#define UEFI_TOOLBOX_CHINESE_FONT_H")
lines.append("")
lines.append("#include <stdint.h>")
lines.append("")
lines.append("/* 16x16 中文点阵（unifont 提取，2 字节/行，MSB 在前） */")
lines.append("typedef struct { uint16_t unicode; uint8_t bitmap[32]; } glyph16_t;")
lines.append("")
lines.append("static const glyph16_t chinese_font[] = {")

for ch in chars:
    img = Image.new('1', (16, 16), 0)
    d = ImageDraw.Draw(img)
    d.text((0, 0), ch, font=font, fill=1)
    rows = []
    for y in range(16):
        b1 = b2 = 0
        for x in range(16):
            if img.getpixel((x, y)):
                if x < 8:
                    b1 |= 1 << (7 - x)
                else:
                    b2 |= 1 << (15 - x)
        rows.append("0x%02X,0x%02X" % (b1, b2))
    lines.append("    { 0x%04X, { %s } }," % (ord(ch), ", ".join(rows)))

lines.append("};")
lines.append("")
lines.append("#endif /* UEFI_TOOLBOX_CHINESE_FONT_H */")

out = '\n'.join(lines) + '\n'
open('include/chinese_font.h', 'w', encoding='utf-8').write(out)
print("生成 %d 个中文字形 -> include/chinese_font.h" % len(chars))
