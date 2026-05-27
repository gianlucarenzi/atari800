import tkinter as tk
from tkinter import filedialog, ttk
from PIL import Image, ImageTk
import os

class VeraEditor:
    def __init__(self, root):
        self.root = root
        self.root.title("VERA Sprite/Tile Editor")
        self.root.geometry("1024x600")
        
        self.config_file = "last_path.txt"
        self.image = None
        self.zoom_level = 1.0
        self.zoom_levels = [1.0, 1.5, 2.0, 3.0, 5.0, 7.5, 10.0, 15.0, 20.0] # Limitato a 2000% max
        self.zoom_idx = 0
        self.selection_type = tk.StringVar(value="Sprite")

        self.setup_menu()
        self.create_widgets()
        self.load_last_image()

    def setup_menu(self):
        menubar = tk.Menu(self.root)
        filemenu = tk.Menu(menubar, tearoff=0)
        filemenu.add_command(label="Open", command=self.load_image)
        filemenu.add_command(label="Exit", command=self.root.quit)
        menubar.add_cascade(label="File", menu=filemenu)
        self.root.config(menu=menubar)

    def create_widgets(self):
        self.paned = tk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        self.paned.pack(fill=tk.BOTH, expand=True)

        self.sidebar = tk.Frame(self.paned, width=200, bg='lightgray')
        self.paned.add(self.sidebar, minsize=200)

        # Strumenti
        tk.Label(self.sidebar, text="Zoom:", bg='lightgray').pack(pady=(10,0))
        self.zoom_label = tk.Label(self.sidebar, text="100%", bg='lightgray')
        self.zoom_label.pack()

        tk.Label(self.sidebar, text="Mode:", bg='lightgray').pack(pady=(10,0))
        tk.Radiobutton(self.sidebar, text="Sprite", variable=self.selection_type, value="Sprite", bg='lightgray').pack()
        tk.Radiobutton(self.sidebar, text="Tile", variable=self.selection_type, value="Tile", bg='lightgray').pack()

        tk.Label(self.sidebar, text="Size:", bg='lightgray').pack(pady=(10,0))
        self.size_cb = ttk.Combobox(self.sidebar, state="readonly", values=["8x8", "16x16", "32x32", "64x64"])
        self.size_cb.pack()
        self.size_cb.current(0)

        tk.Label(self.sidebar, text="Selections:", bg='lightgray').pack(pady=(10,0))
        self.sel_listbox = tk.Listbox(self.sidebar)
        self.sel_listbox.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # Canvas
        self.canvas = tk.Canvas(self.paned, bg='black')
        self.paned.add(self.canvas, minsize=400)
        
        self.canvas.bind("<MouseWheel>", self.on_zoom)
        self.canvas.bind("<Button-4>", self.on_zoom)
        self.canvas.bind("<Button-5>", self.on_zoom)

    def on_zoom(self, event):
        if not self.image: return
        
        direction = 1 if (event.delta > 0 or event.num == 4) else -1
        new_idx = max(0, min(len(self.zoom_levels) - 1, self.zoom_idx + direction))
        
        if new_idx != self.zoom_idx:
            x_mouse = self.canvas.canvasx(event.x)
            y_mouse = self.canvas.canvasy(event.y)
            
            self.zoom_idx = new_idx
            self.zoom_level = self.zoom_levels[self.zoom_idx]
            self.zoom_label.config(text=f"{int(self.zoom_level * 100)}%")
            self.display_image()
            
            self.canvas.xview_moveto(x_mouse / self.canvas.bbox("all")[2] if self.canvas.bbox("all") else 0)
            self.canvas.yview_moveto(y_mouse / self.canvas.bbox("all")[3] if self.canvas.bbox("all") else 0)

    def display_image(self):
        if not self.image: return
        new_w, new_h = int(self.image.width * self.zoom_level), int(self.image.height * self.zoom_level)
        resized_img = self.image.resize((new_w, new_h), Image.NEAREST)
        self.tk_image = ImageTk.PhotoImage(resized_img)
        
        self.canvas.delete("image")
        self.canvas.create_image(0, 0, image=self.tk_image, anchor=tk.NW, tags="image")
        self.canvas.config(scrollregion=self.canvas.bbox(tk.ALL))
        self.canvas.tag_lower("image")

    def load_image(self):
        path = filedialog.askopenfilename()
        if path:
            self.open_image(path)
            self.save_last_path(path)

    def open_image(self, path):
        self.image = Image.open(path).convert('RGB')
        self.root.update()
        self.zoom_idx = 0
        self.zoom_level = self.zoom_levels[self.zoom_idx]
        self.zoom_label.config(text="100%")
        self.display_image()

    def load_last_image(self):
        if os.path.exists(self.config_file):
            with open(self.config_file, 'r') as f:
                path = f.read().strip()
                if os.path.exists(path):
                    self.open_image(path)

    def save_last_path(self, path):
        with open(self.config_file, 'w') as f:
            f.write(path)

if __name__ == "__main__":
    root = tk.Tk()
    app = VeraEditor(root)
    root.mainloop()
