import tkinter as tk
from tkinter import filedialog, colorchooser, ttk
from PIL import Image, ImageTk, ImageDraw
import struct
import os

class VeraPalette:
    def color_to_vera(self, r, g, b):
        r4 = r >> 4
        g4 = g >> 4
        b4 = b >> 4
        b0 = (g4 << 4) | b4
        b1 = r4
        return struct.pack('<BB', b0, b1)

class VeraExporter:
    @staticmethod
    def export_sprite(sprite_id, frames_data, filename):
        with open(filename, 'wb') as f:
            f.write(b'SPR\x01')
            f.write(struct.pack('<HH', sprite_id, len(frames_data)))
            for frame in frames_data:
                f.write(frame)

    @staticmethod
    def export_tile(tile_id, tile_data, filename):
        with open(filename, 'wb') as f:
            f.write(b'TLE\x01')
            f.write(struct.pack('<H', tile_id))
            f.write(tile_data)

class VeraEditor:
    def __init__(self, root):
        self.root = root
        self.root.title("VERA Sprite/Tile Editor")
        self.root.geometry("1024x600")
        self.root.minsize(800, 600)
        
        self.config_file = "last_path.txt"
        self.image = None
        self.tk_image = None
        self.transparent_color = (0, 0, 0)
        self.picking_transparency = False
        self.is_dialog_open = False
        self.selection_type = tk.StringVar(value="Sprite")
        self.selection_start = None
        self.current_rect = None
        self.cursor_coords = [0, 0, 16, 16] 
        self.selections = [] 
        self.zoom_level = 1.0

        self.setup_menu()
        self.create_widgets()
        
        self.root.bind("<space>", self.confirm_selection)
        self.root.bind("<Left>", lambda e: self.move_selection(-1, 0))
        self.root.bind("<Right>", lambda e: self.move_selection(1, 0))
        self.root.bind("<Up>", lambda e: self.move_selection(0, -1))
        self.root.bind("<Down>", lambda e: self.move_selection(0, 1))
        
        self.load_last_image()

    def setup_menu(self):
        menubar = tk.Menu(self.root)
        filemenu = tk.Menu(menubar, tearoff=0)
        filemenu.add_command(label="Open", command=self.load_image)
        filemenu.add_command(label="Exit", command=self.root.quit)
        menubar.add_cascade(label="File", menu=filemenu)
        self.root.config(menu=menubar)

    def create_widgets(self):
        # Layout: Sidebar a sinistra, Canvas a destra
        self.paned = tk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        self.paned.pack(fill=tk.BOTH, expand=True)

        # 1. Sidebar (Strumenti + Lista Selezioni)
        self.sidebar_container = tk.Frame(self.paned, width=200, bg='lightgray')
        self.paned.add(self.sidebar_container, minsize=200)

        # Sidebar: Area strumenti
        self.tools_frame = tk.Frame(self.sidebar_container, bg='lightgray')
        self.tools_frame.pack(fill=tk.X, padx=5, pady=5)
        
        tk.Label(self.tools_frame, text="Transparency", bg='lightgray').pack(pady=(10,0))
        tk.Button(self.tools_frame, text="Eyedropper", command=self.activate_pipette).pack(fill=tk.X)
        self.hex_entry = tk.Entry(self.tools_frame, width=8)
        self.hex_entry.insert(0, "#000000")
        self.hex_entry.pack()
        tk.Button(self.tools_frame, text="Set Hex", command=self.set_hex_transparency).pack(fill=tk.X)
        
        self.color_sample = tk.Frame(self.tools_frame, width=40, height=20, bg='#000000', bd=1, relief="solid")
        self.color_sample.pack(pady=5)
        self.lbl_rgb = tk.Label(self.tools_frame, text="RGB:(0,0,0)\nHEX:#000000", bg='lightgray')
        self.lbl_rgb.pack()
        
        tk.Button(self.tools_frame, text="Convert Palette", command=self.convert_palette).pack(fill=tk.X, pady=5)
        
        tk.Label(self.tools_frame, text="Mode:", bg='lightgray').pack(pady=(10,0))
        self.rb_sprite = tk.Radiobutton(self.tools_frame, text="Sprite", variable=self.selection_type, value="Sprite", bg='lightgray', command=self.update_size_options)
        self.rb_sprite.pack()
        self.rb_tile = tk.Radiobutton(self.tools_frame, text="Tile", variable=self.selection_type, value="Tile", bg='lightgray', command=self.update_size_options)
        self.rb_tile.pack()

        tk.Label(self.tools_frame, text="Size:", bg='lightgray').pack(pady=(10,0))
        self.size_cb = ttk.Combobox(self.tools_frame, state="readonly", width=10)
        self.size_cb.pack()
        self.update_size_options()

        # Sidebar: Area Selezioni (Sotto strumenti)
        tk.Label(self.sidebar_container, text="Selections:", bg='lightgray').pack(pady=(10,0))
        self.sel_listbox = tk.Listbox(self.sidebar_container, height=10)
        self.sel_listbox.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # 2. Canvas
        self.canvas = tk.Canvas(self.paned, bg='black')
        self.paned.add(self.canvas, minsize=400)
        
        color = "green" if self.selection_type.get() == "Sprite" else "red"
        self.current_rect = self.canvas.create_rectangle(0, 0, 16, 16, outline=color, tags="cursor_rect")
        
        self.canvas.bind("<Motion>", self.on_mouse_move)
        self.canvas.bind("<Button-1>", self.on_click_confirm)
        self.canvas.bind("<MouseWheel>", self.on_zoom)
        self.canvas.bind("<Button-4>", self.on_zoom)
        self.canvas.bind("<Button-5>", self.on_zoom)

    def update_sel_list(self):
        self.sel_listbox.delete(0, tk.END)
        for i, sel in enumerate(self.selections):
            info = f"{i}: ID={sel['id']} F={sel['frame']} ({sel['type']})"
            self.sel_listbox.insert(tk.END, info)

    def update_size_options(self):
        if self.selection_type.get() == "Sprite":
            self.size_cb['values'] = ["8x8", "16x16", "32x32", "64x64"]
        else:
            self.size_cb['values'] = ["8x8", "16x16"]
        self.size_cb.current(0)
        self.update_cursor_size()

    def update_cursor_size(self):
        if self.current_rect:
            w, h = map(int, self.size_cb.get().split('x'))
            w *= self.zoom_level
            h *= self.zoom_level
            self.cursor_coords[2] = self.cursor_coords[0] + w
            self.cursor_coords[3] = self.cursor_coords[1] + h
            self.canvas.coords(self.current_rect, *self.cursor_coords)
            color = "green" if self.selection_type.get() == "Sprite" else "red"
            self.canvas.itemconfig(self.current_rect, outline=color)

    def on_mouse_move(self, event):
        if not self.picking_transparency and not self.is_dialog_open and self.image:
            w, h = map(int, self.size_cb.get().split('x'))
            w *= self.zoom_level
            h *= self.zoom_level
            self.cursor_coords = [event.x, event.y, event.x + w, event.y + h]
            self.canvas.coords(self.current_rect, *self.cursor_coords)
            self.canvas.tag_raise("cursor_rect")
    
    def move_selection(self, dx, dy):
        if not self.is_dialog_open:
            self.canvas.move(self.current_rect, dx * 5, dy * 5)
            self.cursor_coords = self.canvas.coords(self.current_rect)
    
    def confirm_selection(self, event):
        if self.current_rect and not self.is_dialog_open:
            coords = self.canvas.coords(self.current_rect)
            self.show_metadata_dialog(coords)

    def show_metadata_dialog(self, coords):
        self.is_dialog_open = True
        top = tk.Toplevel(self.root)
        top.title("Assign Metadata")
        
        existing_ids = list(set([s['id'] for s in self.selections if s['id']]))
        tk.Label(top, text=f"Existing IDs: {', '.join(existing_ids) if existing_ids else 'None'}", fg="blue").pack()
        
        tk.Label(top, text="ID (Inserisci esistente per concatenare):").pack()
        id_entry = tk.Entry(top)
        id_entry.pack()
        
        tk.Label(top, text="Frame Index:").pack()
        frame_entry = tk.Entry(top)
        frame_entry.pack()
            
        def save():
            obj_id = id_entry.get()
            frame_idx = frame_entry.get()
            
            orig_coords = [c / self.zoom_level for c in coords]
            
            w, h = int(coords[2]-coords[0]), int(coords[3]-coords[1])
            color = (0, 255, 0) if self.selection_type.get() == "Sprite" else (255, 0, 0)
            overlay = Image.new('RGBA', (w, h), color + (128,))
            tk_overlay = ImageTk.PhotoImage(overlay)
            
            self.canvas.create_image(coords[0], coords[1], image=tk_overlay, anchor=tk.NW, tags="selection")
            
            self.selections.append({
                'coords': orig_coords, 
                'type': self.selection_type.get(),
                'id': obj_id,
                'frame': frame_idx,
                'tk_obj': tk_overlay
            })
            self.update_sel_list()
            self.is_dialog_open = False
            top.destroy()
        
        top.protocol("WM_DELETE_WINDOW", lambda: [setattr(self, 'is_dialog_open', False), top.destroy()])
        tk.Button(top, text="Confirm", command=save).pack()

    def redraw_selections(self):
        self.canvas.delete("selection")
        for sel in self.selections:
            orig = sel['coords']
            scaled = [c * self.zoom_level for c in orig]
            w, h = int(scaled[2]-scaled[0]), int(scaled[3]-scaled[1])
            color = (0, 255, 0) if sel['type'] == "Sprite" else (255, 0, 0)
            overlay = Image.new('RGBA', (w, h), color + (128,))
            tk_overlay = ImageTk.PhotoImage(overlay)
            self.canvas.create_image(scaled[0], scaled[1], image=tk_overlay, anchor=tk.NW, tags="selection")
            sel['tk_obj'] = tk_overlay

    def on_zoom(self, event):
        if event.delta:
            scale = 1.1 if event.delta > 0 else 0.9
        else:
            scale = 1.1 if event.num == 4 else 0.9
        self.zoom_level *= scale
        
        new_w = max(1, int(self.image.width * self.zoom_level))
        new_h = max(1, int(self.image.height * self.zoom_level))
        resized_img = self.image.resize((new_w, new_h), Image.NEAREST)
        self.tk_image = ImageTk.PhotoImage(resized_img)
        
        self.canvas.delete("all")
        self.canvas.create_image(0, 0, image=self.tk_image, anchor=tk.NW, tags="image")
        self.canvas.tag_lower("image")
        
        color = "green" if self.selection_type.get() == "Sprite" else "red"
        self.current_rect = self.canvas.create_rectangle(*self.cursor_coords, outline=color, tags="cursor_rect")
        
        self.redraw_selections()

    def set_hex_transparency(self):
        hex_val = self.hex_entry.get()
        if hex_val.startswith('#') and len(hex_val) == 7:
            r = int(hex_val[1:3], 16)
            g = int(hex_val[3:5], 16)
            b = int(hex_val[5:7], 16)
            self.transparent_color = (r, g, b)
            self.update_color_indicator()

    def activate_pipette(self):
        self.picking_transparency = True
        self.canvas.config(cursor="cross")

    def update_color_indicator(self):
        r, g, b = self.transparent_color
        hex_color = f'#{r:02x}{g:02x}{b:02x}'
        self.color_sample.config(bg=hex_color)
        self.lbl_rgb.config(text=f"RGB:({r},{g},{b})\nHEX:{hex_color}")
            
    def display_image(self):
        new_w = int(self.image.width * self.zoom_level)
        new_h = int(self.image.height * self.zoom_level)
        resized_img = self.image.resize((new_w, new_h), Image.NEAREST)
        self.tk_image = ImageTk.PhotoImage(resized_img)
        self.canvas.delete("all")
        self.canvas.create_image(0, 0, image=self.tk_image, anchor=tk.NW, tags="image")
        self.canvas.tag_lower("image")
        self.redraw_selections()

    def on_click_confirm(self, event):
        if self.picking_transparency and self.image:
            x = int(event.x / self.zoom_level)
            y = int(event.y / self.zoom_level)
            if 0 <= x < self.image.width and 0 <= y < self.image.height:
                self.transparent_color = self.image.getpixel((x, y))
                self.update_color_indicator()
                self.picking_transparency = False
                self.canvas.config(cursor="arrow")
        elif not self.picking_transparency and self.image:
            self.confirm_selection(None)
            
    def load_last_image(self):
        if os.path.exists(self.config_file):
            with open(self.config_file, 'r') as f:
                path = f.read().strip()
                if os.path.exists(path):
                    self.open_image(path)

    def save_last_path(self, path):
        with open(self.config_file, 'w') as f:
            f.write(path)

    def load_image(self):
        path = filedialog.askopenfilename()
        if path:
            self.open_image(path)
            self.save_last_path(path)

    def open_image(self, path):
        self.image = Image.open(path).convert('RGB')
        # Calcola zoom basandosi sulle dimensioni attuali del canvas
        self.root.update() 
        canvas_w = self.canvas.winfo_width()
        canvas_h = self.canvas.winfo_height()
        self.zoom_level = min(canvas_w / self.image.width, canvas_h / self.image.height)
        self.display_image()

    def convert_palette(self):
        if not self.image: return
        data = self.image.getdata()
        new_data = []
        for r, g, b in data:
            if (r,g,b) == self.transparent_color:
                new_data.append((0, 0, 0))
            else:
                new_data.append(((r >> 4) << 4, (g >> 4) << 4, (b >> 4) << 4))
        self.image.putdata(new_data)
        self.display_image()

if __name__ == "__main__":
    root = tk.Tk()
    app = VeraEditor(root)
    root.mainloop()
