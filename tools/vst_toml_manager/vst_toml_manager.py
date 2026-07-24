# /// script
# dependencies = [
#     "textual>=0.50.0",
#     "tomlkit>=0.12.0",
# ]
# ///

import os
import re
import sys
from pathlib import Path
import tomlkit
from textual.app import App, ComposeResult
from textual.containers import Container, Horizontal, Vertical
from textual.widgets import Header, Footer, ListView, ListItem, Label, Input, Switch, Button, Select, DataTable
from textual.binding import Binding
from textual.screen import ModalScreen

# Define path constants dynamically
def get_config_dir() -> Path:
    appdata = os.environ.get("APPDATA")
    if appdata:
        path = Path(appdata) / "musikcube"
    else:
        path = Path.home() / ".config" / "musikcube"
    path.mkdir(parents=True, exist_ok=True)
    return path

CONFIG_DIR = get_config_dir()
TOML_PATH = CONFIG_DIR / "wasapiexclusive_vst.toml"
PRESET_DIR = CONFIG_DIR / "vst_preset"
PRESET_DIR.mkdir(parents=True, exist_ok=True)

def sanitise_preset_name(name: str) -> str:
    # Keep only safe characters for Windows and Linux filesystems
    sanitized = re.sub(r'[\\/*?:"<>|]', '_', name)
    sanitized = re.sub(r'[\x00-\x1f\x7f]', '', sanitized)
    return sanitized.strip(". ")

def scan_vst3s() -> list[str]:
    vst3_root = Path(r"C:\Program Files\Common Files\VST3")
    vsts = []
    if vst3_root.exists():
        for path in vst3_root.rglob("*.vst3"):
            # Check if this path or any parent directory ending in .vst3 is our plugin
            # For directories like 'Serum2.vst3', we prefer the path to the .vst3 directory itself
            parts = path.parts
            vst3_index = next((i for i, part in enumerate(parts) if part.endswith(".vst3")), None)
            if vst3_index is not None:
                vst_path = Path(*parts[:vst3_index + 1])
                vsts.append(str(vst_path))
            else:
                vsts.append(str(path))
    # Deduplicate and sort
    return sorted(list(set(vsts)))

class PropertyEditorModal(ModalScreen[dict]):
    def __init__(self, vst_data: dict, installed_vsts: list[str]):
        super().__init__()
        self.vst_data = vst_data
        self.installed_vsts = installed_vsts

    def compose(self) -> ComposeResult:
        with Vertical(id="modal-dialog"):
            yield Label("EDIT PLUGIN PROPERTIES", id="modal-title")
            
            yield Label("Window Title", classes="editor-label")
            yield Input(value=str(self.vst_data.get("window_title", "")), placeholder="e.g. 01 - Equalizer", id="modal-input-title")
            
            yield Label("VST3 Plugin Path", classes="editor-label")
            yield Input(value=str(self.vst_data.get("path", "")), placeholder="C:\\Program Files\\...\\Plugin.vst3", id="modal-input-path")
            
            yield Label("Select from Installed VST3s", classes="editor-label")
            vst_options = [("Select a VST...", "")] + [(os.path.basename(path), path) for path in self.installed_vsts]
            # Match current path in dropdown
            current_path = self.vst_data.get("path", "")
            default_val = current_path if current_path in self.installed_vsts else ""
            yield Select(vst_options, value=default_val, id="modal-select-vst")
            
            yield Label("Preset Path (.vstpreset)", classes="editor-label")
            yield Input(value=str(self.vst_data.get("preset", "")), placeholder="D:\\presets\\Plugin.vstpreset", id="modal-input-preset")
            
            with Horizontal(classes="checkbox-row"):
                yield Label("Autoload:")
                yield Switch(value=bool(self.vst_data.get("autoload", False)), id="modal-switch-autoload")
            
            with Horizontal(classes="checkbox-row"):
                yield Label("Bypass:")
                yield Switch(value=bool(self.vst_data.get("bypass", False)), id="modal-switch-bypass")
            
            with Horizontal(classes="checkbox-row"):
                yield Label("Show UI:")
                yield Switch(value=bool(self.vst_data.get("show_ui", True)), id="modal-switch-showui")
            
            with Horizontal(classes="modal-buttons"):
                yield Button("Apply Changes", id="btn-apply")
                yield Button("Cancel", id="btn-cancel")

    def on_select_changed(self, event: Select.Changed):
        if event.value != Select.BLANK and event.value != "":
            self.query_one("#modal-input-path", Input).value = event.value

    def on_button_pressed(self, event: Button.Pressed):
        if event.button.id == "btn-apply":
            # Read all fields and build updated dict
            updated = {
                "window_title": self.query_one("#modal-input-title", Input).value.strip(),
                "path": self.query_one("#modal-input-path", Input).value.strip(),
                "preset": self.query_one("#modal-input-preset", Input).value.strip(),
                "autoload": self.query_one("#modal-switch-autoload", Switch).value,
                "bypass": self.query_one("#modal-switch-bypass", Switch).value,
                "show_ui": self.query_one("#modal-switch-showui", Switch).value,
            }
            self.dismiss(updated)
        elif event.button.id == "btn-cancel":
            self.dismiss(None)

class VstManagerApp(App):
    TITLE = "musikcube VST TOML Manager"
    CSS = """
    Screen {
        background: #121214;
    }
    
    #main-container {
        layout: grid;
        grid-size: 2;
        grid-columns: 7.5fr 2.5fr;
        height: 1fr;
    }
    
    .panel {
        background: #1a1a1e;
        border: solid #2e2e33;
        padding: 1;
        margin: 1;
        height: 1fr;
    }
    
    .panel-header {
        width: 100%;
        text-align: center;
        background: #2e2e33;
        color: #f4f4f5;
        text-style: bold;
        padding: 1;
        margin-bottom: 1;
    }
    
    DataTable {
        background: #0e0e10;
        border: solid #27272a;
        height: 1fr;
    }
    
    DataTable > .datatable--header {
        background: #27272a;
        color: #f4f4f5;
        text-style: bold;
    }
    
    DataTable > .datatable--cursor {
        background: #2563eb;
        color: #ffffff;
    }
    
    ListView {
        background: #0e0e10;
        border: solid #27272a;
        height: 1fr;
    }
    
    ListItem {
        padding: 1;
        margin: 0;
        background: #0e0e10;
    }
    
    ListItem:focus {
        background: #2563eb;
        color: #ffffff;
    }

    ListItem.-highlight {
        background: #1e3a8a;
        color: #ffffff;
    }

    ListItem.-highlight:focus {
        background: #2563eb;
        color: #ffffff;
    }
    
    #status-bar {
        background: #1a1a1e;
        color: #a1a1aa;
        padding: 0 1;
        height: 1;
        text-align: center;
        border-top: solid #2e2e33;
    }
    
    #vst-action-row {
        height: 3;
        margin-top: 1;
        width: 100%;
        layout: horizontal;
    }

    #group-edit-actions {
        width: 1fr;
        layout: horizontal;
        align: left middle;
    }

    #group-move-actions {
        width: 1fr;
        layout: horizontal;
        align: right middle;
    }

    #group-edit-actions Button {
        width: 14;
        height: 3;
        margin-right: 1;
    }

    #group-move-actions Button {
        width: 16;
        height: 3;
        margin-left: 1;
    }
    
    #right-panel Button {
        width: 100%;
        margin-top: 1;
    }
    
    #preset-input-container {
        height: auto;
        margin-top: 1;
        border-top: solid #2e2e33;
        padding-top: 1;
    }
    
    #preset-input-container Label {
        text-style: bold;
        color: #a1a1aa;
    }

    /* Modal Styling */
    PropertyEditorModal {
        align: center middle;
        background: rgba(0, 0, 0, 0.7);
    }
    
    #modal-dialog {
        width: 70;
        height: auto;
        background: #1e1e24;
        border: double #3b82f6;
        padding: 1 2;
    }
    
    #modal-title {
        text-align: center;
        text-style: bold;
        color: #60a5fa;
        margin-bottom: 1;
    }
    
    .editor-label {
        text-style: bold;
        color: #a1a1aa;
        margin-top: 1;
        margin-bottom: 0;
    }
    
    .checkbox-row {
        align: left middle;
        height: auto;
        margin-top: 1;
        margin-bottom: 1;
    }
    
    .checkbox-row Label {
        width: 15;
        text-style: bold;
        color: #a1a1aa;
    }
    
    .modal-buttons {
        margin-top: 1;
        height: auto;
    }
    
    .modal-buttons Button {
        width: 1fr;
        margin: 0 1;
    }
    
    #btn-apply {
        background: #059669;
        color: white;
        text-style: bold;
    }
    
    #btn-apply:hover {
        background: #047857;
    }
    
    #btn-cancel {
        background: #ef4444;
        color: white;
    }
    
    #btn-cancel:hover {
        background: #dc2626;
    }

    Toast {
        width: auto;
        max-width: 40;
    }
    """

    BINDINGS = [
        Binding("ctrl+s", "save_toml", "Save TOML"),
        Binding("a", "add_vst", "Add VST"),
        Binding("d", "delete_vst", "Delete VST"),
        Binding("u", "move_up", "Move Up"),
        Binding("n", "move_down", "Move Down"),
        Binding("e", "edit_vst", "Edit"),
        Binding("q", "quit", "Quit"),
    ]

    def __init__(self):
        super().__init__()
        self.doc = tomlkit.document()
        self.chain = tomlkit.aot()
        self.installed_vsts = scan_vst3s()
        self.table_columns = ["bypass", "show_ui", "autoload", "window_title", "path", "preset"]
        self.loaded_preset_name = "None"
        self.is_modified = False
        self.load_toml_file()

    def load_toml_file(self):
        if not TOML_PATH.exists():
            self.doc = tomlkit.document()
            self.doc["chain"] = tomlkit.aot()
            self.chain = self.doc["chain"]
            self.save_toml_data(silent=True)
        else:
            try:
                with open(TOML_PATH, "r", encoding="utf-8") as f:
                    self.doc = tomlkit.parse(f.read())
                if "chain" not in self.doc:
                    self.doc["chain"] = tomlkit.aot()
                self.chain = self.doc["chain"]
            except Exception as e:
                self.doc = tomlkit.document()
                self.doc["chain"] = tomlkit.aot()
                self.chain = self.doc["chain"]
        self.check_preset_match()

    def check_preset_match(self):
        self.loaded_preset_name = "None"
        self.is_modified = False
        
        # Build comparative structure
        current_chain = [dict(item) for item in self.chain]
        
        for preset_file in PRESET_DIR.glob("*.toml"):
            try:
                with open(preset_file, "r", encoding="utf-8") as f:
                    p_doc = tomlkit.parse(f.read())
                p_chain = p_doc.get("chain", [])
                p_chain_dict = [dict(item) for item in p_chain]
                if current_chain == p_chain_dict:
                    self.loaded_preset_name = preset_file.stem
                    return
            except Exception:
                pass

    def save_toml_data(self, silent=False):
        try:
            with open(TOML_PATH, "w", encoding="utf-8") as f:
                f.write(tomlkit.dumps(self.doc))
            self.check_preset_match()
            self.update_status_bar()
            if not silent:
                self.notify(f"Successfully saved to {TOML_PATH.name}!", severity="information")
        except Exception as e:
            if not silent:
                self.notify(f"Failed to save: {e}", severity="error")

    def compose(self) -> ComposeResult:
        yield Header()
        with Container(id="main-container"):
            # Left Panel: DataTable and buttons
            with Vertical(classes="panel", id="left-panel"):
                yield Label("VST PLUGIN CHAIN", classes="panel-header")
                yield DataTable()
                with Horizontal(id="vst-action-row"):
                    with Horizontal(id="group-edit-actions"):
                        yield Button("Add (A)", id="btn-add-vst")
                        yield Button("Edit (E)", id="btn-edit-vst")
                        yield Button("Delete (D)", id="btn-delete-vst")
                    with Horizontal(id="group-move-actions"):
                        yield Button("Move Up (U)", id="btn-up")
                        yield Button("Move Down (N)", id="btn-down")
            
            # Right Panel: Presets Management
            with Vertical(classes="panel", id="right-panel"):
                yield Label("CHAIN PRESETS", classes="panel-header")
                yield ListView(id="list-presets")
                yield Button("Load Selected Preset", id="btn-load-preset")
                with Vertical(id="preset-input-container"):
                    yield Label("New Preset Name")
                    yield Input(placeholder="e.g. Low End Focus", id="input-preset-name")
                    yield Button("Save Current as Preset", id="btn-save-preset")
                yield Button("Delete Selected Preset", id="btn-delete-preset")

        yield Label(f"Config path: {TOML_PATH}", id="status-bar")
        yield Footer()

    def on_mount(self) -> None:
        dt = self.query_one(DataTable)
        dt.cursor_type = "cell"
        dt.add_column("Bypass", key="bypass")
        dt.add_column("Show UI", key="show_ui")
        dt.add_column("Load config", key="autoload")
        dt.add_column("Window name", key="window_title")
        dt.add_column("VST3 Path", key="path")
        dt.add_column("Config path", key="preset")
        
        self.repopulate_table()
        self.rebuild_preset_list()
        self.update_status_bar()

    def repopulate_table(self):
        dt = self.query_one(DataTable)
        cursor_row = dt.cursor_row
        cursor_col = dt.cursor_column
        
        dt.clear(columns=False)
        for index, item in enumerate(self.chain):
            # Center boolean indicators in the columns
            bypass = "  X  " if item.get("bypass", False) else "     "
            show_ui = "   X   " if item.get("show_ui", True) else "       "
            autoload = "     X     " if item.get("autoload", False) else "           "
            
            window_title = item.get("window_title", "") or "(unnamed)"
            path = item.get("path", "")
            preset = item.get("preset", "")
            
            dt.add_row(
                bypass,
                show_ui,
                autoload,
                window_title,
                path,
                preset,
                key=str(index)
            )
            
        if cursor_row is not None and len(self.chain) > 0:
            row = min(cursor_row, len(self.chain) - 1)
            col = cursor_col if cursor_col is not None else 0
            dt.cursor_coordinate = (row, col)

    def rebuild_preset_list(self, select_name: str = ""):
        list_view = self.query_one("#list-presets", ListView)
        list_view.clear()
        
        presets = list(PRESET_DIR.glob("*.toml"))
        presets.sort()
        
        select_index = 0
        for i, preset in enumerate(presets):
            name = preset.stem
            list_view.append(ListItem(Label(f" {name}")))
            if name == select_name:
                select_index = i
                
        if len(list_view.children) > 0:
            list_view.index = select_index

    def update_status_bar(self):
        status_bar = self.query_one("#status-bar", Label)
        preset_str = self.loaded_preset_name
        if self.is_modified:
            preset_str += " [Modified]*"
        status_bar.update(f"Config path: {TOML_PATH}  |  Active Preset: {preset_str}")

    # Keyboard / Event Handling on DataTable
    def key_space(self) -> None:
        dt = self.query_one(DataTable)
        coord = dt.cursor_coordinate
        if coord is not None:
            self.toggle_cell(coord.row, coord.column)

    def on_data_table_cell_submitted(self, event: DataTable.CellSubmitted):
        col_idx = event.coordinate.column
        row_idx = event.coordinate.row
        col_key = self.table_columns[col_idx]
        
        if col_key in ("bypass", "show_ui", "autoload"):
            self.toggle_cell(row_idx, col_idx)
        else:
            self.action_edit_vst()

    def toggle_cell(self, row: int, col: int):
        col_key = self.table_columns[col]
        if col_key in ("bypass", "show_ui", "autoload"):
            vst = self.chain[row]
            current_val = vst.get(col_key, False)
            new_val = not current_val
            vst[col_key] = new_val
            
            # Formatted cell content for alignment centering
            if col_key == "bypass":
                cell_val = "  X  " if new_val else "     "
            elif col_key == "show_ui":
                cell_val = "   X   " if new_val else "       "
            else: # autoload
                cell_val = "     X     " if new_val else "           "
                
            dt = self.query_one(DataTable)
            dt.update_cell_at((row, col), cell_val)
            self.is_modified = True
            self.save_toml_data(silent=True)
            self.notify(f"Toggled {col_key} for {vst.get('window_title') or 'plugin'}", severity="information")

    def action_edit_vst(self):
        dt = self.query_one(DataTable)
        row_idx = dt.cursor_row
        if row_idx is None or row_idx < 0 or row_idx >= len(self.chain):
            self.notify("No plugin selected to edit.", severity="error")
            return
            
        vst_data = self.chain[row_idx]
        
        def on_modal_close(updated_data: dict | None):
            if updated_data is not None:
                # Update data
                self.chain[row_idx].update(updated_data)
                self.repopulate_table()
                self.is_modified = True
                self.save_toml_data(silent=True)
                self.notify("Updated plugin properties.", severity="information")
                
        self.push_screen(PropertyEditorModal(vst_data, self.installed_vsts), on_modal_close)

    # Actions and Button Handlers
    def action_save_toml(self):
        self.save_toml_data()

    def action_add_vst(self):
        new_vst = tomlkit.table()
        new_vst["autoload"] = False
        new_vst["bypass"] = False
        new_vst["path"] = ""
        new_vst["preset"] = ""
        new_vst["show_ui"] = True
        new_vst["window_title"] = ""
        
        self.chain.append(new_vst)
        self.repopulate_table()
        self.is_modified = True
        self.update_status_bar()
        
        # Focus on newly created entry
        dt = self.query_one(DataTable)
        dt.cursor_coordinate = (len(self.chain) - 1, 3) # focus on Window name column
        self.action_edit_vst()

    def action_delete_vst(self):
        dt = self.query_one(DataTable)
        idx = dt.cursor_row
        if idx is not None and 0 <= idx < len(self.chain):
            self.chain.pop(idx)
            self.repopulate_table()
            self.is_modified = True
            self.save_toml_data(silent=True)
            self.notify("Deleted plugin configuration from chain.", severity="warning")
        else:
            self.notify("No plugin selected to delete.", severity="error")

    def action_move_up(self):
        dt = self.query_one(DataTable)
        idx = dt.cursor_row
        if idx is not None and idx > 0:
            item = self.chain.pop(idx)
            self.chain.insert(idx - 1, item)
            self.repopulate_table()
            self.is_modified = True
            self.save_toml_data(silent=True)
            dt.cursor_coordinate = (idx - 1, dt.cursor_column or 0)

    def action_move_down(self):
        dt = self.query_one(DataTable)
        idx = dt.cursor_row
        if idx is not None and idx < len(self.chain) - 1:
            item = self.chain.pop(idx)
            self.chain.insert(idx + 1, item)
            self.repopulate_table()
            self.is_modified = True
            self.save_toml_data(silent=True)
            dt.cursor_coordinate = (idx + 1, dt.cursor_column or 0)

    def on_button_pressed(self, event: Button.Pressed):
        if event.button.id == "btn-save-preset":
            self.save_preset()
        elif event.button.id == "btn-delete-preset":
            self.delete_preset()
        elif event.button.id == "btn-add-vst":
            self.action_add_vst()
        elif event.button.id == "btn-edit-vst":
            self.action_edit_vst()
        elif event.button.id == "btn-delete-vst":
            self.action_delete_vst()
        elif event.button.id == "btn-up":
            self.action_move_up()
        elif event.button.id == "btn-down":
            self.action_move_down()
        elif event.button.id == "btn-load-preset":
            self.load_selected_preset()

    def save_preset(self):
        preset_name_input = self.query_one("#input-preset-name", Input)
        name = preset_name_input.value.strip()
        if not name:
            self.notify("Please enter a valid preset name.", severity="error")
            return
            
        sanitised_name = sanitise_preset_name(name)
        preset_file = PRESET_DIR / f"{sanitised_name}.toml"
        
        try:
            with open(preset_file, "w", encoding="utf-8") as f:
                f.write(tomlkit.dumps(self.doc))
            preset_name_input.value = ""
            self.rebuild_preset_list(select_name=sanitised_name)
            self.check_preset_match()
            self.update_status_bar()
            self.notify(f"Saved chain configuration preset: {sanitised_name}", severity="information")
        except Exception as e:
            self.notify(f"Failed to save preset: {e}", severity="error")

    def delete_preset(self):
        list_view = self.query_one("#list-presets", ListView)
        idx = list_view.index
        if idx is None or idx < 0:
            self.notify("No preset selected to delete.", severity="error")
            return
            
        presets = list(PRESET_DIR.glob("*.toml"))
        presets.sort()
        if idx < len(presets):
            preset_file = presets[idx]
            try:
                preset_file.unlink()
                self.rebuild_preset_list()
                self.check_preset_match()
                self.update_status_bar()
                self.notify(f"Deleted preset {preset_file.stem}", severity="warning")
            except Exception as e:
                self.notify(f"Failed to delete preset: {e}", severity="error")

    def on_list_view_child_double_clicked(self, event: ListView.ChildDoubleClicked):
        if event.list_view.id == "list-presets":
            self.load_selected_preset()

    def load_selected_preset(self):
        list_view = self.query_one("#list-presets", ListView)
        idx = list_view.index
        if idx is None or idx < 0:
            return
            
        presets = list(PRESET_DIR.glob("*.toml"))
        presets.sort()
        if idx < len(presets):
            preset_file = presets[idx]
            try:
                with open(preset_file, "r", encoding="utf-8") as f:
                    preset_doc = tomlkit.parse(f.read())
                
                with open(TOML_PATH, "w", encoding="utf-8") as f:
                    f.write(tomlkit.dumps(preset_doc))
                
                self.load_toml_file()
                self.repopulate_table()
                self.update_status_bar()
                self.notify(f"Loaded preset chain: {preset_file.stem}", severity="information")
            except Exception as e:
                self.notify(f"Failed to load preset: {e}", severity="error")

def main():
    app = VstManagerApp()
    app.run()

if __name__ == "__main__":
    main()
