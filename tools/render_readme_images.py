#!/usr/bin/env python3
"""Renders approximate screenshots of the device screens for the README (assets/screens/*.png).

These are mock-ups drawn with the same layout constants as the firmware, not photos.
Needs the Press Start 2P font: PRESS_START_2P=/path/to/PressStart2P-Regular.ttf python3 tools/render_readme_images.py
"""
import math
import os
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "screens"
OUT.mkdir(parents=True, exist_ok=True)
FONT = os.environ.get("PRESS_START_2P", "PressStart2P-Regular.ttf")

BG, TEXT, DIM = (0, 0, 0), (255, 255, 255), (66, 65, 66)
VIOLET, GREEN, SKY = (170, 110, 255), (80, 220, 120), (90, 200, 255)
CORAL, YELLOW, PINK = (226, 80, 80), (255, 255, 0), (255, 105, 180)
CANVAS_H = 140
SCALE = 2


def new():
    im = Image.new("RGB", (240, 240), BG)
    d = ImageDraw.Draw(im)
    d.fontmode = "1"
    return im, d


def text(d, t, cx, baseline, size, col, left=False):
    f = ImageFont.truetype(FONT, 8 * size)
    w = len(t) * 8 * size
    d.text((cx if left else cx - w // 2, baseline), t, font=f, fill=col, anchor="ls")


def glyph_digits(d, s, x, y, scale, col):
    G = {"0": (7, 5, 5, 5, 7), "1": (2, 6, 2, 2, 7), "2": (7, 1, 7, 4, 7), "3": (7, 1, 7, 1, 7), "4": (5, 5, 7, 1, 1),
         "5": (7, 4, 7, 1, 7), "6": (7, 4, 7, 5, 7), "7": (7, 1, 1, 1, 1), "8": (7, 5, 7, 5, 7), "9": (7, 5, 7, 1, 7)}
    xs = [x, x + 40, None, x + 100, x + 140]
    for ch, px in zip(s, xs):
        if ch == ":":
            d.rectangle((115, y + 10, 124, y + 19), fill=col)
            d.rectangle((115, y + 30, 124, y + 39), fill=col)
            continue
        for r in range(5):
            for c in range(3):
                if G[ch][r] & (4 >> c):
                    d.rectangle((px + c * scale, y + r * scale, px + (c + 1) * scale - 1, y + (r + 1) * scale - 1), fill=col)


def icon(d, rows, cx, cy, col, s=4):
    x0, y0 = cx - 7 * s // 2, cy - 7 * s // 2
    for r, bits in enumerate(rows):
        for c in range(7):
            if bits & (0x40 >> c):
                d.rectangle((x0 + c * s, y0 + r * s, x0 + (c + 1) * s - 1, y0 + (r + 1) * s - 1), fill=col)


def save(im, name):
    im.resize((240 * SCALE, 240 * SCALE), Image.NEAREST).save(OUT / name)


# ---- Pomodoro ----------------------------------------------------------------------------------
def pomodoro():
    im, d = new()
    text(d, "FOCO", 120, 28, 2, TEXT)
    d.line((0, 38, 239, 38), fill=DIM)
    glyph_digits(d, "25:00", 35, 62, 10, TEXT)
    text(d, "LISTO", 120, 148, 2, VIOLET)
    PLAY, STOP, SKIP = (0x40, 0x60, 0x70, 0x78, 0x70, 0x60, 0x40), (0, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0), (0x42, 0x62, 0x72, 0x7A, 0x72, 0x62, 0x42)
    for i, ic in enumerate((STOP, PLAY, SKIP)):
        x = (4, 84, 164)[i]
        d.rectangle((x, 168, x + 71, 168 + 51), outline=DIM)
        d.rectangle((x + 1, 169, x + 70, 168 + 50), outline=DIM)
        icon(d, ic, x + 36, 194, TEXT)
    for i in range(4):
        d.rectangle((8 + i * 58, 228, 8 + i * 58 + 49, 233), fill=VIOLET if i < 2 else DIM)
    save(im, "pomodoro.png")


# ---- Clock -------------------------------------------------------------------------------------
def clock():
    im, d = new()
    text(d, "MARTES", 120, 34, 2, SKY)
    bell = (0x08, 0x1C, 0x1C, 0x3E, 0x3E, 0x7F, 0x08)
    icon(d, bell, 216, 22, YELLOW, 2)
    glyph_digits(d, "14:35", 35, 52, 10, TEXT)
    text(d, "17 NOV", 120, 148, 3, TEXT)
    for i, (lab, val, col) in enumerate((("EXT", "24C", (255, 170, 60)), ("HUM", "58%", SKY))):
        x = 4 + i * 118
        d.rectangle((x, 168, x + 113, 219), outline=DIM)
        d.rectangle((x + 1, 169, x + 112, 218), outline=DIM)
        text(d, lab, x + 57, 185, 1, TEXT)
        text(d, val, x + 57, 212, 3, col)
    for i in range(5):
        d.rectangle((8 + i * 46, 228, 8 + i * 46 + 39, 233), fill=GREEN if i < 4 else DIM)
    save(im, "reloj.png")


# ---- Kraken ------------------------------------------------------------------------------------
def kraken(d, ox=0, oy=0, state="idle", frame=0):
    PIX, KX, KY, col = 16, 24, 4, CORAL

    def blk(gx, gy, gw, gh, c):
        x, y = KX + ox + gx * PIX, KY + oy + gy * PIX
        d.rectangle((x, y, x + gw * PIX - 1, y + gh * PIX - 1), fill=c)

    blink = state == "sleep" or state == "happy" or (state == "idle" and frame % 7 == 6)
    blk(3, 2, 6, 4, col); blk(2, 3, 8, 2, col); blk(4, 1, 4, 1, col)
    for gx in (4, 7):
        if blink:
            blk(gx, 3, 2, 1, TEXT)
        else:
            blk(gx, 3, 2, 2, TEXT); blk(gx + 1, 3 + (frame // 2) % 2, 1, 1, BG)
    blk(5, 5, 2, 2 if state in ("talk", "happy") and frame % 2 == 0 else 1, TEXT)
    t = frame % 2
    for gx, gy in ((2, 6), (3, 7 - t), (4, 6 + t), (5, 7), (6, 6 + t), (7, 7 - t), (8, 6), (9, 7)):
        blk(gx, gy, 1, 1, col)
    if state == "think":
        blk(4 + (frame % 3) * 2, 0, 1, 1, YELLOW)
    if state == "listen":
        blk(1, 3, 1, 2, YELLOW); blk(10, 3, 1, 2, YELLOW)
    if state == "sleep":
        blk(10, 1, 1, 1, (123, 125, 123)); blk(11, 0, 1, 1, (123, 125, 123))


def lines(d, msg, color):
    words, ln, out = msg.split(), "", []
    for w in words:
        if len(ln) + len(w) + 1 > 28:
            out.append(ln); ln = w
        else:
            ln = (ln + " " + w).strip()
    out.append(ln)
    for i, l in enumerate(out[:6]):
        text(d, l, 8, CANVAS_H + 44 + i * 11, 1, color, left=True)


def kraken_screen(name, state, label, lcol, msg, mcol, frame=0, ox=0, oy=0):
    im, d = new()
    kraken(d, ox, oy, state, frame)
    text(d, label, 120, CANVAS_H + 22, 2, lcol)
    lines(d, msg, mcol)
    save(im, name)


# ---- Frida -------------------------------------------------------------------------------------
ATLAS = Image.open(ROOT / "assets" / "frida_atlas.png").convert("RGBA")
N = ATLAS.width // 3


def frida_screen(name, eye, mouth, label, lcol, msg, mcol, bars=False, hearts=False):
    im, d = new()
    tile = ATLAS.crop((mouth * N, eye * N, (mouth + 1) * N, (eye + 1) * N)).resize((N * 2, N * 2), Image.NEAREST)
    im.paste(tile, ((240 - N * 2) // 2, 1), tile)
    if bars:
        for i in range(3):
            h = (14 + i * 12) * 0.8
            for x in (38 - i * 12, 198 + i * 12):
                d.rectangle((x, 70 - h // 2, x + 3, 70 + h // 2), fill=PINK)
    if hearts:
        H = (0x1B, 0x3F, 0x3F, 0x1E, 0x0C, 0x04)
        for hx, hy in ((14, 60), (200, 40)):
            for r in range(6):
                for c in range(7):
                    if H[r] & (0x40 >> c):
                        d.rectangle((hx + c * 3, hy + r * 3, hx + c * 3 + 2, hy + r * 3 + 2), fill=(255, 0, 255))
    text(d, label, 120, CANVAS_H + 22, 2, lcol)
    lines(d, msg, mcol)
    save(im, name)


def hero():
    names = ["pomodoro", "reloj", "kraken", "frida"]
    caps = ["POMODORO", "RELOJ + CLIMA", "KRAKEN", "FRIDA"]
    W, H, S = 2200, 780, 480
    bg = Image.new("RGB", (W, H), (11, 11, 20))
    d = ImageDraw.Draw(bg)
    d.fontmode = "1"
    big = ImageFont.truetype(FONT, 40)
    small = ImageFont.truetype(FONT, 16)
    d.text((W // 2, 70), "krakbot Desktop", font=big, fill=(255, 255, 255), anchor="mm")
    d.text((W // 2, 118), "UN GADGET DE ESCRITORIO CON 4 APPS SEGUN COMO LO GIRES", font=small, fill=(150, 150, 190), anchor="mm")
    x0 = (W - (4 * S + 3 * 50)) // 2
    for i, (n, c) in enumerate(zip(names, caps)):
        x = x0 + i * (S + 50)
        frame = Image.new("RGB", (S + 24, S + 24), (40, 40, 60))
        bg.paste(frame, (x - 12, 160 - 12))
        bg.paste(Image.open(OUT / f"{n}.png"), (x, 160))
        d.text((x + S // 2, 160 + S + 46), c, font=small, fill=(255, 255, 255), anchor="mm")
    bg.save(OUT / "hero.png")


def characters():
    W, H = 1500, 610
    bg = Image.new("RGB", (W, H), (11, 11, 20))
    d = ImageDraw.Draw(bg)
    d.fontmode = "1"
    f = ImageFont.truetype(FONT, 16)
    # Kraken states
    for i, (st, cap) in enumerate((("idle", "REPOSO"), ("listen", "ESCUCHA"), ("think", "PIENSA"), ("talk", "HABLA"), ("sleep", "ZZZ"))):
        tile = Image.new("RGB", (240, 140), BG)
        td = ImageDraw.Draw(tile)
        kraken(td, 0, 0, st, 2)
        tile = tile.resize((240 * 2 // 2 + 40, 140 * 2 // 2 + 20))
        bg.paste(tile.crop((0, 0, 280, 160)).resize((280, 160)), (20 + i * 290, 70))
        d.text((160 + i * 290, 250), cap, font=f, fill=(200, 200, 220), anchor="mm")
    d.text((20, 30), "KRAKEN", font=f, fill=(226, 80, 80), anchor="lm")
    # Frida frames
    d.text((20, 300), "FRIDA", font=f, fill=(255, 105, 180), anchor="lm")
    caps = ["REPOSO", "HABLA", "RIE", "PARPADEA", "GUINO"]
    cells = [(0, 0), (0, 1), (0, 2), (1, 0), (2, 0)]
    for i, ((e, m), cap) in enumerate(zip(cells, caps)):
        t = ATLAS.crop((m * N, e * N, (m + 1) * N, (e + 1) * N)).resize((N * 3, N * 3), Image.NEAREST)
        bg.paste(t, (50 + i * 290, 330), t)
        d.text((50 + i * 290 + N * 3 // 2, 565), cap, font=f, fill=(200, 200, 220), anchor="mm")
    bg.save(OUT / "personajes.png")


if __name__ == "__main__":
    pomodoro()
    clock()
    kraken_screen("kraken.png", "idle", "LISTO", (80, 220, 120), "MANTEN [+] PARA HABLAR", DIM, frame=3)
    kraken_screen("kraken_habla.png", "talk", "HABLANDO", CORAL, "Ah, viajero! *agita tentaculos* El mar me susurra que hoy sera un gran dia.", TEXT, frame=2, ox=4)
    frida_screen("frida.png", 0, 0, "LISTO", (80, 220, 120), "MANTEN [+] PARA HABLAR", DIM)
    frida_screen("frida_habla.png", 0, 2, "HABLANDO", (255, 105, 180), "Soy Frida, la chica de pelo rosa y azul que vive aca adentro. Y vos, como te llamas?", TEXT, bars=True)
    frida_screen("frida_contenta.png", 2, 1, "HOLA!", YELLOW, "MANTEN [+] PARA HABLAR", DIM, hearts=True)
    hero()
    characters()
    print("ok")
