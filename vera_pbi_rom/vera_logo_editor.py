#!/usr/bin/env python3
"""
vera_logo_editor.py — 80×30 ASCII-art / logo editor for VeraX16 PBI.

Usage:
    python3 vera_logo_editor.py <font.bin> [project.logo.json]

Controls:
    Left-click / drag      paint current char+colors onto canvas
    Right-click            eyedropper (pick char+colors from cell)
    Char picker            click a glyph to select it
    FG / BG swatches       click to set foreground / background color
    Inverse checkbox       toggle inverse attribute
    Ctrl+S                 save project
    Ctrl+E / F5            export logo-ansi.h
    Ctrl+Z                 undo last paint stroke
    F                      fill entire canvas with current char+colors

Font file sizes accepted:
    1024 B  →  128 chars × 8 B  (8-pixel glyphs, partial charset)
    2048 B  →  256 chars × 8 B  (8-pixel glyphs, full charset)
    4096 B  →  256 chars × 16 B (16-pixel glyphs, full charset)
"""

import sys
import os
import json
import argparse
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox

try:
    from PIL import Image, ImageDraw, ImageTk
except ImportError:
    sys.exit("PIL/Pillow required:  pip install Pillow")

# ─── VERA 16-color palette ───────────────────────────────────────────────────

VERA_RGB = [
    (0x00, 0x00, 0x00),  #  0 black
    (0xFF, 0xFF, 0xFF),  #  1 white
    (0x88, 0x00, 0x00),  #  2 red
    (0xAA, 0xFF, 0xEE),  #  3 cyan
    (0xCC, 0x44, 0xCC),  #  4 purple
    (0x00, 0xCC, 0x55),  #  5 green
    (0x00, 0x00, 0xAA),  #  6 blue
    (0xFF, 0xFF, 0x55),  #  7 yellow
    (0xDD, 0x88, 0x55),  #  8 orange
    (0x66, 0x44, 0x00),  #  9 brown
    (0xFF, 0x77, 0x77),  # 10 light red
    (0x33, 0x33, 0x33),  # 11 dark grey
    (0x77, 0x77, 0x77),  # 12 grey
    (0xAA, 0xFF, 0x66),  # 13 light green
    (0x00, 0x88, 0xFF),  # 14 light blue
    (0xBB, 0xBB, 0xBB),  # 15 light grey
]
VERA_NAMES = [
    'Black', 'White', 'Red', 'Cyan', 'Purple', 'Green', 'Blue', 'Yellow',
    'Orange', 'Brown', 'Lt.Red', 'Dk.Grey', 'Grey', 'Lt.Green', 'Lt.Blue', 'Lt.Grey',
]
VERA_HEX = ['#%02X%02X%02X' % c for c in VERA_RGB]

# VERA index → ANSI fg / bg codes  (30-37 normal, 90-97 bright)
VERA_ANSI_FG = [30, 37, 31, 36, 35, 32, 34, 33, 33, 33, 91, 90, 37, 92, 94, 97]
VERA_ANSI_BG = [40, 47, 41, 46, 45, 42, 44, 43, 43, 43, 101, 100, 47, 102, 104, 107]

# ─── layout constants ────────────────────────────────────────────────────────

COLS        = 80
ROWS        = 30
CELL_W      = 8         # font pixel width (always 8)
CELL_SCALE        = 2   # 2× rendering of each glyph on canvas
CELL_BORDER       = 1   # black gap between adjacent cells (pixels)
SWATCH_SZ         = 20  # color swatch pixels in palette
PICKER_COLS       = 16  # chars per row in the glyph picker
PICKER_GLYPH_SCALE = 2  # picker glyphs always at 2× regardless of canvas scale
PICKER_SEP        = 1   # 1px separator between picker glyphs

C_BG      = '#1C1C2E'
C_PANEL   = '#14141E'
C_SEL     = '#FF8800'
C_BORDER  = '#444444'

# ─── Cell ────────────────────────────────────────────────────────────────────

class Cell:
    __slots__ = ('char', 'fg', 'bg', 'inv')

    def __init__(self, char=0x20, fg=1, bg=6, inv=False):
        self.char = char
        self.fg   = fg
        self.bg   = bg
        self.inv  = inv

    def copy(self):
        return Cell(self.char, self.fg, self.bg, self.inv)

    def to_dict(self):
        return {'ch': self.char, 'fg': self.fg, 'bg': self.bg, 'inv': int(self.inv)}

    @classmethod
    def from_dict(cls, d):
        return cls(d.get('ch', 0x20), d.get('fg', 1), d.get('bg', 6), bool(d.get('inv', 0)))

    def equals(self, other):
        return (self.char == other.char and self.fg == other.fg
                and self.bg == other.bg and self.inv == other.inv)

# ─── Font loading ─────────────────────────────────────────────────────────────

def load_font(path):
    """Load a binary font file.

    Sizes (matching vera_font_editor.py convention):
        1024 B  →  128 chars × 8 B  →  8-pixel tall glyphs
        2048 B  →  128 chars × 16 B →  16-pixel tall glyphs
        2048 B  →  256 chars × 8 B  →  only if explicitly flagged (rare)

    The two 128-char files (font8x8.bin / font8x16.bin) cover chars 0-127.
    Chars 128-255 are padded with blank glyphs.
    """
    data = Path(path).read_bytes()
    size = len(data)
    if size == 1024:
        n_chars, glyph_h = 128, 8
    elif size == 2048:
        n_chars, glyph_h = 128, 16
    elif size == 4096:
        n_chars, glyph_h = 256, 16
    else:
        # Fallback: treat as 128-char font
        glyph_h = size // 128
        n_chars  = 128
        print(f'Warning: unusual font size {size} B; guessing {n_chars} chars × {glyph_h} B')
    glyphs = [data[i * glyph_h:(i + 1) * glyph_h] for i in range(n_chars)]
    # Pad to 256: chars not in the file render as blank
    blank = bytes(glyph_h)
    while len(glyphs) < 256:
        glyphs.append(blank)
    return glyphs, glyph_h

# ─── Glyph rendering (PIL) ────────────────────────────────────────────────────

def render_cell(glyphs, glyph_h, char_idx, fg, bg, inv, scale=1):
    """Return a PIL Image of size (8*scale, glyph_h*scale)."""
    fg_rgb = VERA_RGB[bg if inv else fg]
    bg_rgb = VERA_RGB[fg if inv else bg]
    w = CELL_W * scale
    h = glyph_h * scale
    img = Image.new('RGB', (w, h), bg_rgb)
    draw = ImageDraw.Draw(img)
    for row, byte in enumerate(glyphs[char_idx]):
        for col in range(8):
            if byte & (0x80 >> col):
                x0, y0 = col * scale, row * scale
                draw.rectangle([x0, y0, x0 + scale - 1, y0 + scale - 1], fill=fg_rgb)
    return img

# ─── LogoEditor ───────────────────────────────────────────────────────────────

class LogoEditor:
    PICKER_ROWS = 256 // PICKER_COLS  # 16

    def __init__(self, root, font_path, project_path=None):
        self.root = root
        root.title('VeraX16 Logo Editor — 80×30')
        root.configure(bg=C_BG)

        self.glyphs, self.glyph_h = load_font(font_path)
        self.cell_h = self.glyph_h

        # Choose scale: use CELL_SCALE (2) only when the screen is tall enough.
        # Estimate minimum required height: canvas + ~160px chrome (menu/status/
        # window decorations). If the screen can't fit it, fall back to scale 1.
        CHROME_H = 160
        required_h = ROWS * (self.cell_h * CELL_SCALE + CELL_BORDER) + CHROME_H
        screen_h   = root.winfo_screenheight()
        self.scale = CELL_SCALE if screen_h >= required_h else 1

        # Displayed cell stride: glyph at scale× + 2px black border gap
        self.stride_w = CELL_W      * self.scale + CELL_BORDER
        self.stride_h = self.cell_h * self.scale + CELL_BORDER

        # Full canvas pixel size (black background = borders)
        self.canvas_w = COLS * self.stride_w
        self.canvas_h = ROWS * self.stride_h

        # Picker stride (always 2× + 1px separator)
        self.picker_stride_x = CELL_W      * PICKER_GLYPH_SCALE + PICKER_SEP
        self.picker_stride_y = self.cell_h * PICKER_GLYPH_SCALE + PICKER_SEP

        # Grid + undo
        self.grid  = [[Cell() for _ in range(COLS)] for _ in range(ROWS)]
        self._undo = None   # single-level undo snapshot

        # Editor state
        self.cur_char  = 0x41  # 'A'
        self.cur_fg    = 1     # white
        self.cur_bg    = 6     # blue
        self.cur_inv   = tk.BooleanVar(value=False)

        # Canvas keyboard cursor
        self.cursor_row = 0
        self.cursor_col = 0

        self.project_file = None
        self.dirty = False
        self._dragging = False

        # PIL image cache for the canvas (full 640×N image)
        self._canvas_img  = None   # PIL Image
        self._canvas_tk   = None   # ImageTk.PhotoImage
        self._dirty_cells = set()  # (row, col) to redraw

        # PIL image for the glyph picker
        self._picker_img  = None
        self._picker_tk   = None
        self._picker_dirty = True

        self._build_ui()
        self._rebuild_canvas_full()
        self._rebuild_picker()
        self._update_preview()

        root.bind('<Control-s>', lambda e: self._cmd_save())
        root.bind('<Control-e>', lambda e: self._cmd_export())
        root.bind('<F5>',        lambda e: self._cmd_export())
        root.bind('<Control-z>', lambda e: self._cmd_undo())
        root.bind('f',           lambda e: self._cmd_fill())
        root.bind('F',           lambda e: self._cmd_fill())

        # Canvas cursor navigation
        root.bind('<Left>',      lambda e: self._move_cursor(0, -1))
        root.bind('<Right>',     lambda e: self._move_cursor(0,  1))
        root.bind('<Up>',        lambda e: self._move_cursor(-1, 0))
        root.bind('<Down>',      lambda e: self._move_cursor( 1, 0))
        root.bind('<space>',     lambda e: self._paint_at_cursor())

        if project_path:
            self._load_project(project_path)

    # ── UI construction ───────────────────────────────────────────────────────

    def _build_ui(self):
        root = self.root

        # ── Menu ──────────────────────────────────────────────────────────────
        menubar = tk.Menu(root)
        root.config(menu=menubar)
        fm = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label='File', menu=fm)
        fm.add_command(label='New',        command=self._cmd_new,     accelerator='')
        fm.add_command(label='Open…',      command=self._cmd_open,    accelerator='')
        fm.add_command(label='Save',       command=self._cmd_save,    accelerator='Ctrl+S')
        fm.add_command(label='Save As…',   command=self._cmd_save_as, accelerator='')
        fm.add_separator()
        fm.add_command(label='Export C header (logo-ansi.h)…',
                       command=self._cmd_export, accelerator='Ctrl+E / F5')
        fm.add_separator()
        fm.add_command(label='Quit', command=root.quit)

        em = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label='Edit', menu=em)
        em.add_command(label='Undo',            command=self._cmd_undo, accelerator='Ctrl+Z')
        em.add_command(label='Fill canvas',     command=self._cmd_fill, accelerator='F')
        em.add_command(label='Clear canvas',    command=self._cmd_clear)

        # ── Main layout ───────────────────────────────────────────────────────
        # Left: canvas.  Right: sidebar.
        main = tk.Frame(root, bg=C_BG)
        main.pack(fill='both', expand=True, padx=4, pady=4)

        # Canvas frame — with scrollbars (canvas may be >screen size at 2x)
        cf = tk.LabelFrame(main,
                           text=f'Canvas  80×30  ({self.scale}× + {CELL_BORDER}px border)',
                           bg=C_BG, fg='#AAAAAA', font=('monospace', 9))
        cf.pack(side='left', anchor='nw', fill='both', expand=True)

        # Viewport: cap display size so the window fits on screen
        scr_w = self.root.winfo_screenwidth()
        scr_h = self.root.winfo_screenheight()
        view_w = min(self.canvas_w, scr_w - 280)
        view_h = min(self.canvas_h, scr_h - 120)

        cf_inner = tk.Frame(cf, bg=C_BG)
        cf_inner.pack(fill='both', expand=True)

        hbar = tk.Scrollbar(cf_inner, orient='horizontal')
        hbar.pack(side='bottom', fill='x')
        vbar = tk.Scrollbar(cf_inner, orient='vertical')
        vbar.pack(side='right', fill='y')

        self.tk_canvas = tk.Canvas(
            cf_inner,
            width=view_w, height=view_h,
            scrollregion=(0, 0, self.canvas_w, self.canvas_h),
            xscrollcommand=hbar.set, yscrollcommand=vbar.set,
            bg='black', cursor='crosshair',
            highlightthickness=1, highlightbackground=C_BORDER)
        self.tk_canvas.pack(side='left', fill='both', expand=True)

        hbar.config(command=self.tk_canvas.xview)
        vbar.config(command=self.tk_canvas.yview)

        self.tk_canvas.bind('<ButtonPress-1>',   self._on_canvas_press)
        self.tk_canvas.bind('<ButtonRelease-1>', self._on_canvas_release)
        self.tk_canvas.bind('<B1-Motion>',       self._on_canvas_drag)
        self.tk_canvas.bind('<Button-3>',        self._on_canvas_pick)
        # Mouse-wheel scroll
        self.tk_canvas.bind('<MouseWheel>',
            lambda e: self.tk_canvas.yview_scroll(-1 if e.delta > 0 else 1, 'units'))
        self.tk_canvas.bind('<Shift-MouseWheel>',
            lambda e: self.tk_canvas.xview_scroll(-1 if e.delta > 0 else 1, 'units'))

        # Sidebar
        sb = tk.Frame(main, bg=C_PANEL, padx=6, pady=4)
        sb.pack(side='left', fill='y', padx=(6, 0))

        self._build_sidebar(sb)

        # Status bar
        self.status_var = tk.StringVar(value='Ready')
        status = tk.Label(root, textvariable=self.status_var, anchor='w',
                          bg='#111111', fg='#888888', font=('monospace', 9))
        status.pack(fill='x', side='bottom', padx=4, pady=(0, 2))

    def _build_sidebar(self, parent):
        # ── Char picker ───────────────────────────────────────────────────────
        tk.Label(parent, text='CHARACTER', bg=C_PANEL, fg='#AAAAAA',
                 font=('sans', 8, 'bold')).pack(anchor='w')

        # Picker always at 2× + 1px separators
        picker_w = PICKER_COLS      * self.picker_stride_x
        picker_h = self.PICKER_ROWS * self.picker_stride_y
        # Show at most 8 rows visible; scroll via scrollbar
        vis_rows = min(self.PICKER_ROWS, 8)
        vis_h    = vis_rows * self.picker_stride_y

        pf = tk.Frame(parent, bg=C_PANEL)
        pf.pack(anchor='w')

        pbar = tk.Scrollbar(pf, orient='vertical')
        pbar.pack(side='right', fill='y')

        self.tk_picker = tk.Canvas(pf, width=picker_w, height=vis_h,
                                   bg='#161616', yscrollcommand=pbar.set,
                                   scrollregion=(0, 0, picker_w, picker_h),
                                   highlightthickness=1, highlightbackground=C_BORDER,
                                   cursor='hand2')
        self.tk_picker.pack(side='left')
        pbar.config(command=self.tk_picker.yview)
        self.tk_picker.bind('<Button-1>', self._on_picker_click)
        self.tk_picker.bind('<MouseWheel>',
                            lambda e: self.tk_picker.yview_scroll(-1 if e.delta > 0 else 1, 'units'))

        # Highlight rect for selected char
        self._picker_sel_rect = self.tk_picker.create_rectangle(
            0, 0, 0, 0, outline=C_SEL, width=2)

        tk.Frame(parent, height=6, bg=C_PANEL).pack()

        # ── FG / BG palettes ──────────────────────────────────────────────────
        for label, attr in (('FOREGROUND', 'fg'), ('BACKGROUND', 'bg')):
            tk.Label(parent, text=label, bg=C_PANEL, fg='#AAAAAA',
                     font=('sans', 8, 'bold')).pack(anchor='w')
            pf2 = tk.Frame(parent, bg=C_PANEL)
            pf2.pack(anchor='w')
            btns = []
            for i, hex_col in enumerate(VERA_HEX):
                col = i % 8
                row = i // 8
                b = tk.Label(pf2, bg=hex_col, width=2, height=1,
                             relief='flat', cursor='hand2')
                b.grid(row=row, column=col, padx=1, pady=1)
                idx = i
                b.bind('<Button-1>', lambda e, a=attr, n=idx: self._set_color(a, n))
                btns.append(b)
            setattr(self, f'_{attr}_btns', btns)
            self._update_palette_sel(attr)
            tk.Frame(parent, height=4, bg=C_PANEL).pack()

        # ── Inverse ───────────────────────────────────────────────────────────
        inv_cb = tk.Checkbutton(parent, text=' Inverse', variable=self.cur_inv,
                                bg=C_PANEL, fg='#CCCCCC', selectcolor='#333355',
                                activebackground=C_PANEL, font=('monospace', 10),
                                command=self._update_preview)
        inv_cb.pack(anchor='w')
        tk.Frame(parent, height=4, bg=C_PANEL).pack()

        # ── Preview ───────────────────────────────────────────────────────────
        tk.Label(parent, text='PREVIEW', bg=C_PANEL, fg='#AAAAAA',
                 font=('sans', 8, 'bold')).pack(anchor='w')
        self._prev_scale = 4
        pw = CELL_W * self._prev_scale
        ph = self.glyph_h * self._prev_scale
        self.tk_preview = tk.Canvas(parent, width=pw, height=ph,
                                    bg='black', highlightthickness=1,
                                    highlightbackground=C_BORDER)
        self.tk_preview.pack(anchor='w')
        self.tk_preview_img = None

        tk.Frame(parent, height=4, bg=C_PANEL).pack()
        self.preview_info = tk.Label(parent, text='', bg=C_PANEL, fg='#AAAAAA',
                                     font=('monospace', 9), justify='left')
        self.preview_info.pack(anchor='w')

    # ── PIL rendering ─────────────────────────────────────────────────────────

    def _render_cell_pil(self, img, row, col, cell, is_cursor=False):
        """Paint one cell into the PIL Image at scale× with border gap.

        If is_cursor, overlay an orange 2-px border to show the keyboard cursor.
        """
        x = col * self.stride_w
        y = row * self.stride_h
        gw = CELL_W      * self.scale
        gh = self.cell_h * self.scale
        cell_img = render_cell(self.glyphs, self.glyph_h,
                               cell.char, cell.fg, cell.bg, cell.inv,
                               scale=self.scale)
        img.paste(cell_img, (x, y))
        if is_cursor:
            draw = ImageDraw.Draw(img)
            draw.rectangle([x, y, x + gw - 1, y + gh - 1],
                           outline=(255, 200, 0), width=max(1, self.scale))

    def _rebuild_canvas_full(self):
        """Redraw the entire canvas PIL image."""
        img = Image.new('RGB', (self.canvas_w, self.canvas_h), (0, 0, 0))
        for row in range(ROWS):
            for col in range(COLS):
                is_cur = (row == self.cursor_row and col == self.cursor_col)
                self._render_cell_pil(img, row, col, self.grid[row][col], is_cur)
        self._canvas_img = img
        self._flush_canvas()

    def _redraw_dirty(self):
        """Redraw only cells in _dirty_cells."""
        if not self._dirty_cells:
            return
        img = self._canvas_img
        for (row, col) in self._dirty_cells:
            is_cur = (row == self.cursor_row and col == self.cursor_col)
            self._render_cell_pil(img, row, col, self.grid[row][col], is_cur)
        self._dirty_cells.clear()
        self._flush_canvas()

    def _flush_canvas(self):
        """Push PIL image to tk canvas."""
        self._canvas_tk = ImageTk.PhotoImage(self._canvas_img)
        self.tk_canvas.delete('img')
        self.tk_canvas.create_image(0, 0, anchor='nw', image=self._canvas_tk, tags='img')

    def _rebuild_picker(self):
        """Render the character picker at 2× with 1px separators."""
        gw = CELL_W      * PICKER_GLYPH_SCALE   # glyph width in pixels
        gh = self.cell_h * PICKER_GLYPH_SCALE   # glyph height in pixels
        sx = self.picker_stride_x               # stride incl. separator
        sy = self.picker_stride_y
        img_w = PICKER_COLS      * sx
        img_h = self.PICKER_ROWS * sy
        # Dark background fills the separator gaps
        img = Image.new('RGB', (img_w, img_h), (22, 22, 22))
        for ci in range(256):
            row = ci // PICKER_COLS
            col = ci  % PICKER_COLS
            cell_img = render_cell(self.glyphs, self.glyph_h,
                                   ci, self.cur_fg, self.cur_bg,
                                   self.cur_inv.get(),
                                   scale=PICKER_GLYPH_SCALE)
            img.paste(cell_img, (col * sx, row * sy))
        self._picker_img = img
        self._picker_tk = ImageTk.PhotoImage(img)
        self.tk_picker.delete('img')
        self.tk_picker.create_image(0, 0, anchor='nw', image=self._picker_tk, tags='img')
        self._update_picker_sel()
        self._picker_dirty = False

    def _update_picker_sel(self):
        gw = CELL_W      * PICKER_GLYPH_SCALE
        gh = self.cell_h * PICKER_GLYPH_SCALE
        ci  = self.cur_char
        col = ci % PICKER_COLS
        row = ci // PICKER_COLS
        x0 = col * self.picker_stride_x
        y0 = row * self.picker_stride_y
        self.tk_picker.coords(self._picker_sel_rect,
                              x0, y0, x0 + gw, y0 + gh)

    def _update_preview(self):
        img = render_cell(self.glyphs, self.glyph_h,
                          self.cur_char, self.cur_fg, self.cur_bg,
                          self.cur_inv.get(), scale=self._prev_scale)
        self._prev_tk = ImageTk.PhotoImage(img)
        self.tk_preview.delete('all')
        self.tk_preview.create_image(0, 0, anchor='nw', image=self._prev_tk)
        ch = self.cur_char
        label = f'#{ch:02X}  {chr(ch) if 0x20 <= ch < 0x7F else "?"}'
        self.preview_info.config(
            text=f'{label}\nfg:{self.cur_fg} {VERA_NAMES[self.cur_fg]}\n'
                 f'bg:{self.cur_bg} {VERA_NAMES[self.cur_bg]}\n'
                 f'inv:{self.cur_inv.get()}')

    def _update_palette_sel(self, attr):
        btns = getattr(self, f'_{attr}_btns')
        cur  = self.cur_fg if attr == 'fg' else self.cur_bg
        for i, b in enumerate(btns):
            b.config(relief='sunken' if i == cur else 'flat',
                     bd=3 if i == cur else 1)

    def _update_status(self):
        self.status_var.set(
            f'char:#{ self.cur_char:02X}  '
            f'fg:{self.cur_fg}({VERA_NAMES[self.cur_fg]})  '
            f'bg:{self.cur_bg}({VERA_NAMES[self.cur_bg]})  '
            f'inv:{self.cur_inv.get()}  '
            f'{"*modified*" if self.dirty else ""}')

    # ── canvas interaction ────────────────────────────────────────────────────

    def _cell_from_pos(self, x, y):
        """Convert widget mouse coords → (row, col), accounting for scroll."""
        cx = int(self.tk_canvas.canvasx(x))
        cy = int(self.tk_canvas.canvasy(y))
        col = cx // self.stride_w
        row = cy // self.stride_h
        # Reject clicks that land on the border gap (not on the glyph area)
        cell_x = cx % self.stride_w
        cell_y = cy % self.stride_h
        glyph_pw = CELL_W      * self.scale
        glyph_ph = self.cell_h * self.scale
        if cell_x >= glyph_pw or cell_y >= glyph_ph:
            return None
        if 0 <= col < COLS and 0 <= row < ROWS:
            return row, col
        return None

    def _on_canvas_press(self, e):
        rc = self._cell_from_pos(e.x, e.y)
        if rc:
            self._save_undo()
            self._dragging = True
            self._paint(*rc)

    def _on_canvas_release(self, e):
        self._dragging = False

    def _on_canvas_drag(self, e):
        if not self._dragging:
            return
        rc = self._cell_from_pos(e.x, e.y)
        if rc:
            self._paint(*rc)

    def _on_canvas_pick(self, e):
        rc = self._cell_from_pos(e.x, e.y)
        if rc:
            cell = self.grid[rc[0]][rc[1]]
            self.cur_char = cell.char
            self.cur_fg   = cell.fg
            self.cur_bg   = cell.bg
            self.cur_inv.set(cell.inv)
            self._update_palette_sel('fg')
            self._update_palette_sel('bg')
            self._update_picker_sel()
            self._update_preview()
            self._rebuild_picker()

    def _paint(self, row, col):
        cell = self.grid[row][col]
        new_char = self.cur_char
        new_fg   = self.cur_fg
        new_bg   = self.cur_bg
        new_inv  = self.cur_inv.get()
        if (cell.char == new_char and cell.fg == new_fg
                and cell.bg == new_bg and cell.inv == new_inv):
            return
        cell.char = new_char
        cell.fg   = new_fg
        cell.bg   = new_bg
        cell.inv  = new_inv
        self._dirty_cells.add((row, col))
        self._redraw_dirty()
        self.dirty = True
        self._update_status()

    def _on_picker_click(self, e):
        # Account for scroll offset; use picker stride (2× + 1px sep)
        cy  = int(self.tk_picker.canvasy(e.y))
        col = e.x // self.picker_stride_x
        row = cy  // self.picker_stride_y
        ci  = row * PICKER_COLS + col
        if 0 <= ci < 256:
            self.cur_char = ci
            self._update_picker_sel()
            self._update_preview()
            self._update_status()

    def _set_color(self, attr, idx):
        if attr == 'fg':
            self.cur_fg = idx
        else:
            self.cur_bg = idx
        self._update_palette_sel(attr)
        self._update_preview()
        self._rebuild_picker()
        self._update_status()

    # ── canvas keyboard cursor ────────────────────────────────────────────────

    def _move_cursor(self, dr, dc):
        old_r, old_c = self.cursor_row, self.cursor_col
        self.cursor_row = max(0, min(ROWS - 1, self.cursor_row + dr))
        self.cursor_col = max(0, min(COLS - 1, self.cursor_col + dc))
        if (self.cursor_row, self.cursor_col) != (old_r, old_c):
            self._dirty_cells.add((old_r, old_c))
            self._dirty_cells.add((self.cursor_row, self.cursor_col))
            self._redraw_dirty()
            self._scroll_to_cursor()
        self._update_status()

    def _scroll_to_cursor(self):
        """Scroll the canvas so the cursor cell is visible."""
        cx = self.cursor_col * self.stride_w
        cy = self.cursor_row * self.stride_h
        vw = self.tk_canvas.winfo_width()
        vh = self.tk_canvas.winfo_height()
        x0 = int(self.tk_canvas.canvasx(0))
        y0 = int(self.tk_canvas.canvasy(0))
        if cx < x0:
            self.tk_canvas.xview_moveto(cx / self.canvas_w)
        elif cx + self.stride_w > x0 + vw:
            self.tk_canvas.xview_moveto(
                max(0, cx + self.stride_w - vw) / self.canvas_w)
        if cy < y0:
            self.tk_canvas.yview_moveto(cy / self.canvas_h)
        elif cy + self.stride_h > y0 + vh:
            self.tk_canvas.yview_moveto(
                max(0, cy + self.stride_h - vh) / self.canvas_h)

    def _paint_at_cursor(self):
        """Paint the cell at the keyboard cursor position (spacebar action)."""
        self._save_undo()
        self._paint(self.cursor_row, self.cursor_col)

    # ── undo ─────────────────────────────────────────────────────────────────

    def _save_undo(self):
        self._undo = [[c.copy() for c in row] for row in self.grid]

    def _cmd_undo(self):
        if self._undo is None:
            return
        self.grid = self._undo
        self._undo = None
        self._rebuild_canvas_full()
        self.dirty = True
        self._update_status()

    # ── commands ──────────────────────────────────────────────────────────────

    def _cmd_new(self):
        if self.dirty and not messagebox.askyesno('New', 'Discard changes?'):
            return
        self.grid = [[Cell() for _ in range(COLS)] for _ in range(ROWS)]
        self._undo = None
        self.project_file = None
        self.dirty = False
        self._rebuild_canvas_full()
        self.root.title('VeraX16 Logo Editor — 80×30')
        self._update_status()

    def _cmd_open(self):
        path = filedialog.askopenfilename(
            title='Open project',
            filetypes=[('Logo project', '*.logo.json *.json'), ('All', '*')])
        if path:
            self._load_project(path)

    def _cmd_save(self):
        if self.project_file:
            self._save_project(self.project_file)
        else:
            self._cmd_save_as()

    def _cmd_save_as(self):
        path = filedialog.asksaveasfilename(
            title='Save project',
            defaultextension='.logo.json',
            initialfile='logo.logo.json',
            filetypes=[('Logo project', '*.logo.json'), ('All', '*')])
        if path:
            self._save_project(path)

    def _cmd_export(self):
        default = (Path(self.project_file).stem + '.h'
                   if self.project_file else 'logo-ansi.h')
        path = filedialog.asksaveasfilename(
            title='Export C header',
            defaultextension='.h',
            initialfile=default,
            filetypes=[('C header', '*.h'), ('All', '*')])
        if path:
            self._export_c_header(path)
            self.status_var.set(f'Exported → {path}')

    def _cmd_fill(self):
        self._save_undo()
        for row in range(ROWS):
            for col in range(COLS):
                c = self.grid[row][col]
                c.char = self.cur_char
                c.fg   = self.cur_fg
                c.bg   = self.cur_bg
                c.inv  = self.cur_inv.get()
        self._rebuild_canvas_full()
        self.dirty = True
        self._update_status()

    def _cmd_clear(self):
        self._save_undo()
        self.grid = [[Cell() for _ in range(COLS)] for _ in range(ROWS)]
        self._rebuild_canvas_full()
        self.dirty = True
        self._update_status()

    # ── project file ──────────────────────────────────────────────────────────

    def _save_project(self, path):
        data = {
            'version': 1, 'cols': COLS, 'rows': ROWS,
            'grid': [[cell.to_dict() for cell in row] for row in self.grid],
        }
        Path(path).write_text(json.dumps(data, separators=(',', ':')))
        self.project_file = path
        self.dirty = False
        self.root.title(f'VeraX16 Logo Editor — {Path(path).name}')
        self.status_var.set(f'Saved → {path}')

    def _load_project(self, path):
        data = json.loads(Path(path).read_text())
        saved = data.get('grid', [])
        for r in range(ROWS):
            for c in range(COLS):
                if r < len(saved) and c < len(saved[r]):
                    self.grid[r][c] = Cell.from_dict(saved[r][c])
                else:
                    self.grid[r][c] = Cell()
        self.project_file = path
        self.dirty = False
        self._rebuild_canvas_full()
        self.root.title(f'VeraX16 Logo Editor — {Path(path).name}')
        self._update_status()

    # ── C header export ───────────────────────────────────────────────────────

    @staticmethod
    def _c_esc(ch):
        """Escape one character for a C string literal."""
        if ch == 0x1B:   return '\\x1B'
        if ch == 0x5C:   return '\\\\'
        if ch == 0x22:   return '\\"'
        if 0x20 <= ch < 0x7F:
            return chr(ch)
        return f'\\x{ch:02X}'

    def _build_cell_table(self):
        """Return (start_row, start_col, num_rows, num_cols, data) where
        data is a flat list of (vera_color, glyph) pairs covering ALL cells
        in the bounding box of user-touched cells, row by row, left to right.

        "User-touched" = any cell that differs from the pristine default
        (char=0x20, fg=1/white, bg=6/blue, inv=False).  Cells outside the
        bounding box are never emitted; cells INSIDE the box are always
        emitted even if they look like the default, so the user can set a
        custom background color anywhere inside the logo area.

        Format accepted by draw_logo():
            header: start_row, start_col, num_rows, num_cols  (4 bytes)
            body:   vera_color, glyph  per cell, row-major    (num_rows*num_cols*2 bytes)
        """
        DEF_FG = 1   # VERA white
        DEF_BG = 6   # VERA blue

        def is_touched(cell):
            ef = cell.bg if cell.inv else cell.fg
            eb = cell.fg if cell.inv else cell.bg
            return not (cell.char == 0x20 and ef == DEF_FG and eb == DEF_BG)

        # --- bounding box of touched cells ---
        touched = [(r, c)
                   for r in range(ROWS)
                   for c in range(COLS)
                   if is_touched(self.grid[r][c])]
        if not touched:
            return 0, 0, 0, 0, []

        min_row = min(r for r, _ in touched)
        max_row = max(r for r, _ in touched)
        min_col = min(c for _, c in touched)
        max_col = max(c for _, c in touched)
        num_rows = max_row - min_row + 1
        num_cols = max_col - min_col + 1

        # --- ALL cells inside the bounding box ---
        data = []
        for row in range(min_row, max_row + 1):
            for col in range(min_col, max_col + 1):
                cell = self.grid[row][col]
                ef = cell.bg if cell.inv else cell.fg   # effective fg
                eb = cell.fg if cell.inv else cell.bg   # effective bg (fills space)
                vera_color = (eb << 4) | ef
                data.append((vera_color, cell.char))

        return min_row, min_col, num_rows, num_cols, data

    def _build_ansi_string(self):
        """Return a list of C string-literal fragments.

        Only rows with at least one non-default cell are emitted.
        Position (ESC[r;cH) and color are emitted only when a non-empty run
        exists — this avoids spurious color-state changes with no visible chars.

        Default state = space (0x20), fg=white (ANSI 37), bg=blue (ANSI 44),
        matching vt_reset() + ESC[2J executed by the caller before draw_logo().

        ESC[2J is NOT included here.
        Glyph 0x1B is replaced by space (logo_emit treats 0x1B as ESC).
        Trailing spaces are stripped only when the background is the default
        blue (44); colored spaces are kept because they render background blocks.
        """
        DEF_FG = VERA_ANSI_FG[1]   # 37 = white
        DEF_BG = VERA_ANSI_BG[6]   # 44 = blue

        def cell_ansi(cell):
            ef = cell.bg if cell.inv else cell.fg
            eb = cell.fg if cell.inv else cell.bg
            return VERA_ANSI_FG[ef], VERA_ANSI_BG[eb]

        def is_default(cell):
            af, ab = cell_ansi(cell)
            return cell.char == 0x20 and af == DEF_FG and ab == DEF_BG

        parts = []
        prev_ansi_fg = None
        prev_ansi_bg = None

        for row in range(ROWS):
            if all(is_default(self.grid[row][c]) for c in range(COLS)):
                continue     # skip fully-default rows

            col = 0
            while col < COLS:
                # Skip default cells
                while col < COLS and is_default(self.grid[row][col]):
                    col += 1
                if col >= COLS:
                    break

                run_col  = col
                ansi_fg, ansi_bg = cell_ansi(self.grid[row][col])

                # Gather same-color run (including default-colored spaces within
                # it so the cursor advances correctly across the run).
                run = ''
                while col < COLS:
                    c = self.grid[row][col]
                    af, ab = cell_ansi(c)
                    if af != ansi_fg or ab != ansi_bg:
                        break
                    ch = 0x20 if c.char == 0x1B else c.char
                    run += self._c_esc(ch)
                    col += 1

                # Strip trailing spaces only for default-background runs
                # (non-default bg spaces are visible background-color blocks).
                if ansi_bg == DEF_BG:
                    run = run.rstrip(' ')

                if not run:
                    continue  # nothing to emit — skip position+color too

                # Position (emitted AFTER confirming the run is non-empty)
                parts.append(f'"\\x1B[{row + 1};{run_col + 1}H"')

                # Color
                if ansi_fg != prev_ansi_fg or ansi_bg != prev_ansi_bg:
                    parts.append(f'"\\x1B[{ansi_fg};{ansi_bg}m"')
                    prev_ansi_fg = ansi_fg
                    prev_ansi_bg = ansi_bg

                # Characters (split at 64 to keep C string literals short)
                for i in range(0, len(run), 64):
                    parts.append(f'"{run[i:i+64]}"')

        return parts

    def _export_c_header(self, path):
        """Export the logo as a VERA-native bounding-box table.

        Header: start_row, start_col, num_rows, num_cols  (4 bytes)
        Body:   vera_color, glyph  for every cell in the box, row-major.

        ALL cells inside the bounding box of touched cells are exported —
        including those that look like the default — so the user can choose
        any fg/bg for the logo area background.  Only cells completely
        outside the bounding box (never touched) are omitted.

        draw_logo() positions the cursor once per row (VCTL + ROWCRS_OS/
        COLCRS_OS) then emits color+glyph pairs left-to-right; CURSOR_X
        auto-advances after each putchar().  No VT100 parsing.

        In runcpm.c:  draw_logo(vctl);
        """
        guard = Path(path).name.upper().replace('.', '_').replace('-', '_')
        start_row, start_col, num_rows, num_cols, data = self._build_cell_table()

        # Build data lines: 8 (color,glyph) pairs per source line
        data_lines = []
        for i in range(0, len(data), 8):
            chunk = data[i:i+8]
            pairs = ','.join(f'0x{color:02X},0x{glyph:02X}' for color, glyph in chunk)
            data_lines.append(f'    {pairs},')

        lines = []
        lines.append(f'/* {Path(path).name} — generated by vera_logo_editor.py */')
        lines.append(f'/* VeraX16 80\xd7{ROWS} logo — VERA-native bounding-box table.    */')
        lines.append(f'#ifndef {guard}')
        lines.append(f'#define {guard}')
        lines.append('')
        lines.append(f'#define LOGO_COLS      {COLS}')
        lines.append(f'#define LOGO_ROWS      {ROWS}')
        lines.append(f'#define LOGO_START_ROW {start_row}')
        lines.append(f'#define LOGO_START_COL {start_col}')
        lines.append(f'#define LOGO_NUM_ROWS  {num_rows}')
        lines.append(f'#define LOGO_NUM_COLS  {num_cols}')
        lines.append('')
        lines.append('/*')
        lines.append(' * logo_data[]: header (4B) + body (num_rows*num_cols*2B), row-major.')
        lines.append(' *   [0] start_row  [1] start_col  [2] num_rows  [3] num_cols')
        lines.append(' *   [4..] vera_color, glyph  per cell  (vera_color=(bg<<4)|fg)')
        lines.append(' *')
        lines.append(' * ALL cells inside the bounding box are included — touching or not —')
        lines.append(' * so custom background colors inside the logo area are preserved.')
        lines.append(' *')
        lines.append(' * In runcpm.c:  draw_logo(vctl);')
        lines.append(' */')
        lines.append('static const unsigned char logo_data[] = {')
        lines.append(f'    /* header */ 0x{start_row:02X},0x{start_col:02X},'
                     f'0x{num_rows:02X},0x{num_cols:02X},')
        lines.extend(data_lines)
        lines.append('};')
        lines.append('')
        lines.append('/*')
        lines.append(' * draw_logo() — render logo directly via VERA VCTL block.')
        lines.append(' * Positions cursor once per row; CURSOR_X auto-advances each putchar.')
        lines.append(' * ROWCRS_OS/COLCRS_OS kept in sync to prevent VBI cursor_tick override.')
        lines.append(' * v = vctl pointer (pass NULL to skip VERA writes, e.g. no driver).')
        lines.append(' */')
        lines.append('static void draw_logo(volatile unsigned char *v)')
        lines.append('{')
        lines.append('    const unsigned char *p = logo_data;')
        lines.append('    unsigned char sr = *p++; /* start_row */')
        lines.append('    unsigned char sc = *p++; /* start_col */')
        lines.append('    unsigned char nr = *p++; /* num_rows  */')
        lines.append('    unsigned char nc = *p++; /* num_cols  */')
        lines.append('    unsigned char r, c;')
        lines.append('    for (r = sr; r < (unsigned char)(sr + nr); ++r) {')
        lines.append('        if (v) {')
        lines.append('            v[9] = r;   /* VCTL_CURSOR_Y */')
        lines.append('            v[8] = sc;  /* VCTL_CURSOR_X */')
        lines.append('            *((volatile unsigned char*)0x0054) = r;  /* ROWCRS_OS */')
        lines.append('            *((volatile unsigned char*)0x0055) = sc; /* COLCRS_OS */')
        lines.append('        }')
        lines.append('        for (c = sc; c < (unsigned char)(sc + nc); ++c) {')
        lines.append('            if (v) v[7] = *p++; /* VCTL_PARAM1: vera_color */')
        lines.append('            else   p++;')
        lines.append('            putchar(*p++);      /* glyph — CURSOR_X auto-advances */')
        lines.append('        }')
        lines.append('    }')
        lines.append('}')
        lines.append('')
        lines.append(f'#endif /* {guard} */')

        Path(path).write_text('\n'.join(lines) + '\n', encoding='utf-8')


# ─── entry point ─────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description='VeraX16 80×30 logo/ASCII-art editor')
    ap.add_argument('font',    help='Binary font file (1024/2048/4096 B)')
    ap.add_argument('project', nargs='?', help='Project file to open (.logo.json)')
    args = ap.parse_args()

    root = tk.Tk()
    root.resizable(False, False)
    app = LogoEditor(root, args.font, args.project)
    root.protocol('WM_DELETE_WINDOW', root.quit)
    root.mainloop()


if __name__ == '__main__':
    main()
