"""Map Details & Artwork tab: every optional menu input, with in-game previews.

All drawing lives in menupreviews; this module only lays out the inputs, caches
decoded images and turns clicks on the preview into settings:
* Globe: drag to spin, click to place the map (BO2 longitude/latitude).
* Map selection: click to place the start location on the Blit.
"""
from __future__ import annotations

import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from PIL import Image, ImageTk

from . import menupreviews as mp
from .menuart import SIZES, artwork_warning, read_art

# Optional images besides the three map layers: (settings key, label, hint).
EXTRA_IMAGES = [
    ('menu_icon', 'Globe icon', 'Transparent PNG, drawn on the globe and in the lobby list. Default: cut from your art.'),
    ('menu_preview', 'Lobby preview', 'Custom Games map picture. Fills the whole frame. Default: your completed art.'),
    ('menu_loading', 'Loading screen', 'Full-screen image while the map loads. Default: your completed art.'),
]
TEXT_FIELDS = [
    ('menu_title', 'Map name', 'Globe signpost, map selection, lobby and loading screen.'),
    ('menu_description', 'Description', 'Under the map name on the map selection screen.'),
    ('menu_mode', 'Mode line', 'Replaces "TRANZIT" under the map name in the lobby.'),
    ('menu_location', 'Location', 'Under the map name on the loading screen.'),
]
MODES = ['Globe', 'Map selection', 'Focused selection', 'Lobby', 'Loading screen', 'Large', 'Blur', 'Blit']
ARTWORK_KEYS = ('menu_title', 'menu_description', 'menu_mode', 'menu_location', 'menu_blit', 'menu_large',
                'menu_blur', 'menu_icon', 'menu_preview', 'menu_loading', 'menu_location_point',
                'menu_longitude', 'menu_latitude', 'project')


class ArtworkTab:
    """Mixin for gui.Launcher (uses its vars, _group, _button, _save, root)."""

    def _artwork(self):
        tab = self.art_tab
        tab.columnconfigure(0, weight=0, minsize=430)
        tab.columnconfigure(1, weight=1)
        tab.rowconfigure(0, weight=1)
        self.art_cache, self.globe, self.globe_lon, self.preview_box = {}, None, 130.0, None
        self._drag = None
        page = ttk.Frame(tab)
        page.grid(row=0, column=0, sticky='nsew', padx=(0, 12))
        inputs = self._scrollable(page)

        text = self._group(inputs, 'Text shown in game')
        text.pack(fill='x')
        for key, label, hint in TEXT_FIELDS:
            ttk.Label(text, text=label, font=('Segoe UI', 9, 'bold')).pack(anchor='w')
            entry = ttk.Entry(text, textvariable=self.vars[key])
            entry.pack(fill='x', pady=(2, 0))
            entry.bind('<FocusIn>', lambda _e, k=key: self._focus_preview(k))
            self.action_widgets.append(entry)
            ttk.Label(text, text=hint, style='Muted.TLabel', wraplength=390).pack(anchor='w', pady=(0, 6))

        layers = self._group(inputs, 'Map selection layers')
        layers.pack(fill='x', pady=(8, 0))
        ttk.Label(layers, text='Aligned layers: three images on the same canvas. Large has a transparent cutout, Blit fills it, '
                  'Blur is the blurred background. Stock layout: Blit 512 × 256, Large and Blur 2048 × 2048.',
                  style='Muted.TLabel', wraplength=390).pack(anchor='w', pady=(0, 6))
        self.upload_labels = {}
        for role in SIZES:
            self._upload_row(layers, 'menu_' + role, role.title(), lambda r=role: self._choose_art(r))
        point = ttk.Frame(layers)
        point.pack(fill='x', pady=(4, 0))
        ttk.Label(point, text='Start location (Blit pixels x,y)').pack(side='left')
        entry = ttk.Entry(point, textvariable=self.vars['menu_location_point'], width=12)
        entry.pack(side='left', padx=6)
        self.action_widgets.append(entry)
        ttk.Label(layers, text='Click the Map selection preview to place it. Blank: where the Blit is densest.',
                  style='Muted.TLabel', wraplength=390).pack(anchor='w')
        self._button(layers, 'Clear map layers', self._clear_art).pack(anchor='w', pady=(6, 0))

        extra = self._group(inputs, 'More images (optional)')
        extra.pack(fill='x', pady=(8, 0))
        for key, label, hint in EXTRA_IMAGES:
            self._upload_row(extra, key, label, lambda k=key, l=label: self._choose_extra(k, l))
            ttk.Label(extra, text=hint, style='Muted.TLabel', wraplength=390).pack(anchor='w', pady=(0, 6))

        globe = self._group(inputs, 'Globe position')
        globe.pack(fill='x', pady=(8, 0))
        row = ttk.Frame(globe)
        row.pack(fill='x')
        for key, label in (('menu_longitude', 'Longitude'), ('menu_latitude', 'Latitude')):
            ttk.Label(row, text=label).pack(side='left')
            entry = ttk.Entry(row, textvariable=self.vars[key], width=8)
            entry.pack(side='left', padx=(4, 12))
            entry.bind('<FocusIn>', lambda _e: self._focus_preview('menu_longitude'))
            self.action_widgets.append(entry)
        ttk.Label(globe, text="BO2's own convention: east is negative (Alcatraz is 122, Origins -11). "
                  'Easiest: click the Globe preview. Drag to spin it.', style='Muted.TLabel', wraplength=390).pack(anchor='w', pady=(4, 0))

        song = self._group(inputs, 'Loading song (optional)')
        song.pack(fill='x', pady=(8, 0))
        entry = ttk.Entry(song, textvariable=self.vars['loading_song'])
        entry.pack(fill='x', pady=3)
        self.action_widgets.append(entry)
        buttons = ttk.Frame(song)
        buttons.pack(fill='x')
        self._button(buttons, 'Upload song…', self._choose_song).pack(side='left')
        self._button(buttons, 'Clear song', lambda: self.vars['loading_song'].set('')).pack(side='left', padx=5)
        ttk.Label(song, text='MP3, WAV, FLAC, OGG or M4A, played while the map loads.',
                  style='Muted.TLabel', wraplength=390).pack(anchor='w', pady=(4, 0))

        preview = self._group(tab, 'In-game preview')
        preview.grid(row=0, column=1, sticky='nsew')
        bar = ttk.Frame(preview)
        bar.pack(fill='x')
        self.preview_mode = tk.StringVar(value='Globe')
        for mode in MODES[:5]:
            ttk.Radiobutton(bar, text=mode, value=mode, variable=self.preview_mode,
                            command=self._art_preview, style='Toolbutton').pack(side='left', padx=(0, 4))
        layers_menu = ttk.Combobox(bar, state='readonly', width=8, values=['Layer…', *MODES[5:]])
        layers_menu.set('Layer…')
        layers_menu.pack(side='right')
        layers_menu.bind('<<ComboboxSelected>>', lambda _e: (self.preview_mode.set(layers_menu.get()), self._art_preview()))
        self.preview_canvas = tk.Canvas(preview, width=640, height=360, bg='#0c0e12', highlightthickness=0, cursor='hand2')
        self.preview_canvas.pack(fill='both', expand=True, pady=8)
        self.preview_canvas.bind('<Configure>', self._preview_resized)
        self.preview_canvas.bind('<ButtonPress-1>', self._preview_press)
        self.preview_canvas.bind('<B1-Motion>', self._preview_drag)
        self.preview_canvas.bind('<ButtonRelease-1>', self._preview_release)
        self.art_hint = ttk.Label(preview, text='', style='Muted.TLabel', wraplength=640)
        self.art_hint.pack(anchor='w')
        self.art_status = ttk.Label(preview, text='', wraplength=640, style='Muted.TLabel')
        self.art_status.pack(anchor='w', pady=(2, 0))
        ttk.Label(preview, text='Approximate stock layout drawn with your BO2 install\'s own menu textures. '
                  'Check final framing in game.', style='Muted.TLabel', wraplength=640, font=('Segoe UI', 9)).pack(anchor='w', pady=(4, 0))

    def _preview_resized(self, event):
        for name in ('art_hint', 'art_status', 'art_note'):
            label = getattr(self, name, None)   # resize can fire while the tab is still being built
            if label is not None:
                label.configure(wraplength=max(240, event.width - 8))
        self._art_preview()

    # ------------------------------------------------------------- inputs

    def _upload_row(self, parent, key, label, command):
        ttk.Label(parent, text=label, font=('Segoe UI', 9, 'bold')).pack(anchor='w')
        row = ttk.Frame(parent)
        row.pack(fill='x', pady=(2, 4))
        self.upload_labels[key] = ttk.Label(row, text='No image selected', width=26, style='Muted.TLabel')
        self.upload_labels[key].pack(side='left', fill='x', expand=True)
        self._button(row, 'Clear', lambda: (self.vars[key].set(''), self._save())).pack(side='right')
        self._button(row, 'Upload…', command).pack(side='right', padx=4)

    def _focus_preview(self, key):
        mode = {'menu_title': None, 'menu_description': 'Map selection', 'menu_mode': 'Lobby',
                'menu_location': 'Loading screen', 'menu_longitude': 'Globe'}.get(key)
        if mode and self.preview_mode.get() != mode:
            self.preview_mode.set(mode)
            self._art_preview()

    def _clear_art(self):
        for role in SIZES:
            self.vars['menu_' + role].set('')
        self._save()

    def _choose_art(self, role):
        path = filedialog.askopenfilename(parent=self.root, title=f'Upload {role.title()} — aligned canvas or stock artwork',
                                          filetypes=[('Map images', '*.png *.jpg *.jpeg *.tga'), ('All files', '*.*')])
        if not path:
            return
        try:
            read_art(path, role, check_size=False)
        except (OSError, ValueError) as error:
            messagebox.showerror('Invalid map image', str(error), parent=self.root)
            return
        self.vars['menu_' + role].set(path)
        self._save()

    def _choose_extra(self, key, label):
        path = filedialog.askopenfilename(parent=self.root, title=f'Upload {label}',
                                          filetypes=[('Images', '*.png *.jpg *.jpeg *.tga *.webp'), ('All files', '*.*')])
        if not path:
            return
        try:
            Image.open(path).verify()
        except (OSError, ValueError) as error:
            messagebox.showerror('Invalid image', str(error), parent=self.root)
            return
        self.vars[key].set(path)
        self.preview_mode.set({'menu_icon': 'Globe', 'menu_preview': 'Lobby', 'menu_loading': 'Loading screen'}[key])
        self._save()

    def _choose_song(self):
        path = filedialog.askopenfilename(parent=self.root, title='Upload loading song',
            filetypes=[('Audio files', '*.mp3 *.wav *.flac *.ogg *.m4a'), ('All files', '*.*')])
        if path:
            self.vars['loading_song'].set(path)
            self._save()

    def _image(self, key, role=None):
        """Decoded image for a settings key, cached by path, mtime and size."""
        path = self.vars[key].get()
        if not path:
            return None
        stat = Path(path).stat()
        cache_key = (path, stat.st_mtime_ns, stat.st_size)
        if self.art_cache.get(key, (None,))[0] != cache_key:
            image = read_art(path, role, check_size=False) if role else Image.open(path).convert('RGBA')
            self.art_cache[key] = (cache_key, image)
        return self.art_cache[key][1]

    # ------------------------------------------------------------ preview

    def _float(self, key):
        try:
            return float(self.vars[key].get())
        except ValueError:
            return 0.0

    def _art_preview(self):
        canvas = getattr(self, 'preview_canvas', None)
        if canvas is None:
            return
        images, errors = {}, []
        for key, label in self.upload_labels.items():
            name = Path(self.vars[key].get()).name
            label.configure(text=(name[:30] + '…' if len(name) > 31 else name) or 'No image selected')
        for role in SIZES:
            try:
                image = self._image('menu_' + role, role)
                if image is not None:
                    images[role] = image
            except (OSError, ValueError) as error:
                errors.append(f'{role.title()}: {error}')
        extra = {}
        for key, _label, _hint in EXTRA_IMAGES:
            try:
                extra[key] = self._image(key)
            except (OSError, ValueError) as error:
                extra[key] = None
                errors.append(f'{key[5:].title()}: {error}')
        title = self.vars['menu_title'].get() or self.vars['project'].get() or 'Your Map'
        description = self.vars['menu_description'].get() or 'Your map description appears here.'
        mode = self.preview_mode.get()
        bo2 = self.vars['bo2'].get()
        hints = {'Globe': 'Drag to spin the globe. Click it to place your map there.',
                 'Map selection': 'Click to place the start location (the box players pick).',
                 'Focused selection': 'After picking the start location: Blur behind, sharp Blit, option list.'}
        if mode == 'Globe':
            if self.globe is None:
                self.globe = mp.Globe(mp.stock_texture(bo2, 'globe_map_zm'), 560)
            icon = extra.get('menu_icon') or (images.get('blit') if images.get('blit') is not None else None)
            markers = [(lon, lat, mp.stock_texture(bo2, img), name, False) for lon, lat, img, name in mp.stock_maps(bo2)]
            markers.append((self._float('menu_longitude'), self._float('menu_latitude'), icon, title, True))
            frame = self.globe.scene(self.globe_lon, markers, mp.stock_texture(bo2, 'menu_zm_background_main'))
        elif mode in ('Map selection', 'Focused selection'):
            frame = mp.map_selection(images, title, description, focused=mode == 'Focused selection',
                                     point=self.vars['menu_location_point'].get())
        elif mode == 'Lobby':
            frame = mp.lobby(images, extra.get('menu_preview'), title, self.vars['menu_mode'].get(), bo2)
        elif mode == 'Loading screen':
            frame = mp.loading(images, extra.get('menu_loading'), title, self.vars['menu_location'].get())
        else:
            layer = images.get(mode.lower())
            frame = (layer.convert('RGB') if layer is not None
                     else mp._placeholder(mp.SCREEN, f'Upload {mode} to preview it.').convert('RGB'))
        width, height = max(320, canvas.winfo_width()), max(180, canvas.winfo_height())
        scale = min(width / frame.width, height / frame.height)
        shown = frame.resize((max(1, int(frame.width * scale)), max(1, int(frame.height * scale))), Image.Resampling.LANCZOS)
        self.preview_box = ((width - shown.width) // 2, (height - shown.height) // 2, scale)
        self.preview_photo = ImageTk.PhotoImage(shown, master=self.root)
        canvas.delete('all')
        canvas.create_image(self.preview_box[0], self.preview_box[1], anchor='nw', image=self.preview_photo)
        self.art_hint.configure(text=hints.get(mode, ''))
        missing = [r.title() for r in SIZES if r not in images]
        warning = artwork_warning(images) if len(images) == 3 else None
        self.art_status.configure(text='\n'.join(errors) if errors else (
            'Map selection layers still needed: ' + ', '.join(missing) if missing else
            ('Warning: ' + warning if warning else 'Map selection layers use a supported layout.')))

    def _screen_point(self, event):
        """Canvas event -> 1280 x 720 menu-space point, or None outside the frame."""
        if self.preview_box is None:
            return None
        left, top, scale = self.preview_box
        x, y = (event.x - left) / scale, (event.y - top) / scale
        if 0 <= x < mp.SCREEN[0] and 0 <= y < mp.SCREEN[1]:
            return x, y
        return None

    def _preview_press(self, event):
        self._drag = (event.x, self.globe_lon, False)

    def _preview_drag(self, event):
        if self._drag is None or self.preview_mode.get() != 'Globe' or self.preview_box is None:
            return
        start, lon, _moved = self._drag
        if abs(event.x - start) > 3:
            self._drag = (start, lon, True)
            self.globe_lon = lon - (event.x - start) / self.preview_box[2] * 0.35
            now = time.monotonic()
            if now - getattr(self, '_last_spin', 0) > 0.05:
                self._last_spin = now
                self._art_preview()

    def _preview_release(self, event):
        moved = self._drag is not None and self._drag[2]
        self._drag = None
        point = self._screen_point(event)
        mode = self.preview_mode.get()
        if moved:
            self._art_preview()
            return
        if point is None:
            return
        if mode == 'Globe' and self.globe is not None:
            spot = self.globe.pick(self.globe_lon, point[0] - self.globe.origin[0], point[1] - self.globe.origin[1])
            if spot is not None:
                self.vars['menu_longitude'].set(str(spot[0]))
                self.vars['menu_latitude'].set(str(spot[1]))
                self._save()
        elif mode == 'Map selection':
            canvas_point = mp.screen_to_canvas(self._layers(), point)
            if canvas_point is not None:
                self.vars['menu_location_point'].set(f'{canvas_point[0]},{canvas_point[1]}')
                self._save()

    def _layers(self):
        images = {}
        for role in SIZES:
            try:
                image = self._image('menu_' + role, role)
            except (OSError, ValueError):
                image = None
            if image is not None:
                images[role] = image
        return images
