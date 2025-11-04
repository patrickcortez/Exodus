import tkinter as tk
from tkinter import ttk, font, messagebox
from tkinter.scrolledtext import ScrolledText
import os
import json
from tkinter import simpledialog
import shutil

# --- CONFIGURATION ---
# Tell the script where to find your nodewatch.json file
# It's assumed to be in the same directory as this Python script.
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
NODEWATCH_JSON_PATH = os.path.join(SCRIPT_DIR, "nodewatch.json")
# --- NEW: Added user.json path ---
USER_JSON_PATH = os.path.join(SCRIPT_DIR, "user.json")
# --- END CONFIGURATION ---

# --- NEW: Line Numbered Text Editor Widget ---
class TextEditorWithLines(tk.Frame):
    """
    A custom Tkinter widget that combines a ScrolledText editor
    with a synchronized line number bar.
    """
    def __init__(self, master, *args, **kwargs):
        tk.Frame.__init__(self, master)
        
        # Get font from kwargs, or use default
        editor_font = kwargs.pop('font', font.Font(family="Monospace", size=10))

        # 1. Line Number Bar (a Text widget)
        self.line_numbers = tk.Text(self, width=4, padx=4, takefocus=0,
                                    bd=0, background="#f0f0f0", 
                                    foreground="gray", state='disabled',
                                    font=editor_font)
        self.line_numbers.pack(side=tk.LEFT, fill=tk.Y)

        # 2. Main Text Editor (a ScrolledText widget)
        self.text_widget = ScrolledText(self, wrap=tk.WORD, undo=True,
                                        font=editor_font, *args, **kwargs)
        self.text_widget.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True)

        # 3. Link scrolling
        # Get the ScrolledText's internal scrollbar
        # --- MODIFIED: Store vbar and handle its absence ---
        self.original_vbar = None
        try:
            self.original_vbar = self.text_widget.vbar
        except AttributeError:
             # Fallback for different ScrolledText implementations
             # This part is tricky, but ScrolledText usually has 'vbar'
            pass
        # --- END MODIFIED ---

        # Set the editor's yscrollcommand to our new proxy function
        self.text_widget.config(yscrollcommand=self.on_text_scroll)
        
        # Set the scrollbar's command to scroll *both* widgets
        # --- MODIFIED: Check if vbar exists ---
        if self.original_vbar:
            self.original_vbar.config(command=self.on_vbar_scroll)
            # Store the original scrollbar's 'set' method
            self.original_scrollbar_set = self.original_vbar.set
        else:
            # If we couldn't find vbar, scroll-syncing might be limited
            self.original_scrollbar_set = lambda *args: None
        # --- END MODIFIED ---

        # 4. Bind events to update line numbers
        # <<Modified>> is triggered by any change
        self.text_widget.bind("<<Modified>>", self.on_text_change)
        # Need to also catch things that move the view without modifying text
        self.text_widget.bind("<Configure>", self.on_text_change) # On resize
        
        self._update_in_progress = False
        self.update_line_numbers()

    # --- NEW: Theme control method ---
    def set_theme(self, bg, fg, line_num_bg, line_num_fg, scroll_bg, scroll_trough):
        """Applies theme colors to all child widgets."""
        self.config(bg=bg) # Main frame
        self.line_numbers.config(bg=line_num_bg, fg=line_num_fg)
        self.text_widget.config(bg=bg, fg=fg, insertbackground=fg) # insertbackground is the cursor
        
        # Style the tk.Scrollbar
        if self.original_vbar:
            self.original_vbar.config(bg=scroll_bg, troughcolor=scroll_trough,
                                      activebackground=scroll_bg)
    # --- END NEW ---

    def on_vbar_scroll(self, *args):
        """Called when the user drags the scrollbar."""
        self.text_widget.yview(*args)
        self.line_numbers.yview(*args)
        self.update_line_numbers()

    def on_text_scroll(self, *args):
        """Called when the text widget is scrolled (e.g., mousewheel)."""
        # This syncs the scrollbar *to* the text widget
        self.original_scrollbar_set(*args)
        # And syncs the line numbers *to* the text widget
        try:
            self.line_numbers.yview_moveto(args[0])
        except IndexError:
            pass # Happens on initial load sometimes
        self.update_line_numbers()

    def on_text_change(self, event=None):
        """Called when text is modified or view changes."""
        if self.text_widget.edit_modified():
            self.text_widget.edit_modified(False) # Reset flag
        self.update_line_numbers()

    def update_line_numbers(self):
        """Redraws the line numbers in the bar."""
        if self._update_in_progress:
            return
        try:
            self._update_in_progress = True
            
            # Get total number of lines
            num_lines_str = self.text_widget.index('end-1c').split('.')[0]
            num_lines = int(num_lines_str) if num_lines_str else 1

            # Get current line numbers
            current_lines_str = self.line_numbers.get("1.0", tk.END)
            num_current = len(current_lines_str.split("\n")) - 1

            if num_lines != num_current:
                # Generate new line number string
                line_str = "\n".join(str(i) for i in range(1, num_lines + 1))
                
                # Update the text
                self.line_numbers.config(state='normal')
                self.line_numbers.delete("1.0", tk.END)
                self.line_numbers.insert("1.0", line_str)
                self.line_numbers.config(state='disabled')
            
            # Adjust width of line number bar if needed
            new_width = len(str(num_lines)) + 1 # +1 for padding
            if self.line_numbers.cget("width") != new_width:
                 self.line_numbers.config(width=new_width)
                 
            # Sync scroll position
            self.line_numbers.yview_moveto(self.text_widget.yview()[0])
            
        finally:
            self._update_in_progress = False
# --- END NEW WIDGET ---


class ExodusGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("Exodus Node Editor") # <-- Window title is set here
        self.root.geometry("1024x768")

        # Store node data like: {"NodeName": "/path/to/node", ...}
        self.nodes_data = {}
        self.current_file_path = None
        self.clipboard_path = None
        
        # --- NEW: Theme setup ---
        self.style = ttk.Style(self.root)
        self.current_theme = "light" # Default, will be overwritten by load_theme
        self.theme_var = tk.StringVar(value=self.current_theme)
        # --- END NEW ---
        
        # --- 1. Create Main Layout ---
        self.create_menu()
        
        # Create a horizontal PanedWindow (resizable left/right)
        self.main_paned_window = ttk.PanedWindow(root, orient=tk.HORIZONTAL)
        self.main_paned_window.pack(fill=tk.BOTH, expand=True)

        # --- 2. Left Pane (Node + File Explorer) ---
        self.left_pane = ttk.Frame(self.main_paned_window, width=300)
        self.left_pane.pack(fill=tk.Y, side=tk.LEFT)
        self.main_paned_window.add(self.left_pane, weight=1)

        # Node Selector
        ttk.Label(self.left_pane, text="Select Node:").pack(padx=5, pady=5, anchor='w')
        self.node_combobox = ttk.Combobox(self.left_pane, state="readonly")
        self.node_combobox.pack(fill=tk.X, padx=5, pady=2)
        self.node_combobox.bind("<<ComboboxSelected>>", self.on_node_selected)

        # File Tree
        self.tree = ttk.Treeview(self.left_pane, selectmode="browse")
        self.tree.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        self.tree.heading("#0", text="Node Explorer", anchor='w')
        
        # Bind events
        self.tree.bind("<Button-1>", self.on_tree_left_click) 
        self.tree.bind("<<TreeviewOpen>>", self.on_tree_open)
        self.tree.bind("<Double-1>", self.on_tree_double_click)
        self.tree.bind("<Button-3>", self.show_context_menu)

        # --- 3. Right Pane (Text Editor) ---
        self.right_pane = ttk.Frame(self.main_paned_window)
        self.right_pane.pack(fill=tk.BOTH, expand=True)
        self.main_paned_window.add(self.right_pane, weight=4)

        # Use a good monospace font
        editor_font = font.Font(family="Monospace", size=10)
        
        # --- MODIFIED: Create Text Editor with Line Numbers ---
        self.editor_frame = TextEditorWithLines(self.right_pane, font=editor_font)
        self.editor_frame.pack(fill=tk.BOTH, expand=True)
        self.editor = self.editor_frame.text_widget # Get the actual ScrolledText widget
        # --- END MODIFIED ---

        self.editor.bind("<Button-1>", self.on_editor_left_click)

        # --- 4. Create Context Menu (Right-click) ---
        self.create_context_menu()

        # --- 5. Load Initial Data & Theme ---
        self.load_theme() # Load theme before loading node data
        self.load_nodes_from_config()

    def create_menu(self):
        """Creates the top menu bar (File, Edit, etc.)"""
        self.menu_bar = tk.Menu(self.root)
        self.root.config(menu=self.menu_bar)

        # File Menu
        # --- MODIFIED: Store menus as self. variables ---
        self.file_menu = tk.Menu(self.menu_bar, tearoff=0)
        self.menu_bar.add_cascade(label="File", menu=self.file_menu)
        self.file_menu.add_command(label="Save", command=self.save_file, accelerator="Ctrl+S")
        
        # --- NEW: Theme Submenu ---
        self.file_menu.add_separator()
        self.theme_menu = tk.Menu(self.file_menu, tearoff=0)
        self.file_menu.add_cascade(label="Theme", menu=self.theme_menu)
        self.theme_menu.add_radiobutton(label="Light", variable=self.theme_var, 
                                        value="light", command=self.set_light_mode)
        self.theme_menu.add_radiobutton(label="Dark", variable=self.theme_var, 
                                        value="dark", command=self.set_dark_mode)
        # --- END NEW ---
        
        self.file_menu.add_separator()
        self.file_menu.add_command(label="Exit", command=self.root.quit)
        # --- END MODIFIED ---
        
        # Bind Ctrl+S
        self.root.bind_all("<Control-s>", self.save_file)
    
    # --- NEW: Theme-handling functions ---
    
    def load_theme(self):
        """Loads the saved theme from user.json, or defaults to light."""
        theme = "light" # Default
        try:
            with open(USER_JSON_PATH, 'r') as f:
                config = json.load(f)
                theme = config.get("theme", "light")
        except Exception:
            pass # Use default theme if file not found or corrupt

        if theme == "dark":
            self.set_dark_mode()
        else:
            self.set_light_mode()

    def save_theme(self, theme_name):
        """Saves the current theme preference to user.json."""
        try:
            with open(USER_JSON_PATH, 'w') as f:
                json.dump({"theme": theme_name}, f, indent=4)
        except Exception as e:
            messagebox.showerror("Error", f"Could not save theme settings to user.json:\n{e}")

    # --- MODIFIED: Greatly expanded theme functions ---
    def set_light_mode(self):
        """Applies the light theme colors."""
        if self.current_theme == "light" and self.style.theme_use() == 'default':
            return # No change
        
        self.current_theme = "light"
        self.theme_var.set("light")
        
        self.style.theme_use('default') 
        
        # --- Define light colors ---
        bg = "#FFFFFF"
        fg = "#000000"
        tree_bg = "#FFFFFF"
        widget_bg = "#FFFFFF"
        line_num_bg = "#f0f0f0"
        line_num_fg = "gray"
        selected_bg = "#0078d7" 
        selected_fg = "#FFFFFF"
        scroll_bg = "#e0e0e0"
        scroll_trough = "#ffffff"
        menu_bg = "#f0f0f0"
        menu_fg = "#000000"

        self.root.config(bg=bg)
        
        # --- Configure ttk styles ---
        self.style.configure('.', background=bg, foreground=fg)
        self.style.configure('TFrame', background=bg)
        self.style.configure('TLabel', background=bg, foreground=fg)

        # --- NEW: More aggressive Combobox styling ---
        self.style.configure('TCombobox', 
            background=widget_bg, 
            foreground=fg, 
            fieldbackground=widget_bg, 
            insertcolor=fg,
            arrowcolor=fg
        )
        self.style.map('TCombobox', 
            fieldbackground=[('readonly', widget_bg)], 
            foreground=[('readonly', fg)],
            selectbackground=[('readonly', widget_bg)],
            selectforeground=[('readonly', fg)]
        )
        # Style the dropdown listbox
        self.root.option_add("*TCombobox*Listbox.background", widget_bg)
        self.root.option_add("*TCombobox*Listbox.foreground", fg)
        self.root.option_add("*TCombobox*Listbox.selectBackground", selected_bg)
        self.root.option_add("*TCombobox*Listbox.selectForeground", selected_fg)
        
        self.style.configure('TSpinbox', 
            background=widget_bg, 
            foreground=fg, 
            fieldbackground=widget_bg,
            insertcolor=fg,
            arrowcolor=fg
        )
        self.style.map('TSpinbox', 
            fieldbackground=[('readonly', widget_bg)], 
            foreground=[('readonly', fg)]
        )
        # --- END NEW ---
        
        self.style.configure('Treeview', background=tree_bg, foreground=fg, fieldbackground=tree_bg)
        self.style.map('Treeview', background=[('selected', selected_bg)], foreground=[('selected', selected_fg)])
        
        self.style.configure('TPanedWindow', background=bg)
        # self.main_paned_window.config(bg=bg) # <-- THIS LINE WAS REMOVED
        self.left_pane.config(style='TFrame')
        self.right_pane.config(style='TFrame')

        # --- Configure raw tk widgets ---
        
        # Call the editor's theme method
        self.editor_frame.set_theme(bg, fg, line_num_bg, line_num_fg, scroll_bg, scroll_trough)
        
        # Configure all menus
        menu_config = {
            "bg": menu_bg, "fg": menu_fg,
            "activebackground": selected_bg, "activeforeground": selected_fg,
            "bd": 0 # Remove border
        }
        self.menu_bar.config(menu_config)
        self.file_menu.config(menu_config)
        self.theme_menu.config(menu_config)
        self.context_menu.config(menu_config)
        
        self.save_theme("light")

    def set_dark_mode(self):
        """Applies the dark theme colors."""
        if self.current_theme == "dark" and self.style.theme_use() == 'clam':
            return # No change
            
        self.current_theme = "dark"
        self.theme_var.set("dark")
        
        self.style.theme_use('clam') 

        # --- Define dark colors ---
        bg = "#2D2D2D"
        fg = "#DDDDDD"
        tree_bg = "#333333"
        widget_bg = "#3c3c3c"
        selected_bg = "#004a99"
        selected_fg = "#DDDDDD"
        line_num_bg = "#3a3a3a"
        line_num_fg = "#888888"
        scroll_bg = "#555555"
        scroll_trough = "#3c3c3c"
        menu_bg = "#2D2D2D"
        menu_fg = "#DDDDDD"

        self.root.config(bg=bg)
        
        # --- Configure ttk styles ---
        self.style.configure('.', background=bg, foreground=fg)
        self.style.configure('TFrame', background=bg)
        self.style.configure('TLabel', background=bg, foreground=fg)
        
        # --- NEW: More aggressive Combobox styling ---
        # Configure the 'TCombobox' style
        self.style.configure('TCombobox', 
            background=widget_bg, 
            foreground=fg, 
            fieldbackground=widget_bg, 
            insertcolor=fg, # Cursor
            arrowcolor=fg
        )
        # Map the 'readonly' state
        self.style.map('TCombobox', 
            fieldbackground=[('readonly', widget_bg)], 
            foreground=[('readonly', fg)],
            selectbackground=[('readonly', widget_bg)],
            selectforeground=[('readonly', fg)]
        )
        # Style the dropdown listbox
        self.root.option_add("*TCombobox*Listbox.background", widget_bg)
        self.root.option_add("*TCombobox*Listbox.foreground", fg)
        self.root.option_add("*TCombobox*Listbox.selectBackground", selected_bg)
        self.root.option_add("*TCombobox*Listbox.selectForeground", selected_fg)
        
        # Just in case, style TSpinbox which it might inherit
        self.style.configure('TSpinbox', 
            background=widget_bg, 
            foreground=fg, 
            fieldbackground=widget_bg,
            insertcolor=fg,
            arrowcolor=fg
        )
        self.style.map('TSpinbox', 
            fieldbackground=[('readonly', widget_bg)], 
            foreground=[('readonly', fg)]
        )
        # --- END NEW ---
        
        self.style.configure('Treeview', background=tree_bg, foreground=fg, fieldbackground=tree_bg)
        self.style.map('Treeview', background=[('selected', selected_bg)], foreground=[('selected', selected_fg)])
        
        self.style.configure('TPanedWindow', background=bg)
        # self.main_paned_window.config(bg=bg) # <-- THIS LINE WAS REMOVED
        self.left_pane.config(style='TFrame')
        self.right_pane.config(style='TFrame')
        
        # --- Configure raw tk widgets ---
        self.editor_frame.set_theme(bg, fg, line_num_bg, line_num_fg, scroll_bg, scroll_trough)
        
        # Configure all menus
        menu_config = {
            "bg": menu_bg, "fg": menu_fg,
            "activebackground": selected_bg, "activeforeground": selected_fg,
            "bd": 0 # Remove border
        }
        self.menu_bar.config(menu_config)
        self.file_menu.config(menu_config)
        self.theme_menu.config(menu_config)
        self.context_menu.config(menu_config)

        self.save_theme("dark")
    # --- END MODIFIED ---

    # --- END NEW Theme-handling ---

    def on_tree_left_click(self, event):
        """Dismisses context menu on left click in the tree."""
        try:
            self.context_menu.unpost()
        except tk.TclError:
            # Menu was not posted, which is fine
            pass
            
    # --- MODIFIED: Store context_menu as self. variable ---
    def create_context_menu(self):
        """Creates the right-click context menu."""
        self.context_menu = tk.Menu(self.tree, tearoff=0)
        self.context_menu.add_command(label="Create File", command=self.create_new_file)
        self.context_menu.add_command(label="Create Folder", command=self.create_new_folder)
        self.context_menu.add_separator()
        self.context_menu.add_command(label="Copy", command=self.copy_item)
        self.context_menu.add_command(label="Paste", command=self.paste_item)
        self.context_menu.add_separator()
        self.context_menu.add_command(label="Rename", command=self.rename_item)
        self.context_menu.add_command(label="Delete", command=self.delete_item)
    # --- END MODIFIED ---

    def on_editor_left_click(self, event):
        """Dismisses context menu on left click in the editor."""
        try:
            self.context_menu.unpost()
        except tk.TclError:
            # Menu was not posted, which is fine
            pass

    def show_context_menu(self, event):
        """Shows the context menu on right-click."""
        # Select item under cursor
        item_id = self.tree.identify_row(event.y)
        if item_id:
            self.tree.focus(item_id)
            self.tree.selection_set(item_id)
            
            # Get path and type
            path, item_type = self.get_selected_item_path()
            
            if path:
                # Enable/disable items based on context
                self.context_menu.entryconfig("Create File", state="normal" if item_type == "dir" else "disabled")
                self.context_menu.entryconfig("Create Folder", state="normal" if item_type == "dir" else "disabled")
                self.context_menu.entryconfig("Copy", state="normal")
                self.context_menu.entryconfig("Rename", state="normal") 
                self.context_menu.entryconfig("Delete", state="normal")
                
                can_paste = self.clipboard_path and item_type == "dir"
                self.context_menu.entryconfig("Paste", state="normal" if can_paste else "disabled")
            
                # Show the menu
                self.context_menu.post(event.x_root, event.y_root)

    def get_selected_item_path(self):
        """Helper to get the path and type ('dir' or 'file') of the selected item."""
        item_id = self.tree.focus()
        if not item_id:
            return None, None
        
        try:
            values = self.tree.item(item_id, "values")
            if not values:
                return None, None
            return values[0], values[1] # (path, type)
        except (IndexError, tk.TclError):
            # This can happen if the item is invalid (e.g., [Permission Denied])
            return None, None

    def refresh_parent_tree(self, item_id):
        """Refreshes the parent of a given item to show changes."""
        parent_id = self.tree.parent(item_id)
        if not parent_id: # It's a root node
            self.on_node_selected() # Just reload the whole node
            return

        try:
            parent_path = self.tree.item(parent_id, "values")[0]
            self.populate_tree(parent_id, parent_path)
        except (IndexError, tk.TclError):
             # Parent was likely deleted or invalid, reload whole node
             self.on_node_selected()

    def create_new_file(self):
        """Creates a new empty file in the selected directory."""
        path, item_type = self.get_selected_item_path()
        if item_type != "dir":
            return
            
        filename = simpledialog.askstring("Create File", "Enter new filename:", parent=self.root)
        if filename:
            try:
                new_path = os.path.join(path, filename)
                if os.path.exists(new_path):
                    messagebox.showerror("Error", f"A file or folder named '{filename}' already exists.")
                    return
                open(new_path, 'a').close() # Create empty file
                self.populate_tree(self.tree.focus(), path) # Refresh folder
            except Exception as e:
                messagebox.showerror("Error", f"Could not create file:\n{e}")

    def create_new_folder(self):
        """Creates a new empty folder in the selected directory."""
        path, item_type = self.get_selected_item_path()
        if item_type != "dir":
            return
            
        foldername = simpledialog.askstring("Create Folder", "Enter new folder name:", parent=self.root)
        if foldername:
            try:
                new_path = os.path.join(path, foldername)
                if os.path.exists(new_path):
                    messagebox.showerror("Error", f"A file or folder named '{foldername}' already exists.")
                    return
                os.makedirs(new_path)
                self.populate_tree(self.tree.focus(), path) # Refresh folder
            except Exception as e:
                messagebox.showerror("Error", f"Could not create folder:\n{e}")

    def copy_item(self):
        """Copies the selected item's path to the internal clipboard."""
        path, item_type = self.get_selected_item_path()
        if path:
            self.clipboard_path = path
            print(f"Copied to clipboard: {path}")

    def paste_item(self):
        """Pastes the clipboard item into the selected directory."""
        dest_path, dest_type = self.get_selected_item_path()
        if not self.clipboard_path or dest_type != "dir":
            return
            
        source_path = self.clipboard_path
        
        try:
            dest_name = os.path.basename(source_path)
            final_dest = os.path.join(dest_path, dest_name)
            
            if os.path.exists(final_dest):
                if not messagebox.askyesno("Overwrite", f"'{dest_name}' already exists. Overwrite?"):
                    return
                # Delete existing item before pasting
                if os.path.isdir(final_dest):
                    shutil.rmtree(final_dest)
                else:
                    os.remove(final_dest)

            if os.path.isdir(source_path):
                shutil.copytree(source_path, final_dest)
            else:
                shutil.copy2(source_path, final_dest)
                
            self.populate_tree(self.tree.focus(), dest_path)
            
        except Exception as e:
            messagebox.showerror("Error", f"Could not paste item:\n{e}")

    def rename_item(self):
        """Renames the selected file or folder."""
        path, item_type = self.get_selected_item_path()
        if not path:
            return

        item_id = self.tree.focus()
        old_name = os.path.basename(path)
        dir_name = os.path.dirname(path)
        
        new_name = simpledialog.askstring("Rename", 
                                         f"Enter new name for '{old_name}':",
                                         initialvalue=old_name,
                                         parent=self.root)
        
        if new_name and new_name != old_name:
            try:
                new_path = os.path.join(dir_name, new_name)
                
                if os.path.exists(new_path):
                    messagebox.showerror("Error", f"A file or folder named '{new_name}' already exists.")
                    return
                    
                os.rename(path, new_path)
                
                # Refresh the parent tree
                self.refresh_parent_tree(item_id)
                
            except Exception as e:
                messagebox.showerror("Error", f"Could not rename item:\n{e}")

    def delete_item(self):
        """Deletes the selected file or folder after confirmation."""
        path, item_type = self.get_selected_item_path()
        if not path:
            return

        item_name = os.path.basename(path)
        if not messagebox.askyesno("Delete", f"Are you sure you want to permanently delete '{item_name}'?"):
            return
            
        try:
            item_id = self.tree.focus()
            
            if item_type == "dir":
                shutil.rmtree(path)
            else:
                os.remove(path)
                
            # If we deleted the file we are editing, clear the editor
            if path == self.current_file_path:
                self.editor.delete("1.0", tk.END)
                self.current_file_path = None
                self.root.title("Exodus Node Editor")
                self.editor.edit_modified(False)
                self.editor_frame.on_text_change()


            # Refresh the parent to show the deletion
            self.refresh_parent_tree(item_id)
            
        except Exception as e:
            messagebox.showerror("Error", f"Could not delete item:\n{e}")

    def load_nodes_from_config(self):
        """Reads nodewatch.json and populates the node dropdown."""
        try:
            with open(NODEWATCH_JSON_PATH, 'r') as f:
                config = json.load(f)
            
            node_names = []
            for node_name, details in config.items():
                if "path" in details:
                    self.nodes_data[node_name] = details["path"]
                    node_names.append(node_name)
            
            self.node_combobox['values'] = sorted(node_names)
            if node_names:
                self.node_combobox.current(0)
                self.on_node_selected() # Auto-load first node
                
        except FileNotFoundError:
            messagebox.showerror("Error", f"Config file not found: {NODEWATCH_JSON_PATH}")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to load config: {e}")

    def on_node_selected(self, event=None):
        """Called when a new node is chosen from the dropdown."""
        node_name = self.node_combobox.get()
        node_path = self.nodes_data.get(node_name)
        
        if not node_path or not os.path.isdir(node_path):
            messagebox.showwarning("Warning", f"Path for node '{node_name}' is invalid or not a directory.")
            return

        # Clear the tree
        for item in self.tree.get_children():
            self.tree.delete(item)
            
        # Add the root directory
        root_item = self.tree.insert("", "end", text=node_name, values=[node_path, "dir"], open=True)
        self.populate_tree(root_item, node_path)

    def populate_tree(self, parent_item, dir_path):
        """Lazily populates one level of the file tree."""
        # Clear any "dummy" children
        for item in self.tree.get_children(parent_item):
            self.tree.delete(item)
            
        try:
            entries = os.listdir(dir_path)
            # Sort directories first, then files
            entries.sort(key=lambda x: (not os.path.isdir(os.path.join(dir_path, x)), x.lower()))
            
            for item in entries:
                if item == ".log" or item == ".retain":
                    continue
                    
                full_path = os.path.join(dir_path, item)
                
                if os.path.isdir(full_path):
                    # It's a directory
                    child_item = self.tree.insert(parent_item, "end", text=item, values=[full_path, "dir"], open=False)
                    # Add a dummy child so it shows the [+] expand icon
                    self.tree.insert(child_item, "end", text="dummy")
                else:
                    # It's a file
                    self.tree.insert(parent_item, "end", text=item, values=[full_path, "file"])
                    
        except PermissionError:
            self.tree.insert(parent_item, "end", text="[Permission Denied]")
        except Exception as e:
            self.tree.insert(parent_item, "end", text=f"[Error: {e}]")

    def on_tree_open(self, event):
        """Called when a user clicks the [+] to expand a folder."""
        item = self.tree.focus() # The item being opened
        if not item:
            return
            
        try:
            values = self.tree.item(item, "values")
        except tk.TclError:
            return # Item doesn't exist anymore
            
        if not values or values[1] != "dir":
            return
            
        # Check if the first child is a "dummy"
        children = self.tree.get_children(item)
        if children and self.tree.item(children[0], "text") == "dummy":
            dir_path = values[0]
            # Lazily populate this directory now that it's been opened
            self.populate_tree(item, dir_path)

    def on_tree_double_click(self, event):
        """Called when a user double-clicks an item (to open a file)."""
        item = self.tree.focus()
        if not item:
            return
            
        try:
            values = self.tree.item(item, "values")
        except tk.TclError:
            return # Item doesn't exist anymore

        if not values or values[1] != "file":
            return
            
        file_path = values[0]
        self.open_file(file_path)

    def open_file(self, file_path):
        """Reads a file and loads it into the text editor."""
        try:
            # Check if it's the same file
            if file_path == self.current_file_path:
                return 

            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            self.editor.delete("1.0", tk.END)
            self.editor.insert("1.0", content)
            self.current_file_path = file_path
            self.root.title(f"Exodus Node Editor - {file_path}")
            
            # --- NEW: Update line numbers and reset view ---
            self.editor.mark_set("insert", "1.0") # Move cursor to start
            self.editor.see("1.0") # Scroll to start
            self.editor.edit_reset() # Clear undo stack
            self.editor.edit_modified(False) # Set modified flag
            self.editor_frame.on_text_change() # Manually trigger update
            # --- END NEW ---
            
        except UnicodeDecodeError:
            messagebox.showwarning("Open File", f"Cannot open file: {os.path.basename(file_path)}.\nIt appears to be a binary file.")
            self.current_file_path = None
        except Exception as e:
            messagebox.showerror("Error", f"Failed to open file {file_path}:\n{e}")
            self.current_file_path = None

    def save_file(self, event=None):
        """Saves the current editor content to the file."""
        if not self.current_file_path:
            messagebox.showwarning("Save", "No file is open to save.")
            return

        try:
            content = self.editor.get("1.0", "end-1c") # Get all text minus the final newline
            with open(self.current_file_path, 'w', encoding='utf-8') as f:
                f.write(content)
            
            print(f"File saved: {self.current_file_path}")
            
        except Exception as e:
            messagebox.showerror("Error", f"Failed to save file:\n{e}")


if __name__ == "__main__":
    root = tk.Tk()
    app = ExodusGUI(root)
    root.mainloop()