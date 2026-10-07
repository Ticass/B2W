"""Launcher state transitions without running game tools or modifying maps."""
from pathlib import Path
import tempfile
import tkinter as tk
import unittest
from unittest.mock import patch
from waw2bo2.gui import Launcher
from waw2bo2.launcher import Settings


class DesktopUITests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        try:
            self.root = tk.Tk()
        except tk.TclError as error:
            self.skipTest(str(error))
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        with patch('waw2bo2.gui.discover', side_effect=lambda s: s):
            self.app = Launcher(self.root, settings=Settings(work=self.temp.name),
                                settings_path=Path(self.temp.name) / 'settings.json')
        self.root.update_idletasks()

    def test_missing_requirements_block_native_build(self):
        with patch('waw2bo2.gui.messagebox.showinfo') as dialog, patch.object(self.app, '_start') as start:
            self.app.build_button.invoke()
            dialog.assert_called_once()
            start.assert_not_called()
            self.assertEqual(self.app.tabs.select(), str(self.app.setup_tab))

    def test_install_and_launch_require_completed_outputs(self):
        self.assertIn('disabled', self.app.install_button.state())
        self.assertIn('disabled', self.app.launch_button.state())

    def test_progress_and_failure_do_not_display_completion(self):
        self.app._line('== 3b. compiling map scripts')
        self.assertEqual(self.app.current_step, 4)
        self.app._finished('build', 7, 'Linker failed')
        self.assertIn('failed', self.app.status.get())
        self.assertLess(self.app.progress['value'], 8)
        self.assertIn('Linker failed', self.app.console.get('1.0', 'end'))

    def test_worker_messages_are_consumed_on_ui_thread(self):
        self.app.events.put(('line', '== 2. staging assets'))
        self.app.events.put(('line', 'WARNING: unsupported source feature'))
        self.app._poll()
        self.assertEqual(self.app.current_step, 2)
        self.assertEqual(self.app.warnings, 1)

    def test_asset_progress_updates_current_step_without_advancing_it(self):
        self.app._line('== 2. staging assets')
        self.app._line('[staging] Audio: 20/100 completed')
        self.assertEqual(self.app.current_step, 2)
        self.assertIn('Step 3 of 8', self.app.status.get())
        self.assertIn('Audio: 20/100', self.app.status.get())
        self.app.verbose.set(True)
        self.assertTrue(self.app._snapshot().verbose)

    def test_activity_repeating_previous_warning_does_not_count_it_again(self):
        self.app._line('WARNING: missing optional asset')
        self.app._line('[activity] Process still running. Last output: WARNING: missing optional asset')
        self.assertEqual(self.app.warnings, 1)

    def test_advanced_paths_and_reports_are_accessible(self):
        self.app._toggle_advanced()
        self.assertEqual(self.app.advanced.winfo_manager(), 'grid')
        self.app._toggle_advanced()
        self.assertFalse(self.app.advanced.winfo_manager())
        self.app._show_reports()
        self.assertEqual(self.app.tabs.select(), str(self.app.reports_tab))
        self.assertIn('report', self.app.report_text.get('1.0', 'end').lower())

    def test_artwork_tab_and_details_preview(self):
        self.app.tabs.select(self.app.art_tab)
        self.app.vars['menu_title'].set('My Custom Map')
        self.app.vars['menu_description'].set('Survive here.')
        self.app.preview_mode.set('Loading screen')
        self.app._art_preview()
        texts = [self.app.preview_canvas.itemcget(item, 'text') for item in self.app.preview_canvas.find_all()
                 if self.app.preview_canvas.type(item) == 'text']
        self.assertIn('MY CUSTOM MAP', texts)
        self.assertIn('Survive here.', texts)
        self.assertEqual(self.app._snapshot().menu_title, 'My Custom Map')

    def test_extract_all_is_available_without_a_selected_map(self):
        with patch('waw2bo2.gui.preflight', return_value=[]), patch.object(self.app, '_start') as start:
            self.app.extract_button.invoke()
        self.assertEqual(start.call_args.args[0], 'extract')

    def test_extraction_completion_does_not_claim_a_map_was_built_or_installed(self):
        self.app._finished('extract', 0, '')
        self.assertIn('game assets extracted', self.app.status.get())
        self.assertNotIn('Installed to Plutonium.', self.app.console.get('1.0', 'end'))
        self.assertIn('disabled', self.app.install_button.state())
