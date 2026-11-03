# -*- coding: utf-8 -*-
"""
Generates high-tech icon for ONLOI Firmware Studio (Build, Flash & Serial Monitor)
"""
import os
from PIL import Image, ImageDraw, ImageFont

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

def create_firmware_studio_icon():
    size = 256
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Outer rounded background
    pad = 8
    # Dark blue-gray background
    draw.rounded_rectangle([pad, pad, size - pad, size - pad], radius=44, fill="#0F172A", outline="#0EA5E9", width=6)
    
    # Inner border glow
    draw.rounded_rectangle([pad+8, pad+8, size - pad - 8, size - pad - 8], radius=36, outline="#1E293B", width=3)

    # Draw Chip Body in center
    cx, cy = size // 2, size // 2 - 10
    chip_w, chip_h = 100, 100
    x0, y0 = cx - chip_w // 2, cy - chip_h // 2
    x1, y1 = cx + chip_w // 2, cy + chip_h // 2
    
    # Chip Pins (top, bottom, left, right)
    pin_color = "#38BDF8"
    for i in [-30, -10, 10, 30]:
        # Left pins
        draw.line([(x0 - 16, cy + i), (x0, cy + i)], fill=pin_color, width=4)
        # Right pins
        draw.line([(x1, cy + i), (x1 + 16, cy + i)], fill=pin_color, width=4)
        # Top pins
        draw.line([(cx + i, y0 - 16), (cx + i, y0)], fill=pin_color, width=4)
        # Bottom pins
        draw.line([(cx + i, y1), (cx + i, y1 + 16)], fill=pin_color, width=4)

    # Chip rectangle
    draw.rounded_rectangle([x0, y0, x1, y1], radius=14, fill="#1E293B", outline="#06B6D4", width=5)

    # Lightning / Flash Symbol in Chip Center
    flash_points = [
        (cx + 4, cy - 32),
        (cx - 16, cy - 2),
        (cx - 2, cy - 2),
        (cx - 6, cy + 30),
        (cx + 18, cy + 2),
        (cx + 2, cy + 2),
    ]
    draw.polygon(flash_points, fill="#F59E0B", outline="#FBBF24")

    # Bottom Text banner "STUDIO"
    bar_y0, bar_y1 = size - 64, size - 22
    draw.rounded_rectangle([32, bar_y0, size - 32, bar_y1], radius=10, fill="#0284C7", outline="#38BDF8", width=2)
    
    # Draw simple "FW" or "STUDIO" letters manually for sharpness
    try:
        font = ImageFont.truetype("arialbd.ttf", 22)
        draw.text((size // 2, (bar_y0 + bar_y1) // 2), "STUDIO", fill="#FFFFFF", font=font, anchor="mm")
    except Exception:
        draw.text((size // 2 - 34, bar_y0 + 7), "STUDIO", fill="#FFFFFF")

    # Save PNG and multi-size ICO
    png_path = os.path.join(SCRIPT_DIR, "studio_icon.png")
    ico_path = os.path.join(SCRIPT_DIR, "studio_icon.ico")
    
    img.save(png_path, "PNG")
    
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    img.save(ico_path, format="ICO", sizes=sizes)
    print(f"Icon created successfully: {ico_path}")

if __name__ == "__main__":
    create_firmware_studio_icon()
