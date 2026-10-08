"""Native Windows mod-tools launcher for the existing conversion pipeline."""
from __future__ import annotations

from dataclasses import replace
from datetime import datetime
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext, ttk
import webbrowser
from PIL import Image, ImageTk
from .menuart import SIZES, read_art

from .launcher import (BuildPaths, ProcessRunner, Settings, build_command, cli_command,
                       discover, map_fastfiles, perform_build, perform_extract_all, preflight, project_name, user_directory)
from .resources import resource_root
from .diagnostics import create_bundle


STEPS = ['Extract source map', 'Prepare BO2 references', 'Convert map assets',
         'Build gameplay & menus', 'Compile map scripts', 'Link converted map',
         'Verify materials', 'Assemble map package']
STAGE_INDEX = {'0': 0, '1': 1, '1b': 1, '1c': 1, '2': 2, '3a': 3, '3b': 4, '4': 5, '6': 6, '7': 7}
COLORS = {'bg': '#ededed', 'ink': '#242b32', 'muted': '#626b73', 'accent': '#246b99',
          'console': '#11171d', 'green': '#217444', 'amber': '#ac680b', 'red': '#af3434'}


class Launcher(ttk.Frame):
    def __init__(self, root: tk.Tk, *, settings: Settings | None = None, settings_path: Path | None = None):
        super().__init__(root)
        self.root = root
        self.settings_path = settings_path
        self.settings = discover(settings or Settings.load(settings_path))
        self.events: queue.Queue = queue.Queue()
        self.runner: ProcessRunner | None = None
        self.busy = False
        self.started = 0.0
        self.active_settings: Settings | None = None
        self.last_result = ''
        self.current_step = -1
        self.warnings = 0
        self.errors = 0
        self.action_widgets: list = []
        self.vars = {key: tk.StringVar(value=getattr(self.settings, key)) for key in
                     ('waw', 'bo2', 'waw_tools', 't4', 't6', 'decoder', 'work', 'fastfile', 'project',
                      'menu_title', 'menu_description', 'menu_blit', 'menu_large', 'menu_blur')}
        self.art_cache = {}
        self.source_fx = tk.BooleanVar(value=self.settings.source_fx)
        self.bo2_stock_perks = tk.BooleanVar(value=self.settings.bo2_stock_perks)
        self.redump = tk.BooleanVar(value=self.settings.redump)
        self.verbose = tk.BooleanVar(value=self.settings.verbose)
        self.status = tk.StringVar(value='Choose a WaW map to begin.')
        self.elapsed = tk.StringVar(value='Ready')
        self.output_text = tk.StringVar()
        self.count_text = tk.StringVar(value='0 warnings    0 errors')
        self._theme()
        self._menus()
        self._layout()
        self.pack(fill='both', expand=True)
        for variable in self.vars.values():
            variable.trace_add('write', lambda *_: self._refresh())
        for key in ('menu_title', 'menu_description', 'menu_blit', 'menu_large', 'menu_blur', 'project'):
            self.vars[key].trace_add('write', lambda *_: self._art_preview())
        self._art_preview()
        self.source_fx.trace_add('write', lambda *_: self._refresh())
        self.bo2_stock_perks.trace_add('write', lambda *_: self._refresh())
        self._refresh()
        if self.vars['fastfile'].get():
            self.status.set('Map selected. Click Build Map when setup is ready.')
        self._log('WaW → BO2 Mod Tools ready. Choose your source map, then click Build Map.', 'info')
        self.root.after(100, self._poll)
        self.root.protocol('WM_DELETE_WINDOW', self._close)

    def _theme(self):
        self.root.title('WaW → BO2 Mod Tools')
        self.root.geometry('1160x820')
        self.root.minsize(1000, 720)
        self.root.configure(bg=COLORS['bg'])
        style = ttk.Style(self.root)
        if 'vista' in style.theme_names():
            style.theme_use('vista')
        else:
            style.theme_use('clam')
        self.root.option_add('*Font', ('Segoe UI', 10))
        style.configure('TFrame', background=COLORS['bg'])
        style.configure('TLabel', background=COLORS['bg'], foreground=COLORS['ink'])
        style.configure('TLabelframe', background=COLORS['bg'])
        style.configure('TLabelframe.Label', foreground=COLORS['accent'], background=COLORS['bg'])
        style.configure('Muted.TLabel', foreground=COLORS['muted'])
        style.configure('Title.TLabel', font=('Segoe UI', 16, 'bold'))
        style.configure('Heading.TLabel', font=('Segoe UI', 10, 'bold'))
        style.configure('Build.TButton', font=('Segoe UI', 11, 'bold'), padding=(14, 9))
        style.configure('Treeview', rowheight=25, font=('Segoe UI', 9))
        style.configure('Treeview.Heading', font=('Segoe UI', 9, 'bold'))

    def _menus(self):
        menu = tk.Menu(self.root)
        file_menu = tk.Menu(menu, tearoff=False)
        file_menu.add_command(label='Open WaW map…', command=self._choose_map, accelerator='Ctrl+O')
        file_menu.add_command(label='Open build folder', command=self._open_build)
        file_menu.add_separator()
        file_menu.add_command(label='Exit', command=self._close)
        menu.add_cascade(label='File', menu=file_menu)
        tools_menu = tk.Menu(menu, tearoff=False)
        tools_menu.add_command(label='Game & tool paths', command=lambda: self.tabs.select(self.setup_tab))
        tools_menu.add_command(label='Conversion reports', command=self._show_reports)
        menu.add_cascade(label='Tools', menu=tools_menu)
        help_menu = tk.Menu(menu, tearoff=False)
        help_menu.add_command(label='Quick help', command=self._help)
        help_menu.add_command(label='Full usage guide', command=lambda: webbrowser.open('https://github.com/Ticass/B2W/blob/main/docs/USAGE.md'))
        help_menu.add_command(label='About', command=lambda: messagebox.showinfo('About',
            'WaW → BO2 Mod Tools\nWorld at War custom-map converter\n\nDevelopment preview. Review the conversion report and playtest the result.\n\nOpenAssetTools: GPL-3.0; see included licenses.', parent=self.root))
        menu.add_cascade(label='Help', menu=help_menu)
        self.root.configure(menu=menu)
        self.root.bind('<Control-o>', lambda _: self._choose_map())

    def _button(self, parent, text, command, **kwargs):
        button = ttk.Button(parent, text=text, command=command, **kwargs)
        self.action_widgets.append(button)
        return button

    def _group(self, parent, title, **kwargs):
        kwargs.setdefault('padding', 10)
        return ttk.LabelFrame(parent, text=title, **kwargs)

    def _layout(self):
        self.columnconfigure(1, weight=1)
        self.rowconfigure(0, weight=1)
        sidebar = ttk.Frame(self, padding=(12, 12, 8, 10))
        sidebar.grid(row=0, column=0, sticky='ns')
        brand = tk.Canvas(sidebar, width=155, height=128, bg='#20282f', highlightthickness=0)
        brand.pack(fill='x', pady=(0, 12))
        brand.create_text(78, 30, text='WORLD AT WAR', font=('Segoe UI', 11, 'bold'), fill='#d4dce2')
        brand.create_text(78, 65, text='→ BO2', font=('Segoe UI', 24, 'bold'), fill='white')
        brand.create_line(23, 94, 132, 94, fill='#557b94', width=2)
        brand.create_text(78, 111, text='MOD TOOLS', font=('Segoe UI', 10, 'bold'), fill='#9fbed1')
        game = self._group(sidebar, 'Game')
        game.pack(fill='x', pady=(0, 10))
        ttk.Label(game, text='Black Ops II Zombies', font=('Segoe UI', 9, 'bold')).pack(anchor='w')
        ttk.Label(game, text='Plutonium T6', style='Muted.TLabel').pack(anchor='w', pady=(2, 0))
        tool_group = self._group(sidebar, 'Tools')
        tool_group.pack(fill='x', pady=(0, 10))
        self._button(tool_group, 'Game Paths', lambda: self.tabs.select(self.setup_tab)).pack(fill='x', pady=2)
        self._button(tool_group, 'Map Details & Artwork', lambda: self.tabs.select(self.art_tab)).pack(fill='x', pady=2)
        self._button(tool_group, 'Build Folder', self._open_build).pack(fill='x', pady=2)
        self._button(tool_group, 'Conversion Report', self._show_reports).pack(fill='x', pady=2)
        self._button(tool_group, 'Quick Help', self._help).pack(fill='x', pady=2)
        session = self._group(sidebar, 'Process')
        session.pack(fill='x', pady=(0, 10))
        self.process_label = ttk.Label(session, text='No active build', wraplength=130)
        self.process_label.pack(anchor='w')
        ttk.Label(session, textvariable=self.elapsed, style='Muted.TLabel').pack(anchor='w', pady=(5, 0))
        self.stop_button = ttk.Button(sidebar, text='Stop Build', command=self._stop, state='disabled')
        self.stop_button.pack(fill='x')
        ttk.Label(sidebar, text='Source-first conversion\nLocal builds · No account',
                  style='Muted.TLabel', font=('Segoe UI', 8), justify='left').pack(side='bottom', anchor='w', pady=12)

        body = ttk.Frame(self, padding=(4, 12, 12, 8))
        body.grid(row=0, column=1, sticky='nsew')
        body.columnconfigure(0, weight=1)
        body.rowconfigure(1, weight=3)
        body.rowconfigure(4, weight=2)
        header = ttk.Frame(body)
        header.grid(row=0, column=0, sticky='ew', pady=(0, 10))
        ttk.Label(header, text='WaW → BO2 Map Converter', style='Title.TLabel').pack(side='left')
        ttk.Label(header, text='MOD TOOLS LAUNCHER', style='Muted.TLabel', font=('Segoe UI', 9)).pack(side='right')
        self.tabs = ttk.Notebook(body)
        self.tabs.grid(row=1, column=0, sticky='nsew')
        self.builder_tab = ttk.Frame(self.tabs, padding=12)
        self.setup_tab = ttk.Frame(self.tabs, padding=12)
        self.reports_tab = ttk.Frame(self.tabs, padding=12)
        self.art_tab = ttk.Frame(self.tabs, padding=12)
        self.tabs.add(self.builder_tab, text='  Mod Builder  ')
        self.tabs.add(self.art_tab, text='  Map Details & Artwork  ')
        self.tabs.add(self.setup_tab, text='  Setup  ')
        self.tabs.add(self.reports_tab, text='  Reports  ')
        self._builder()
        self._artwork()
        self._setup_tab()
        self._reports()
        progress_row = ttk.Frame(body)
        progress_row.grid(row=2, column=0, sticky='ew', pady=(10, 5))
        ttk.Label(progress_row, textvariable=self.status).pack(side='left')
        self.progress = ttk.Progressbar(body, maximum=len(STEPS), mode='determinate')
        self.progress.grid(row=3, column=0, sticky='ew', pady=(0, 8))
        console_frame = self._group(body, 'Build Console', padding=0)
        console_frame.grid(row=4, column=0, sticky='nsew')
        self.console = scrolledtext.ScrolledText(console_frame, height=11, bg=COLORS['console'], fg='#cbd5dc',
            insertbackground='white', font=('Consolas', 9), relief='flat', wrap='word', state='disabled', padx=10, pady=8)
        self.console.pack(fill='both', expand=True)
        for tag, color in [('error', '#ff8989'), ('warning', '#ebc174'), ('info', '#80bbdf'), ('success', '#8ed6a1')]:
            self.console.tag_configure(tag, foreground=color)
        footer = ttk.Frame(body)
        footer.grid(row=5, column=0, sticky='ew', pady=(6, 0))
        ttk.Label(footer, textvariable=self.count_text, style='Muted.TLabel').pack(side='left')
        ttk.Button(footer, text='Save Console', command=self._save_console).pack(side='right', padx=(6, 0))
        ttk.Button(footer, text='Clear Console', command=self._clear_console).pack(side='right')
        verbose_check = ttk.Checkbutton(footer, text='Verbose Console', variable=self.verbose)
        verbose_check.pack(side='right', padx=10)
        self.action_widgets.append(verbose_check)

    def _builder(self):
        tab = self.builder_tab
        tab.columnconfigure(0, weight=3)
        tab.columnconfigure(1, weight=2)
        tab.rowconfigure(1, weight=1)
        source = self._group(tab, 'Source WaW Map')
        source.grid(row=0, column=0, sticky='ew', padx=(0, 12), pady=(0, 10))
        source.columnconfigure(0, weight=1)
        ttk.Label(source, text='Choose the map .ff from its original mod folder.', style='Muted.TLabel').grid(row=0, column=0, columnspan=2, sticky='w')
        entry = ttk.Entry(source, textvariable=self.vars['fastfile'])
        entry.grid(row=1, column=0, sticky='ew', pady=(8, 10))
        self.action_widgets.append(entry)
        self._button(source, 'Browse…', self._choose_map).grid(row=1, column=1, padx=(6, 0), pady=(8, 10))
        ttk.Label(source, text='BO2 map name').grid(row=2, column=0, sticky='w')
        entry = ttk.Entry(source, textvariable=self.vars['project'])
        entry.grid(row=3, column=0, columnspan=2, sticky='ew', pady=(4, 0))
        self.action_widgets.append(entry)

        build = self._group(tab, 'Build Map')
        build.grid(row=1, column=0, sticky='nsew', padx=(0, 12))
        ttk.Label(build, text='Geometry · Scripts · Weapons · FX · Sounds', style='Muted.TLabel').pack(anchor='w')
        check = ttk.Checkbutton(build, text='Refresh source files (slower rebuild)', variable=self.redump)
        check.pack(anchor='w', pady=(8, 4))
        self.action_widgets.append(check)
        check = ttk.Checkbutton(build, text='Use BO2 stock perks (maps with BO2 perks only)', variable=self.bo2_stock_perks)
        check.pack(anchor='w', pady=(0, 4))
        self.action_widgets.append(check)
        self.build_button = self._button(build, 'Build Map', self._build, style='Build.TButton')
        self.build_button.pack(fill='x', pady=(5, 6))
        self.ready_label = ttk.Label(build, text='', wraplength=400, style='Muted.TLabel')
        self.ready_label.pack(anchor='w')
        ttk.Label(build, textvariable=self.output_text, wraplength=400, style='Muted.TLabel', font=('Segoe UI', 8)).pack(anchor='w', pady=(7, 0))

        checklist = self._group(tab, 'Build Steps')
        checklist.grid(row=0, column=1, sticky='nsew', rowspan=2)
        self.step_labels = []
        for index, label in enumerate(STEPS):
            widget = ttk.Label(checklist, text=f'○  {label}', style='Muted.TLabel')
            widget.pack(anchor='w', pady=3)
            self.step_labels.append(widget)
        ttk.Separator(checklist).pack(fill='x', pady=(9, 9))
        ttk.Label(checklist, text='Install & Run', style='Heading.TLabel').pack(anchor='w')
        self.install_button = self._button(checklist, 'Install to Plutonium', self._install)
        self.install_button.pack(fill='x', pady=(8, 5))
        self.launch_button = self._button(checklist, 'Launch Map', self._launch)
        self.launch_button.pack(fill='x')

    def _artwork(self):
        tab = self.art_tab
        tab.columnconfigure(0, weight=1)
        tab.columnconfigure(1, weight=1)
        details = self._group(tab, 'In-game map details')
        details.grid(row=0, column=0, sticky='nsew', padx=(0, 10))
        for key, label in [('menu_title', 'Map title'), ('menu_description', 'Map description')]:
            ttk.Label(details, text=label).pack(anchor='w')
            entry = ttk.Entry(details, textvariable=self.vars[key])
            entry.pack(fill='x', pady=(3, 8))
            self.action_widgets.append(entry)
        ttk.Label(details, text='Shown in map selection, the lobby, and the mod list.\nBlank fields use the BO2 map name.',
                  style='Muted.TLabel', wraplength=330).pack(anchor='w')
        uploads = self._group(tab, 'Upload map images')
        uploads.grid(row=1, column=0, sticky='nsew', padx=(0, 10), pady=(8, 0))
        self.upload_labels = {}
        for role, (w, h) in SIZES.items():
            ttk.Label(uploads, text=f'{role.title()} — {w} × {h} px' + (' · transparent PNG / TGA' if role == 'blit' else ' · PNG / JPG / TGA'),
                      font=('Segoe UI', 9, 'bold')).pack(anchor='w')
            row = ttk.Frame(uploads)
            row.pack(fill='x', pady=(3, 6))
            self.upload_labels[role] = ttk.Label(row, text='No image selected', width=18, style='Muted.TLabel')
            self.upload_labels[role].pack(side='left', fill='x', expand=True)
            self._button(row, 'Upload…', lambda r=role: self._choose_art(r)).pack(side='right')
        self._button(uploads, 'Clear artwork', self._clear_art).pack(anchor='w', pady=(3, 0))
        ttk.Label(uploads, text='Large supplies loading art and the 256 × 256 lobby thumbnail.\nUpload all three; no automatic cropping or blur.',
                  style='Muted.TLabel', wraplength=410, font=('Segoe UI', 9)).pack(anchor='w', pady=(6, 0))
        preview = self._group(tab, 'In-game artwork preview')
        preview.grid(row=0, column=1, rowspan=2, sticky='nsew')
        self.preview_mode = tk.StringVar(value='Map selection')
        modes = ttk.Combobox(preview, state='readonly', textvariable=self.preview_mode,
                            values=['Map selection', 'Large', 'Blur', 'Blit', 'Lobby thumbnail', 'Loading screen'])
        modes.pack(fill='x')
        modes.bind('<<ComboboxSelected>>', lambda _: self._art_preview())
        self.preview_canvas = tk.Canvas(preview, width=360, height=225, bg='#151a20', highlightthickness=0)
        self.preview_canvas.pack(fill='both', expand=True, pady=8)
        self.preview_canvas.bind('<Configure>', lambda _: self._art_preview())
        self.art_status = ttk.Label(preview, text='', wraplength=330, style='Muted.TLabel')
        self.art_status.pack(anchor='w')
        ttk.Label(preview, text='Approximate stock layout. Verify final framing in game.',
                  style='Muted.TLabel', wraplength=410, font=('Segoe UI', 9)).pack(anchor='w', pady=(4, 0))

    def _clear_art(self):
        for role in SIZES:
            self.vars['menu_' + role].set('')
        self._save()

    def _choose_art(self, role):
        path = filedialog.askopenfilename(parent=self.root, title=f'Upload {role.title()} ({SIZES[role][0]} × {SIZES[role][1]} px)',
                                          filetypes=[('Map images', '*.png *.jpg *.jpeg *.tga'), ('All files', '*.*')])
        if not path:
            return
        try:
            read_art(path, role)
        except (OSError, ValueError) as error:
            messagebox.showerror('Invalid map image', str(error), parent=self.root)
            return
        self.vars['menu_' + role].set(path)
        self._save()

    def _art_preview(self):
        canvas = self.preview_canvas
        canvas.delete('all')
        images, errors = {}, []
        for role in SIZES:
            path = self.vars['menu_' + role].get()
            name = Path(path).name
            self.upload_labels[role].configure(text=(name[:24] + '…' if len(name) > 25 else name) or 'No image selected')
            if not path:
                continue
            try:
                stat = Path(path).stat()
                key = (path, stat.st_mtime_ns, stat.st_size)
                if self.art_cache.get(role, (None,))[0] != key:
                    self.art_cache[role] = (key, read_art(path, role))
                images[role] = self.art_cache[role][1]
            except (OSError, ValueError) as error:
                errors.append(str(error))
        mode = self.preview_mode.get()
        role = {'Large': 'large', 'Blur': 'blur', 'Blit': 'blit', 'Lobby thumbnail': 'large',
                'Loading screen': 'large', 'Map selection': 'large'}[mode]
        image = images.get(role)
        width, height = max(360, canvas.winfo_width()), max(225, canvas.winfo_height())
        if image is not None:
            image = image.copy()
            if mode == 'Map selection' and 'blit' in images:
                # Measured stock Blit-to-Large projection (MAP_MENU_ASSETS.md).
                overlay = images['blit'].resize((320, 284), Image.Resampling.LANCZOS)
                image.alpha_composite(overlay, (864, 882))
            if mode == 'Lobby thumbnail':
                image = image.resize((256, 256), Image.Resampling.LANCZOS)
            if mode in ('Map selection', 'Loading screen'):
                image = image.resize((width - 20, max(80, (width - 20) * 9 // 16)), Image.Resampling.LANCZOS)
            image.thumbnail((width - 20, height - 78), Image.Resampling.LANCZOS)
            self.preview_photo = ImageTk.PhotoImage(image, master=self.root)
            canvas.create_image(width // 2, 8, anchor='n', image=self.preview_photo)
        else:
            canvas.create_text(width // 2, 55, text=f'Upload {role.title()} to preview {mode.lower()}', fill='#abb8c3', width=width - 24)
        title = self.vars['menu_title'].get() or self.vars['project'].get() or 'YOUR MAP TITLE'
        description = self.vars['menu_description'].get() or 'Your map description appears here.'
        canvas.create_text(12, height - 66, anchor='nw', text=title.upper(), fill='white', font=('Segoe UI', 11, 'bold'), width=width - 24)
        canvas.create_text(12, height - 42, anchor='nw', text=description, fill='#b4c1cb', width=width - 24, font=('Segoe UI', 9))
        missing = [r.title() for r in SIZES if r not in images]
        self.art_status.configure(text='\n'.join(errors) if errors else ('Still needed: ' + ', '.join(missing) if missing else 'All three images match the required resolutions.'))

    def _setup_tab(self):
        tab = self.setup_tab
        tab.columnconfigure(0, weight=1)
        intro = ttk.Frame(tab)
        intro.grid(row=0, column=0, sticky='ew', pady=(0, 8))
        ttk.Label(intro, text='Point to your game folders once. These settings are remembered.', style='Muted.TLabel').pack(side='left')
        self._button(intro, 'Auto-detect', self._autodetect).pack(side='right')
        games = self._group(tab, 'Game Installations')
        games.grid(row=1, column=0, sticky='ew')
        games.columnconfigure(1, weight=1)
        for row, (field, title) in enumerate([('waw', 'World at War'), ('bo2', 'Black Ops II + Mod Tools'), ('waw_tools', 'WaW Mod Tools (optional)')]):
            self._path_row(games, row, field, title)
        options = ttk.Frame(tab)
        options.grid(row=2, column=0, sticky='ew', pady=(7, 8))
        checkbox = ttk.Checkbutton(options, text='Recover missing FX from WaW Mod Tools sources', variable=self.source_fx)
        checkbox.pack(side='left')
        self.action_widgets.append(checkbox)
        self.advanced = self._group(tab, 'Advanced Paths')
        self.advanced.columnconfigure(1, weight=1)
        for row, (field, title) in enumerate([('t4', 'WaW extractor folder'), ('t6', 'BO2 bridge tools folder'),
                                            ('decoder', 'Audio decoder'), ('work', 'Build files folder')]):
            self._path_row(self.advanced, row, field, title)
        self.advanced_shown = False
        self._button(options, 'Advanced Paths', self._toggle_advanced).pack(side='right')
        cache = self._group(tab, 'Shared Game Assets')
        cache.grid(row=3, column=0, sticky='ew', pady=(0, 6))
        ttk.Label(cache, text='Build Map prepares shared assets automatically. Extract All prepares them in advance.',
                  style='Muted.TLabel').pack(side='left')
        self.extract_button = self._button(cache, 'Extract All', self._extract_all)
        self.extract_button.pack(side='right', padx=(8, 0))
        self.setup_tree = ttk.Treeview(tab, columns=('status', 'details'), show='tree headings', height=6)
        self.setup_tree.heading('#0', text='Requirement')
        self.setup_tree.heading('status', text='Status')
        self.setup_tree.heading('details', text='Details')
        self.setup_tree.column('#0', width=190, stretch=False)
        self.setup_tree.column('status', width=95, stretch=False)
        self.setup_tree.column('details', width=460)
        self.setup_tree.tag_configure('missing', foreground=COLORS['red'])
        self.setup_tree.tag_configure('ready', foreground=COLORS['green'])
        self.setup_tree.grid(row=4, column=0, sticky='nsew', pady=(4, 0))
        tab.rowconfigure(4, weight=1)

    def _path_row(self, parent, row, field, title):
        ttk.Label(parent, text=title).grid(row=row, column=0, sticky='w', padx=(0, 10), pady=3)
        entry = ttk.Entry(parent, textvariable=self.vars[field])
        entry.grid(row=row, column=1, sticky='ew', pady=3)
        self.action_widgets.append(entry)
        self._button(parent, 'Browse…', lambda: self._choose_path(field)).grid(row=row, column=2, padx=(6, 0), pady=3)

    def _reports(self):
        ttk.Label(self.reports_tab, text='Review unsupported features and compatibility decisions after building.', style='Muted.TLabel').pack(anchor='w', pady=(0, 8))
        self.report_text = scrolledtext.ScrolledText(self.reports_tab, height=12, font=('Consolas', 9), wrap='word', state='disabled')
        self.report_text.pack(fill='both', expand=True)
        row = ttk.Frame(self.reports_tab)
        row.pack(fill='x', pady=(8, 0))
        self._button(row, 'Refresh Report', self._load_report).pack(side='left')
        self._button(row, 'Save Diagnostics…', self._save_diagnostics).pack(side='left', padx=6)
        self._button(row, 'Open All Reports', self._open_reports).pack(side='right')

    def _save_diagnostics(self):
        bundle = getattr(self, 'diagnostics_path', None)
        if bundle is None or not bundle.is_file():
            try:
                bundle = create_bundle(self.active_settings or self._snapshot(), self.console.get('1.0', 'end'))
                self.diagnostics_path = bundle
            except OSError as error:
                messagebox.showerror('Diagnostics could not be saved', str(error), parent=self.root)
                return
        destination = filedialog.asksaveasfilename(parent=self.root, title='Save build diagnostics',
            initialfile=bundle.name, defaultextension='.zip', filetypes=[('Diagnostics ZIP', '*.zip')])
        if destination:
            try:
                if Path(destination).resolve() != bundle.resolve():
                    shutil.copy2(bundle, destination)
                self._log('Diagnostics saved: ' + destination, 'info')
            except OSError as error:
                messagebox.showerror('Diagnostics could not be saved', str(error), parent=self.root)

    def _snapshot(self) -> Settings:
        return Settings(**{key: value.get().strip() for key, value in self.vars.items()},
                        source_fx=self.source_fx.get(), bo2_stock_perks=self.bo2_stock_perks.get(),
                        redump=self.redump.get(), verbose=self.verbose.get())

    def _refresh(self):
        settings = self._snapshot()
        checks = preflight(settings, include_map=False)
        self.setup_tree.delete(*self.setup_tree.get_children())
        for check in checks:
            status = 'Ready' if check.ready else ('Required' if check.required else 'Optional')
            self.setup_tree.insert('', 'end', text=check.name, values=(status, check.detail),
                                   tags=('ready' if check.ready else 'missing' if check.required else '',))
        all_checks = preflight(settings)
        failures = [c for c in all_checks if c.required and not c.ready]
        if not self.busy:
            self.build_button.configure(state='normal')
            text = 'Ready to build. Shared game assets prepare automatically.' if not failures else f'{len(failures)} setup items need attention. Click Build Map to see what is missing.'
            self.ready_label.configure(text=text, foreground=COLORS['green'] if not failures else COLORS['muted'])
        try:
            paths = BuildPaths.for_settings(settings)
            complete = paths.complete(settings.project, bo2_stock_perks=settings.bo2_stock_perks)
            self.output_text.set('Build files are managed automatically. Use Build Folder to find your package.')
        except ValueError:
            complete = False
            self.output_text.set('')
        self.install_button.configure(state='normal' if complete and not self.busy else 'disabled')
        installed = self._installed(settings)
        self.launch_button.configure(state='normal' if installed and not self.busy else 'disabled')

    def _installed(self, settings):
        if not settings.project or not settings.bo2:
            return False
        folder = Path(os.environ.get('LOCALAPPDATA', str(Path.home()))) / 'Plutonium/storage/t6/mods' / settings.project
        try:
            paths = BuildPaths.for_settings(settings)
            receipt = json.loads((paths.root / 'installed.json').read_text(encoding='utf-8'))
            return receipt.get('output') == str(paths.output) and all((folder / name).is_file() for name in
                (settings.project + '.ff', settings.project + '.ipak', 'mod.ff', 'mod_load.ff', 'mod.json'))
        except (ValueError, OSError):
            return False

    def _choose_map(self):
        if self.busy:
            return
        initial = Path(os.environ.get('LOCALAPPDATA', str(Path.home()))) / 'Activision/CoDWaW/mods'
        path = filedialog.askopenfilename(parent=self.root, title='Choose the WaW map fastfile (not mod.ff)',
            initialdir=initial if initial.exists() else None, filetypes=[('WaW fastfiles', '*.ff')])
        if not path:
            return
        if Path(path).stem.lower() in {'mod', 'mod_load'} or Path(path).stem.lower().endswith('_patch'):
            maps = map_fastfiles(Path(path).parent)
            if len(maps) == 1:
                path = str(maps[0])
            else:
                messagebox.showinfo('Choose the map', 'Select the map fastfile itself, rather than mod.ff or a patch zone.', parent=self.root)
                return
        self.vars['fastfile'].set(path)
        self.vars['project'].set(project_name(path))
        self._save()
        self.tabs.select(self.builder_tab)
        self.status.set('Map selected. Click Build Map when setup is ready.')

    def _choose_path(self, field):
        if self.busy:
            return
        if field == 'decoder':
            path = filedialog.askopenfilename(parent=self.root, title='Choose the audio decoder', filetypes=[('Executable', '*.exe')])
        else:
            path = filedialog.askdirectory(parent=self.root, title='Choose ' + field.replace('_', ' '),
                                           initialdir=self.vars[field].get() or None)
        if path:
            self.vars[field].set(path)
            self._save()

    def _autodetect(self):
        if self.busy:
            return
        settings = discover(self._snapshot())
        for key, var in self.vars.items():
            var.set(getattr(settings, key))
        self._save()
        self._log('Checked Steam libraries and bundled tools. Review the Setup list.', 'info')

    def _toggle_advanced(self):
        self.advanced_shown = not self.advanced_shown
        if self.advanced_shown:
            self.advanced.grid(row=3, column=0, sticky='ew', pady=(0, 6))
        else:
            self.advanced.grid_remove()

    def _save(self):
        try:
            self._snapshot().save(self.settings_path)
        except OSError as error:
            self._log('Could not save settings: ' + str(error), 'warning')

    def _build(self):
        if self.busy:
            return
        settings = self._snapshot()
        failures = [c for c in preflight(settings) if c.required and not c.ready]
        if failures:
            messagebox.showinfo('Setup required', '\n\n'.join(c.name + '\n' + c.detail for c in failures), parent=self.root)
            self.tabs.select(self.art_tab if failures[0].name == 'Map artwork' else
                             self.setup_tab if any(c.name not in ('Source map', 'BO2 map name', 'Map artwork') for c in failures) else
                             self.art_tab if any(c.name == 'Map artwork' for c in failures) else self.builder_tab)
            return
        self._save()
        self.warnings = self.errors = 0
        self.count_text.set('0 warnings    0 errors')
        self.current_step = -1
        self.progress['value'] = 0
        for index, label in enumerate(self.step_labels):
            label.configure(text='○  ' + STEPS[index], foreground=COLORS['muted'])
        self._log('\nBuilding ' + settings.project + ' from ' + settings.fastfile, 'info')
        self._start('build', settings, lambda runner: perform_build(settings, runner))

    def _extract_all(self):
        if self.busy:
            return
        settings = self._snapshot()
        failures = [c for c in preflight(settings, include_map=False) if c.required and not c.ready
                    and c.name not in ('Audio decoder', 'WaW source tools')]
        if failures:
            messagebox.showinfo('Setup required', '\n\n'.join(c.name + '\n' + c.detail for c in failures), parent=self.root)
            return
        self._save()
        self._log('Extracting all installed game zones into the shared cache.', 'info')
        self._start('extract', settings, lambda runner: perform_extract_all(settings, runner))

    def _start(self, kind, settings, operation):
        self.busy = True
        self.started = time.monotonic()
        self.active_settings = replace(settings)
        self.diagnostics_path = None
        self.runner = ProcessRunner(lambda event, value: self.events.put((event, value)), verbose=settings.verbose)
        runner = self.runner
        for widget in self.action_widgets:
            widget.configure(state='disabled')
        self.stop_button.configure(state='normal')
        self.process_label.configure(text='Building map' if kind == 'build' else 'Installing map')
        self.status.set('Starting build…' if kind == 'build' else 'Installing to Plutonium…')
        if kind == 'extract':
            self.process_label.configure(text='Extracting game assets')
            self.status.set('Extracting all game assets…')
            self.stop_button.configure(text='Stop Extraction')

        def work():
            try:
                code = operation(runner)
                self.events.put(('finished', (kind, code, '')))
            except Exception as error:
                self.events.put(('finished', (kind, -1, str(error))))
        threading.Thread(target=work, name='map-' + kind, daemon=True).start()

    def _install(self):
        if self.busy:
            return
        settings = self._snapshot()
        paths = BuildPaths.for_settings(settings)
        if not paths.complete(settings.project, bo2_stock_perks=settings.bo2_stock_perks):
            return
        # Packaging already checks every required output and skips identical files.
        def install(runner):
            code = runner.run(cli_command('package', str(paths.stage), settings.project, '--bo2', settings.bo2,
                                        '--work', str(paths.mod)), paths.root / 'install.log')
            if not code and not runner.cancelled.is_set():
                (paths.root / 'installed.json').write_text(json.dumps({'output': str(paths.output)}), encoding='utf-8')
            return code
        self._start('install', settings, install)

    def _launch(self):
        if self.busy:
            return
        settings = self._snapshot()
        pluto = Path(os.environ.get('LOCALAPPDATA', str(Path.home()))) / 'Plutonium'
        boot = pluto / 'bin/plutonium-bootstrapper-win32.exe'
        if not self._installed(settings) or not boot.is_file():
            messagebox.showinfo('Plutonium required', 'Install the built map and configure Plutonium T6 first.', parent=self.root)
            return
        try:
            command = [str(boot), 't6zm', settings.bo2, '-lan', '+set', 'fs_game',
                       'mods/' + settings.project, '+devmap', settings.project]
            if os.name != 'nt':
                from .linuxruntime import launch_command, wine_environment
                command = launch_command(command)
            subprocess.Popen(command, cwd=pluto,
                             env=wine_environment() if os.name != 'nt' else None,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            self._log('Launched ' + settings.project + ' in Plutonium T6.', 'success')
        except OSError as error:
            messagebox.showerror('Launch failed', str(error), parent=self.root)

    def _stop(self):
        if self.runner and self.busy:
            self.stop_button.configure(state='disabled')
            self.status.set('Stopping this build…')
            threading.Thread(target=self.runner.cancel, daemon=True).start()

    def _poll(self):
        for _ in range(250):
            try:
                event, value = self.events.get_nowait()
            except queue.Empty:
                break
            if event == 'line':
                self._line(value)
            elif event == 'finished':
                self._finished(*value)
        if self.busy:
            seconds = int(time.monotonic() - self.started)
            self.elapsed.set(f'{seconds // 60:02d}:{seconds % 60:02d} elapsed')
        self.root.after(100, self._poll)

    def _line(self, line):
        import re
        if line.startswith('[activity]'):
            self._log(line, 'info')
            return
        if line.startswith('[staging]'):
            self.status.set(f'Step {self.current_step + 1} of {len(STEPS)} — ' + line[len('[staging] '):])
        stage = re.match(r'^==\s+(\d+[a-z]?)\.', line)
        if stage and stage.group(1) in STAGE_INDEX:
            self.current_step = STAGE_INDEX[stage.group(1)]
            for index, label in enumerate(self.step_labels):
                mark = '✓' if index < self.current_step else '▶' if index == self.current_step else '○'
                color = COLORS['green'] if index < self.current_step else COLORS['accent'] if index == self.current_step else COLORS['muted']
                label.configure(text=mark + '  ' + STEPS[index], foreground=color)
            self.status.set(f'Step {self.current_step + 1} of {len(STEPS)} — ' + STEPS[self.current_step])
            self.progress['value'] = self.current_step
        lower = line.lower()
        tag = 'info' if line.startswith('==') else ''
        if re.search(r'\b(error|exception|traceback)\b', lower) and '0 errors' not in lower:
            tag = 'error'
            self.errors += 1
        elif re.search(r'\bwarning\b', lower):
            tag = 'warning'
            self.warnings += 1
        self.count_text.set(f'{self.warnings} warnings    {self.errors} errors')
        self._log(line, tag)

    def _finished(self, kind, code, error):
        cancelled = self.runner is not None and self.runner.cancelled.is_set()
        self.busy = False
        self.stop_button.configure(state='disabled', text='Stop Build')
        for widget in self.action_widgets:
            widget.configure(state='normal')
        self.process_label.configure(text='No active build')
        self.last_result = 'Stopped' if cancelled else 'Failed' if code else 'Complete'
        if cancelled:
            self.status.set('Extraction stopped. Run Extract All again to resume.' if kind == 'extract' else
                            'Build stopped. No successful build was recorded.')
            self._log('Stopped the active build and its child tools.', 'warning')
        elif code:
            self.errors += 1
            self.status.set('Extraction failed. Run Extract All again to resume.' if kind == 'extract' else
                            'Build failed. Review the console and reports.')
            self._log(error or f'Native tool exited with code {code}.', 'error')
            self._failure_details()
            if self.active_settings:
                try:
                    self.diagnostics_path = create_bundle(self.active_settings, self.console.get('1.0', 'end'), error)
                    self._log('Diagnostics ZIP ready: ' + str(self.diagnostics_path), 'info')
                    self._log('Use Reports → Save Diagnostics to share the complete failure report.', 'info')
                except OSError as bundle_error:
                    self._log('Could not save diagnostics: ' + str(bundle_error), 'warning')
        else:
            if kind != 'extract':
                self.status.set('Map built. Install to Plutonium when ready.' if kind == 'build' else 'Installed. Launch Map to playtest.')
                self._log('Build complete. Package verified and ready to install.' if kind == 'build' else 'Installed to Plutonium.', 'success')
            if kind == 'extract':
                self.status.set('Shared game assets extracted. Ready to build maps.')
                self._log('Extract All complete. Future builds reuse the shared game cache.', 'success')
            if kind == 'build':
                self.progress['value'] = len(STEPS)
                for index, label in enumerate(self.step_labels):
                    label.configure(text='✓  ' + STEPS[index], foreground=COLORS['green'])
        self.count_text.set(f'{self.warnings} warnings    {self.errors} errors')
        self._load_report()
        self._refresh()

    def _failure_details(self):
        if not self.active_settings:
            return
        try:
            paths = BuildPaths.for_settings(self.active_settings)
        except ValueError:
            return  # Extraction can fail before a source map is selected.
        # Native tools redirect verbose output to files; surface the newest tail.
        candidates = list(paths.stage.glob('*.log')) + list(paths.mod.rglob('*.log'))
        candidates = [p for p in candidates if p.stat().st_mtime >= time.time() - (time.monotonic() - self.started) - 2]
        for path in sorted(candidates, key=lambda p: p.stat().st_mtime, reverse=True)[:2]:
            self._log('Native log: ' + str(path), 'info')
            try:
                with path.open('rb') as log:
                    log.seek(max(0, path.stat().st_size - 12000))
                    tail = log.read()
                text = tail.decode('utf-16' if b'\x00' in tail[:100] else 'utf-8-sig', errors='replace')
                for line in text.splitlines()[-35:]:
                    self._log(line)
            except OSError:
                pass

    def _log(self, text, tag=''):
        self.console.configure(state='normal')
        self.console.insert('end', text + '\n', tag)
        if int(self.console.index('end-1c').split('.')[0]) > 7000:
            self.console.delete('1.0', '1000.0')
        self.console.see('end')
        self.console.configure(state='disabled')

    def _clear_console(self):
        self.console.configure(state='normal')
        self.console.delete('1.0', 'end')
        self.console.configure(state='disabled')

    def _save_console(self):
        path = filedialog.asksaveasfilename(parent=self.root, title='Save build console', defaultextension='.log',
            initialfile='waw2bo2-build.log', filetypes=[('Build log', '*.log')])
        if path:
            try:
                Path(path).write_text(self.console.get('1.0', 'end-1c'), encoding='utf-8')
            except OSError as error:
                messagebox.showerror('Save failed', str(error), parent=self.root)

    def _paths(self):
        try:
            return BuildPaths.for_settings(self._snapshot())
        except ValueError:
            messagebox.showinfo('Select a map', 'Choose a source map first.', parent=self.root)
            return None

    def _open_folder(self, folder):
        folder.mkdir(parents=True, exist_ok=True)
        if os.name == 'nt':
            try:
                os.startfile(folder)
            except OSError as error:
                messagebox.showerror('Cannot open folder', str(error), parent=self.root)
        else:
            webbrowser.open(folder.as_uri())

    def _open_build(self):
        paths = self._paths()
        if paths:
            self._open_folder(paths.root)

    def _show_reports(self):
        self._load_report()
        self.tabs.select(self.reports_tab)

    def _open_reports(self):
        paths = self._paths()
        if paths:
            self._open_folder(paths.stage / 'zone_raw' / self.vars['project'].get())

    def _load_report(self):
        text = 'No conversion report yet. Build a map to create one.'
        try:
            settings = self._snapshot()
            paths = BuildPaths.for_settings(settings)
            report_file = paths.stage / 'zone_raw' / settings.project / 'bridge_stage.report.json'
            if report_file.exists():
                data = json.loads(report_file.read_text(encoding='utf-8'))
                errors = data.get('errors', [])
                warnings = data.get('warnings', [])
                text = f'{settings.project}\n{len(errors)} staging errors · {len(warnings)} staging warnings\n\n'
                for title, items in [('ERRORS', errors), ('WARNINGS / COMPATIBILITY', warnings)]:
                    text += title + '\n' + ('\n'.join(str(item) for item in items) or 'None') + '\n\n'
                text += 'Full report\n' + str(report_file) + '\n\n' + json.dumps(data, indent=2, ensure_ascii=False)
        except (ValueError, OSError) as error:
            text = 'Cannot read report: ' + str(error)
        self.report_text.configure(state='normal')
        self.report_text.delete('1.0', 'end')
        self.report_text.insert('end', text)
        self.report_text.configure(state='disabled')

    def _help(self):
        messagebox.showinfo('Quick start',
            '1. Open Setup and check the detected game folders.\n'
            '2. Browse to the WaW map .ff in its original mod folder.\n'
            '3. Click Build Map. The tool manages intermediate files.\n'
            '4. Review Reports, then click Install to Plutonium.\n'
            '5. Click Launch Map and compare it with the WaW original.\n\n'
            'BO2 Mod Tools and game assets are required. Portable builds include the converter, '
            'WaW extractor, BO2 bridge, and audio decoder.\n\n'
            'Builds stay separate from installed maps until you click Install. '
            'Close the game before installing changed files.', parent=self.root)

    def _close(self):
        if self.busy:
            if not messagebox.askyesno('Build in progress', 'Stop this build and close the launcher?', parent=self.root):
                return
            self._stop()
            self.root.after(100, self._close_when_idle)
            return
        self._save()
        self.root.destroy()

    def _close_when_idle(self):
        if self.busy:
            self.root.after(100, self._close_when_idle)
        else:
            self._close()


def main() -> None:
    if os.name != 'nt':
        from .linuxruntime import configure_localappdata
        try:
            configure_localappdata()
        except (RuntimeError, OSError, subprocess.SubprocessError):
            pass  # The GUI remains available to inspect paths without Wine.
    if os.name == 'nt':
        import ctypes
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (OSError, AttributeError):
            pass
    root = tk.Tk()
    Launcher(root)
    root.mainloop()


if __name__ == '__main__':
    main()
