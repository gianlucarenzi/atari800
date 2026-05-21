#!/usr/bin/env python3
"""
vera_font_editor.py — bitmap editor for VERA 1bpp binary fonts (8×8 or 8×16).

Usage:
    python3 vera_font_editor.py font8x8.bin
    python3 vera_font_editor.py font8x16.bin
    python3 vera_font_editor.py              (opens file dialog)

Controls:
    Left-click / drag      set pixel ON
    Right-click / drag     set pixel OFF
    Arrow keys             navigate between characters
    Ctrl+C / Ctrl+V        copy / paste glyph
    Ctrl+S                 save
    Ctrl+Z                 undo (single level per glyph)
"""

import sys
import tkinter as tk
from tkinter import filedialog, messagebox
from pathlib import Path

# ─── layout constants ────────────────────────────────────────────────────────

FONT_W      = 8        # tile width in pixels (always 8)
GRID_COLS   = 16       # characters per row in the overview
GRID_ROWS   = 8        # rows of characters  (16×8 = 128)
OV_SCALE    = 4        # overview: pixels per font-pixel
ED_SCALE    = 36       # editor:   pixels per font-pixel

# ─── colours ─────────────────────────────────────────────────────────────────

C_ON      = "#FFFFFF"
C_OFF     = "#111111"
C_GRID    = "#444444"
C_SEL     = "#FF8800"
C_BG      = "#1C1C2E"
C_PANEL   = "#14141E"
C_BTN     = "#2A2A4A"
C_BTN_FG  = "#DDDDDD"

# ─── helpers ─────────────────────────────────────────────────────────────────

def _detect_height(size):
    if size == 1024:
        return 8
    if size == 2048:
        return 16
    raise ValueError(f"Unexpected size {size} B — expected 1024 (8×8) or 2048 (8×16)")

def _bit(byte, col):
    return (byte >> (7 - col)) & 1

def _set_bit(byte, col, v):
    mask = 1 << (7 - col)
    return (byte | mask) if v else (byte & ~mask)

# ─── main application ─────────────────────────────────────────────────────────

class FontEditor:

    def __init__(self, root, filename=None):
        self.root     = root
        self.filename = None
        self.fh       = 8            # font height (8 or 16)
        self.glyphs   = []           # list of 128 bytearrays
        self.sel      = 0            # selected char index
        self.clip     = None         # clipboard (bytearray)
        self.undo_buf = {}           # {idx: bytearray} — one undo per glyph
        self.dirty    = False
        self._draw_mode = 1          # set or clear during drag

        self._build_ui()
        root.title("VERA Font Editor")
        root.configure(bg=C_BG)
        root.resizable(False, False)

        if filename:
            self._load(filename)
        else:
            self._new(8)

    # ── UI construction ───────────────────────────────────────────────────────

    def _build_ui(self):
        r = self.root

        # ── menu ──────────────────────────────────────────────────────────────
        mb = tk.Menu(r)
        fm = tk.Menu(mb, tearoff=0)
        fm.add_command(label="Open…",    command=self.do_open,    accelerator="Ctrl+O")
        fm.add_command(label="Save",     command=self.do_save,    accelerator="Ctrl+S")
        fm.add_command(label="Save As…", command=self.do_save_as)
        fm.add_separator()
        fm.add_command(label="Quit",     command=self.do_quit,    accelerator="Ctrl+Q")
        mb.add_cascade(label="File", menu=fm)
        r.config(menu=mb)

        r.bind("<Control-o>", lambda _: self.do_open())
        r.bind("<Control-s>", lambda _: self.do_save())
        r.bind("<Control-q>", lambda _: self.do_quit())
        r.bind("<Control-c>", lambda _: self.do_copy())
        r.bind("<Control-v>", lambda _: self.do_paste())
        r.bind("<Control-z>", lambda _: self.do_undo())
        r.bind("<Left>",  lambda _: self._nav(-1,  0))
        r.bind("<Right>", lambda _: self._nav(+1,  0))
        r.bind("<Up>",    lambda _: self._nav( 0, -1))
        r.bind("<Down>",  lambda _: self._nav( 0, +1))

        # ── outer frame ───────────────────────────────────────────────────────
        outer = tk.Frame(r, bg=C_BG)
        outer.pack(padx=8, pady=8)

        # ── left: overview ────────────────────────────────────────────────────
        lf = tk.Frame(outer, bg=C_BG)
        lf.grid(row=0, column=0, rowspan=2, padx=(0, 12), sticky="n")

        tk.Label(lf, text="All characters  (click to select)",
                 bg=C_BG, fg="#888888", font=("monospace", 9)).pack(anchor="w", pady=(0,3))

        self.ov_canvas = tk.Canvas(lf, bg=C_OFF, cursor="hand2",
                                   highlightthickness=2,
                                   highlightbackground="#444466")
        self.ov_canvas.pack()
        self.ov_canvas.bind("<Button-1>", self._ov_click)

        # PhotoImage for the overview (pixel-exact, fast updates)
        self.ov_img = None   # created after font is loaded

        # ── right: info bar ───────────────────────────────────────────────────
        rf = tk.Frame(outer, bg=C_BG)
        rf.grid(row=0, column=1, sticky="nw")

        self.info_var = tk.StringVar()
        tk.Label(rf, textvariable=self.info_var, bg=C_BG, fg="#CCCCCC",
                 font=("monospace", 11), anchor="w").pack(anchor="w", pady=(0, 6))

        # ── right: editor canvas ──────────────────────────────────────────────
        self.ed_canvas = tk.Canvas(rf, bg=C_OFF, cursor="pencil",
                                   highlightthickness=2,
                                   highlightbackground="#444466")
        self.ed_canvas.pack()
        self.ed_canvas.bind("<Button-1>",  self._ed_press)
        self.ed_canvas.bind("<B1-Motion>", self._ed_drag)
        self.ed_canvas.bind("<Button-3>",  self._ed_press_r)
        self.ed_canvas.bind("<B3-Motion>", self._ed_drag_r)

        # ── toolbar ───────────────────────────────────────────────────────────
        tf = tk.Frame(outer, bg=C_PANEL)
        tf.grid(row=1, column=1, sticky="ew", pady=(8, 0))

        def btn(parent, text, cmd, **kw):
            b = tk.Button(parent, text=text, command=cmd,
                          bg=C_BTN, fg=C_BTN_FG, relief="flat",
                          activebackground="#4A4A8A", activeforeground="white",
                          font=("monospace", 9), padx=5, pady=3, **kw)
            b.pack(side=tk.LEFT, padx=2, pady=4)
            return b

        row1 = tk.Frame(tf, bg=C_PANEL); row1.pack(fill=tk.X, padx=4)
        row2 = tk.Frame(tf, bg=C_PANEL); row2.pack(fill=tk.X, padx=4)

        btn(row1, "Copy",     self.do_copy)
        btn(row1, "Paste",    self.do_paste)
        btn(row1, "Undo",     self.do_undo)
        btn(row1, "Clear",    self.do_clear)
        btn(row1, "Invert",   self.do_invert)
        btn(row1, "Flip H",   self.do_flip_h)
        btn(row1, "Flip V",   self.do_flip_v)
        btn(row2, "Shift ←",  lambda: self.do_shift(-1,  0))
        btn(row2, "Shift →",  lambda: self.do_shift(+1,  0))
        btn(row2, "Shift ↑",  lambda: self.do_shift( 0, -1))
        btn(row2, "Shift ↓",  lambda: self.do_shift( 0, +1))
        btn(row2, "Roll ←",   lambda: self.do_roll(-1,  0))
        btn(row2, "Roll →",   lambda: self.do_roll(+1,  0))
        btn(row2, "Roll ↑",   lambda: self.do_roll( 0, -1))
        btn(row2, "Roll ↓",   lambda: self.do_roll( 0, +1))

    # ── font I/O ──────────────────────────────────────────────────────────────

    def _new(self, height):
        self.fh     = height
        self.glyphs = [bytearray(height) for _ in range(128)]
        self.sel    = 0
        self.dirty  = False
        self._resize()
        self._full_refresh()

    def _load(self, path):
        data = Path(path).read_bytes()
        h = _detect_height(len(data))
        self.filename = path
        self.fh       = h
        self.glyphs   = [bytearray(data[i*h:(i+1)*h]) for i in range(128)]
        self.sel      = 0
        self.dirty    = False
        self.undo_buf.clear()
        self.root.title(f"VERA Font Editor — {Path(path).name}")
        self._resize()
        self._full_refresh()

    def _save(self, path):
        data = bytearray(b for g in self.glyphs for b in g)
        Path(path).write_bytes(data)
        self.filename = path
        self.dirty    = False
        self.root.title(f"VERA Font Editor — {Path(path).name}")

    # ── canvas sizing ─────────────────────────────────────────────────────────

    def _resize(self):
        h = self.fh
        ow = GRID_COLS * FONT_W * OV_SCALE
        oh = GRID_ROWS * h      * OV_SCALE
        self.ov_canvas.config(width=ow, height=oh)
        self.ov_img = tk.PhotoImage(width=ow, height=oh)
        self.ov_canvas.delete("all")
        self.ov_canvas.create_image(0, 0, anchor="nw", image=self.ov_img)

        ew = FONT_W * ED_SCALE
        eh = h      * ED_SCALE
        self.ed_canvas.config(width=ew, height=eh)

    # ── rendering ─────────────────────────────────────────────────────────────

    def _full_refresh(self):
        self._paint_all_overview()
        self._draw_editor()
        self._draw_selection_box()
        self._update_info()

    def _paint_glyph_overview(self, idx):
        """Paint one glyph into the overview PhotoImage."""
        img   = self.ov_img
        h     = self.fh
        s     = OV_SCALE
        cx    = idx % GRID_COLS
        cy    = idx // GRID_COLS
        x0    = cx * FONT_W * s
        y0    = cy * h      * s
        glyph = self.glyphs[idx]
        for row in range(h):
            byte = glyph[row]
            for col in range(FONT_W):
                color = C_ON if _bit(byte, col) else C_OFF
                # put a s×s block
                x1, y1 = x0 + col*s, y0 + row*s
                img.put(color, to=(x1, y1, x1+s, y1+s))

    def _paint_all_overview(self):
        for i in range(128):
            self._paint_glyph_overview(i)

    def _draw_selection_box(self):
        c  = self.ov_canvas
        s  = OV_SCALE
        h  = self.fh
        cx = self.sel % GRID_COLS
        cy = self.sel // GRID_COLS
        x0 = cx * FONT_W * s
        y0 = cy * h      * s
        c.delete("sel_box")
        c.create_rectangle(x0, y0, x0 + FONT_W*s, y0 + h*s,
                            outline=C_SEL, width=2, tags="sel_box")

    def _draw_editor(self):
        c  = self.ed_canvas
        c.delete("all")
        s  = ED_SCALE
        h  = self.fh
        g  = self.glyphs[self.sel]
        for row in range(h):
            byte = g[row]
            for col in range(FONT_W):
                color = C_ON if _bit(byte, col) else C_OFF
                c.create_rectangle(col*s, row*s, col*s+s, row*s+s,
                                    fill=color, outline=C_GRID, width=1)

    def _update_info(self):
        idx = self.sel
        ch  = chr(idx) if 32 <= idx < 127 else "·"
        dirty = "*" if self.dirty else " "
        self.info_var.set(
            f"{dirty} Char #{idx:3d}  ${idx:02X}  '{ch}'   "
            f"font {FONT_W}×{self.fh}   "
            f"(← → ↑ ↓ to navigate)"
        )

    # ── overview interaction ──────────────────────────────────────────────────

    def _ov_click(self, ev):
        s   = OV_SCALE
        h   = self.fh
        col = ev.x // (FONT_W * s)
        row = ev.y // (h      * s)
        if 0 <= col < GRID_COLS and 0 <= row < GRID_ROWS:
            self._select(row * GRID_COLS + col)

    def _select(self, idx):
        self.sel = idx % 128
        self._draw_selection_box()
        self._draw_editor()
        self._update_info()

    def _nav(self, dx, dy):
        self._select(self.sel + dx + dy * GRID_COLS)

    # ── editor interaction ────────────────────────────────────────────────────

    def _canvas_to_pixel(self, x, y):
        col = x // ED_SCALE
        row = y // ED_SCALE
        if 0 <= col < FONT_W and 0 <= row < self.fh:
            return col, row
        return None, None

    def _toggle_pixel(self, x, y, value):
        col, row = self._canvas_to_pixel(x, y)
        if col is None:
            return
        self._push_undo()
        g = self.glyphs[self.sel]
        g[row] = _set_bit(g[row], col, value)
        self.dirty = True
        self._draw_editor()
        self._paint_glyph_overview(self.sel)
        self._draw_selection_box()
        self._update_info()

    def _ed_press(self, ev):
        col, row = self._canvas_to_pixel(ev.x, ev.y)
        if col is None:
            return
        # toggle: if pixel is on, drag will clear; if off, drag will set
        self._draw_mode = 0 if _bit(self.glyphs[self.sel][row], col) else 1
        self._toggle_pixel(ev.x, ev.y, self._draw_mode)

    def _ed_drag(self, ev):
        self._toggle_pixel(ev.x, ev.y, self._draw_mode)

    def _ed_press_r(self, ev):
        self._draw_mode = 0
        self._toggle_pixel(ev.x, ev.y, 0)

    def _ed_drag_r(self, ev):
        self._toggle_pixel(ev.x, ev.y, 0)

    # ── undo ─────────────────────────────────────────────────────────────────

    def _push_undo(self):
        # Save only once per glyph (first change before current edit session)
        if self.sel not in self.undo_buf:
            self.undo_buf[self.sel] = bytearray(self.glyphs[self.sel])

    def do_undo(self):
        if self.sel in self.undo_buf:
            self.glyphs[self.sel] = self.undo_buf.pop(self.sel)
            self.dirty = True
            self._draw_editor()
            self._paint_glyph_overview(self.sel)
            self._draw_selection_box()
            self._update_info()

    # ── edit commands ─────────────────────────────────────────────────────────

    def _commit(self):
        self.dirty = True
        self._draw_editor()
        self._paint_glyph_overview(self.sel)
        self._draw_selection_box()
        self._update_info()

    _CLIP_PREFIX = "VERA_GLYPH:"

    def do_copy(self):
        data = self.glyphs[self.sel].hex().upper()
        self.root.clipboard_clear()
        self.root.clipboard_append(self._CLIP_PREFIX + data)

    def do_paste(self):
        try:
            text = self.root.clipboard_get()
        except Exception:
            return
        if not text.startswith(self._CLIP_PREFIX):
            return
        try:
            raw = bytes.fromhex(text[len(self._CLIP_PREFIX):])
        except ValueError:
            return
        if len(raw) not in (8, 16):
            return
        self._push_undo()
        # If heights differ, center vertically (8→16 pad, 16→8 crop center)
        src_h, dst_h = len(raw), self.fh
        if src_h == dst_h:
            self.glyphs[self.sel] = bytearray(raw)
        elif src_h == 8 and dst_h == 16:
            pad = bytearray(4)
            self.glyphs[self.sel] = pad + bytearray(raw) + pad
        else:                          # src_h == 16, dst_h == 8: crop center
            self.glyphs[self.sel] = bytearray(raw[4:12])
        self._commit()

    def do_clear(self):
        self._push_undo()
        self.glyphs[self.sel] = bytearray(self.fh)
        self._commit()

    def do_invert(self):
        self._push_undo()
        g = self.glyphs[self.sel]
        for i in range(len(g)):
            g[i] ^= 0xFF
        self._commit()

    def do_flip_h(self):
        self._push_undo()
        g = self.glyphs[self.sel]
        for i in range(len(g)):
            b, r = g[i], 0
            for _ in range(8):
                r = (r << 1) | (b & 1)
                b >>= 1
            g[i] = r
        self._commit()

    def do_flip_v(self):
        self._push_undo()
        g = self.glyphs[self.sel]
        g[:] = g[::-1]
        self._commit()

    def do_shift(self, dx, dy):
        """Shift without wrap — pixels that fall off the edge are lost."""
        self._push_undo()
        g = self.glyphs[self.sel]
        h = self.fh
        if dy < 0:
            g[:] = g[-dy:] + bytearray(-dy)
        elif dy > 0:
            g[:] = bytearray(dy) + g[:-dy]
        if dx < 0:
            for i in range(h):
                g[i] = (g[i] << (-dx)) & 0xFF
        elif dx > 0:
            for i in range(h):
                g[i] = (g[i] >> dx) & 0xFF
        self._commit()

    def do_roll(self, dx, dy):
        """Roll with wrap — pixels that fall off reappear on the other side."""
        self._push_undo()
        g = self.glyphs[self.sel]
        h = self.fh
        if dy < 0:
            n = (-dy) % h
            g[:] = g[n:] + g[:n]
        elif dy > 0:
            n = dy % h
            g[:] = g[h-n:] + g[:h-n]
        if dx < 0:
            n = (-dx) % 8
            for i in range(h):
                g[i] = ((g[i] << n) | (g[i] >> (8-n))) & 0xFF
        elif dx > 0:
            n = dx % 8
            for i in range(h):
                g[i] = ((g[i] >> n) | (g[i] << (8-n))) & 0xFF
        self._commit()

    # ── file commands ─────────────────────────────────────────────────────────

    def _check_dirty(self):
        if not self.dirty:
            return True
        r = messagebox.askyesnocancel("Unsaved changes", "Save before continuing?")
        if r is None:
            return False
        if r:
            self.do_save()
        return True

    def do_open(self):
        if not self._check_dirty():
            return
        path = filedialog.askopenfilename(
            title="Open font binary",
            filetypes=[("Binary font", "*.bin"), ("All files", "*.*")]
        )
        if path:
            try:
                self._load(path)
            except Exception as e:
                messagebox.showerror("Error", str(e))

    def do_save(self):
        if self.filename:
            self._save(self.filename)
        else:
            self.do_save_as()

    def do_save_as(self):
        path = filedialog.asksaveasfilename(
            title="Save font binary",
            defaultextension=".bin",
            filetypes=[("Binary font", "*.bin"), ("All files", "*.*")]
        )
        if path:
            self._save(path)

    def do_quit(self):
        if self._check_dirty():
            self.root.destroy()


# ─── entry point ──────────────────────────────────────────────────────────────

def main():
    root = tk.Tk()
    fname = sys.argv[1] if len(sys.argv) > 1 else None
    FontEditor(root, fname)
    root.mainloop()

if __name__ == "__main__":
    main()
