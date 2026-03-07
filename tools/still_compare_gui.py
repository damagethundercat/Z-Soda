#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import threading
import traceback
import webbrowser
from pathlib import Path
from typing import Dict

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from PIL import Image, ImageOps, ImageTk

VIDEO_EXTENSIONS = {
    ".avi",
    ".m4v",
    ".mkv",
    ".mov",
    ".mp4",
    ".mpeg",
    ".mpg",
    ".webm",
    ".wmv",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Desktop GUI for still depth comparison bundles.")
    parser.add_argument("--smoke-test", action="store_true", help="Construct the UI and exit immediately.")
    parser.add_argument("--bundle-dir", default="", help="Existing comparison bundle directory to load on startup.")
    return parser.parse_args()


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_output_dir() -> Path:
    return repo_root() / "artifacts" / "still_compare_gui"


def open_path(path: Path) -> None:
    if not path.exists():
        return
    if os.name == "nt":
        os.startfile(str(path))  # type: ignore[attr-defined]
        return
    webbrowser.open(path.as_uri())


def is_video_path(path: str | Path) -> bool:
    return Path(path).suffix.lower() in VIDEO_EXTENSIONS


def looks_like_harness_artifact(path: str | Path) -> bool:
    candidate = Path(path)
    normalized = {part.lower() for part in candidate.parts}
    return candidate.name.lower() == "input_source.png" or (
        "artifacts" in normalized and "zsoda" in normalized
    )


class PreviewPane(ttk.Frame):
    def __init__(self, master: tk.Misc, title: str, *, width: int = 340, height: int = 220) -> None:
        super().__init__(master, style="Card.TFrame", padding=10)
        self.preview_width = width
        self.preview_height = height
        self.title_label = ttk.Label(self, text=title, style="CardTitle.TLabel")
        self.title_label.pack(anchor="w")
        self.path_label = ttk.Label(self, text="No image", style="Hint.TLabel", wraplength=width)
        self.path_label.pack(anchor="w", pady=(2, 8))
        self.image_label = ttk.Label(self, text="Drop in a result", style="ImageHint.TLabel", anchor="center")
        self.image_label.pack(fill="both", expand=True)
        self.image_label.bind("<Button-1>", self._open_image)
        self.current_path: Path | None = None
        self._photo: ImageTk.PhotoImage | None = None

    def _open_image(self, _event: tk.Event) -> None:
        if self.current_path is not None:
            open_path(self.current_path)

    def show_image(self, image_path: str | Path | None, note: str = "") -> None:
        self.current_path = None
        self._photo = None
        if not image_path:
            self.path_label.configure(text=note or "No image")
            self.image_label.configure(text="No image", image="")
            return

        path = Path(image_path)
        if not path.exists():
            self.path_label.configure(text=f"Missing: {path}")
            self.image_label.configure(text="Missing", image="")
            return

        try:
            image = Image.open(path).convert("RGB")
            preview = ImageOps.contain(image, (self.preview_width, self.preview_height))
            self._photo = ImageTk.PhotoImage(preview)
            self.image_label.configure(image=self._photo, text="")
            self.path_label.configure(text=str(path))
            self.current_path = path
        except Exception as exc:  # noqa: BLE001
            self.path_label.configure(text=f"Failed to load: {path}")
            self.image_label.configure(text=str(exc), image="")


class ComparisonGui:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Z-Soda Still Compare")
        self.root.geometry("1560x980")
        self.root.configure(bg="#ece6dc")

        self.compare_script = repo_root() / "tools" / "still_quality_compare.py"
        self.report_path: Path | None = None
        self._worker: threading.Thread | None = None

        self.source_var = tk.StringVar()
        self.source_time_var = tk.DoubleVar(value=0.0)
        self.qd3_var = tk.StringVar()
        self.official_image_var = tk.StringVar()
        self.output_dir_var = tk.StringVar(value=str(default_output_dir()))
        self.model_root_var = tk.StringVar(value=str(repo_root() / "models"))
        self.model_id_var = tk.StringVar(value="depth-anything-v3-large-multiview")
        self.backend_var = tk.StringVar(value="auto")
        self.resize_mode_var = tk.StringVar(value="upper_bound_letterbox")
        self.mapping_mode_var = tk.StringVar(value="v2-style")
        self.quality_var = tk.IntVar(value=1)
        self.guided_low_var = tk.DoubleVar(value=0.02)
        self.guided_high_var = tk.DoubleVar(value=0.98)
        self.guided_alpha_var = tk.DoubleVar(value=0.12)
        self.temporal_alpha_var = tk.DoubleVar(value=1.0)
        self.edge_enhancement_var = tk.DoubleVar(value=0.18)
        self.auto_official_var = tk.BooleanVar(value=True)
        self.official_repo_root_var = tk.StringVar(
            value=str(repo_root() / ".tmp_external_research" / "Depth-Anything-3")
        )
        self.official_model_repo_var = tk.StringVar(value="depth-anything/DA3-LARGE-1.1")
        self.official_process_res_var = tk.IntVar(value=504)
        self.official_device_var = tk.StringVar(value="auto")

        self.preview_panes: Dict[str, PreviewPane] = {}
        self.status_text: tk.Text | None = None
        self.run_button: ttk.Button | None = None
        self.open_report_button: ttk.Button | None = None
        self.sidebar_canvas: tk.Canvas | None = None
        self.sidebar_frame: ttk.Frame | None = None
        self.sidebar_window_id: int | None = None

        self.official_image_var.trace_add("write", lambda *_args: self._update_manual_preview("official da3"))
        self._build_style()
        self._build_layout()
        self._bind_sidebar_mousewheel()

    def _build_style(self) -> None:
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure("Root.TFrame", background="#ece6dc")
        style.configure("Sidebar.TFrame", background="#1f2a2c")
        style.configure("SidebarHeader.TLabel", background="#1f2a2c", foreground="#f4ede1", font=("Segoe UI", 18, "bold"))
        style.configure("Section.TLabel", background="#1f2a2c", foreground="#d7c7b3", font=("Segoe UI", 10, "bold"))
        style.configure("Field.TLabel", background="#1f2a2c", foreground="#f4ede1", font=("Segoe UI", 10))
        style.configure("Hint.TLabel", background="#fffaf2", foreground="#6a655d", font=("Segoe UI", 9))
        style.configure("ImageHint.TLabel", background="#f6efe2", foreground="#7a7267", font=("Segoe UI", 10))
        style.configure("Card.TFrame", background="#fffaf2", relief="flat")
        style.configure("CardTitle.TLabel", background="#fffaf2", foreground="#9a3f22", font=("Segoe UI", 12, "bold"))
        style.configure("Accent.TButton", font=("Segoe UI", 10, "bold"))
        style.configure("Ghost.TButton", font=("Segoe UI", 10))
        style.map("Accent.TButton", background=[("active", "#b95027")], foreground=[("active", "#ffffff")])

    def _build_layout(self) -> None:
        root_frame = ttk.Frame(self.root, style="Root.TFrame")
        root_frame.pack(fill="both", expand=True)
        root_frame.columnconfigure(1, weight=1)
        root_frame.rowconfigure(0, weight=1)

        sidebar_shell = ttk.Frame(root_frame, style="Sidebar.TFrame", width=430)
        sidebar_shell.grid(row=0, column=0, sticky="ns")
        sidebar_shell.grid_propagate(False)
        sidebar_shell.rowconfigure(0, weight=1)
        sidebar_shell.columnconfigure(0, weight=1)

        self.sidebar_canvas = tk.Canvas(
            sidebar_shell,
            bg="#1f2a2c",
            highlightthickness=0,
            borderwidth=0,
            width=430,
        )
        sidebar_scrollbar = ttk.Scrollbar(sidebar_shell, orient="vertical", command=self.sidebar_canvas.yview)
        self.sidebar_canvas.configure(yscrollcommand=sidebar_scrollbar.set)
        self.sidebar_canvas.grid(row=0, column=0, sticky="nsew")
        sidebar_scrollbar.grid(row=0, column=1, sticky="ns")

        sidebar = ttk.Frame(self.sidebar_canvas, style="Sidebar.TFrame", padding=18)
        self.sidebar_window_id = self.sidebar_canvas.create_window((0, 0), window=sidebar, anchor="nw")
        sidebar.bind("<Configure>", self._configure_sidebar_scrollregion)
        self.sidebar_canvas.bind("<Configure>", self._sync_sidebar_width)
        self.sidebar_frame = sidebar

        ttk.Label(sidebar, text="Still Compare GUI", style="SidebarHeader.TLabel").pack(anchor="w")
        ttk.Label(
            sidebar,
            text="QD3는 직접 업로드하고, official DA3와 Z-Soda는 현재 워크스페이스 기준으로 즉시 생성합니다.",
            style="Hint.TLabel",
            wraplength=390,
        ).pack(anchor="w", pady=(6, 14))

        self._add_path_field(sidebar, "Source", self.source_var, self._pick_source)
        self._add_entry(sidebar, "Video Time (s)", self.source_time_var)
        self._add_path_field(sidebar, "QD3", self.qd3_var, self._pick_qd3)
        self._add_official_block(sidebar)
        self._add_path_field(sidebar, "Output Dir", self.output_dir_var, self._pick_output_dir, directory=True)

        ttk.Label(sidebar, text="Z-Soda Settings", style="Section.TLabel").pack(anchor="w", pady=(16, 6))
        self._add_path_field(sidebar, "Model Root", self.model_root_var, self._pick_model_root, directory=True)
        self._add_entry(sidebar, "Model ID", self.model_id_var)
        self._add_combo(sidebar, "Backend", self.backend_var, ["auto", "cpu", "cuda", "directml", "tensorrt", "remote"])
        self._add_combo(sidebar, "Resize", self.resize_mode_var, ["upper_bound_letterbox", "lower_bound_center_crop"])
        self._add_combo(sidebar, "Mapping", self.mapping_mode_var, ["v2-style", "raw", "normalize", "guided"])
        self._add_spin(sidebar, "Quality", self.quality_var, 1, 4)
        self._add_entry(sidebar, "Guided Low", self.guided_low_var)
        self._add_entry(sidebar, "Guided High", self.guided_high_var)
        self._add_entry(sidebar, "Guided Alpha", self.guided_alpha_var)
        self._add_entry(sidebar, "Temporal Alpha", self.temporal_alpha_var)
        self._add_entry(sidebar, "Edge Enhance", self.edge_enhancement_var)

        button_row = ttk.Frame(sidebar, style="Sidebar.TFrame")
        button_row.pack(fill="x", pady=(18, 10))
        self.run_button = ttk.Button(button_row, text="Run Compare", style="Accent.TButton", command=self.run_compare)
        self.run_button.pack(side="left", fill="x", expand=True)
        ttk.Button(button_row, text="Load Bundle", style="Ghost.TButton", command=self.load_bundle).pack(side="left", padx=(8, 0))

        footer_row = ttk.Frame(sidebar, style="Sidebar.TFrame")
        footer_row.pack(fill="x")
        self.open_report_button = ttk.Button(footer_row, text="Open Report", style="Ghost.TButton", command=self.open_report)
        self.open_report_button.pack(side="left")
        ttk.Button(footer_row, text="Open Output", style="Ghost.TButton", command=self.open_output_dir).pack(side="left", padx=(8, 0))

        main_area = ttk.Frame(root_frame, style="Root.TFrame", padding=(18, 18, 18, 12))
        main_area.grid(row=0, column=1, sticky="nsew")
        main_area.columnconfigure(0, weight=1)
        main_area.rowconfigure(0, weight=1)
        main_area.rowconfigure(1, weight=0)

        previews = ttk.Frame(main_area, style="Root.TFrame")
        previews.grid(row=0, column=0, sticky="nsew")
        for col in range(3):
            previews.columnconfigure(col, weight=1)
        for row in range(2):
            previews.rowconfigure(row, weight=1)

        pane_specs = [
            ("source", "Source"),
            ("qd3", "QD3"),
            ("official", "Official DA3"),
            ("zsoda_raw", "Z-Soda Raw"),
            ("zsoda_pipeline", "Z-Soda Pipeline"),
        ]
        for index, (key, title) in enumerate(pane_specs):
            pane = PreviewPane(previews, title)
            pane.grid(row=index // 3, column=index % 3, sticky="nsew", padx=8, pady=8)
            self.preview_panes[key] = pane

        status_card = ttk.Frame(main_area, style="Card.TFrame", padding=12)
        status_card.grid(row=1, column=0, sticky="ew", pady=(12, 0))
        ttk.Label(status_card, text="Status", style="CardTitle.TLabel").pack(anchor="w")
        self.status_text = tk.Text(
            status_card,
            height=10,
            wrap="word",
            bg="#f6efe2",
            fg="#2c2925",
            relief="flat",
            font=("Consolas", 10),
        )
        scrollbar = ttk.Scrollbar(status_card, orient="vertical", command=self.status_text.yview)
        self.status_text.configure(yscrollcommand=scrollbar.set)
        self.status_text.pack(side="left", fill="both", expand=True, pady=(8, 0))
        scrollbar.pack(side="right", fill="y", pady=(8, 0))
        self.log("GUI ready.")

    def _configure_sidebar_scrollregion(self, _event: tk.Event) -> None:
        if self.sidebar_canvas is None:
            return
        self.sidebar_canvas.configure(scrollregion=self.sidebar_canvas.bbox("all"))

    def _sync_sidebar_width(self, event: tk.Event) -> None:
        if self.sidebar_canvas is None or self.sidebar_window_id is None:
            return
        self.sidebar_canvas.itemconfigure(self.sidebar_window_id, width=event.width)

    def _bind_sidebar_mousewheel(self) -> None:
        self.root.bind_all("<MouseWheel>", self._on_sidebar_mousewheel, add="+")
        self.root.bind_all("<Button-4>", self._on_sidebar_mousewheel_linux, add="+")
        self.root.bind_all("<Button-5>", self._on_sidebar_mousewheel_linux, add="+")

    def _widget_in_sidebar(self, widget: tk.Misc | None) -> bool:
        current = widget
        while current is not None:
            if current is self.sidebar_canvas or current is self.sidebar_frame:
                return True
            try:
                parent_name = current.winfo_parent()
            except tk.TclError:
                return False
            if not parent_name:
                return False
            try:
                current = current.nametowidget(parent_name)
            except KeyError:
                return False
        return False

    def _on_sidebar_mousewheel(self, event: tk.Event) -> None:
        if self.sidebar_canvas is None:
            return
        widget = self.root.winfo_containing(event.x_root, event.y_root)
        if not self._widget_in_sidebar(widget):
            return
        delta = int(-event.delta / 120) if event.delta else 0
        if delta:
            self.sidebar_canvas.yview_scroll(delta, "units")

    def _on_sidebar_mousewheel_linux(self, event: tk.Event) -> None:
        if self.sidebar_canvas is None:
            return
        widget = self.root.winfo_containing(event.x_root, event.y_root)
        if not self._widget_in_sidebar(widget):
            return
        if getattr(event, "num", None) == 4:
            self.sidebar_canvas.yview_scroll(-1, "units")
        elif getattr(event, "num", None) == 5:
            self.sidebar_canvas.yview_scroll(1, "units")

    def _add_path_field(
        self,
        parent: ttk.Frame,
        label: str,
        variable: tk.Variable,
        browse_command,
        *,
        directory: bool = False,
    ) -> None:
        frame = ttk.Frame(parent, style="Sidebar.TFrame")
        frame.pack(fill="x", pady=4)
        ttk.Label(frame, text=label, style="Field.TLabel").pack(anchor="w")
        ttk.Entry(frame, textvariable=variable).pack(fill="x", pady=(4, 4))
        ttk.Button(frame, text="Browse", style="Ghost.TButton", command=browse_command).pack(anchor="e")
        if not directory:
            variable.trace_add("write", lambda *_args, key=label.lower(): self._update_manual_preview(key))

    def _add_official_block(self, parent: ttk.Frame) -> None:
        container = ttk.Frame(parent, style="Sidebar.TFrame")
        container.pack(fill="x", pady=4)
        ttk.Label(container, text="Official DA3", style="Field.TLabel").pack(anchor="w")
        ttk.Checkbutton(
            container,
            text="Auto-generate official output",
            variable=self.auto_official_var,
            command=self._refresh_official_mode,
        ).pack(anchor="w", pady=(4, 6))
        self.official_manual_frame = ttk.Frame(container, style="Sidebar.TFrame")
        ttk.Entry(self.official_manual_frame, textvariable=self.official_image_var).pack(fill="x", pady=(0, 4))
        ttk.Button(self.official_manual_frame, text="Browse", style="Ghost.TButton", command=self._pick_official_image).pack(anchor="e")

        self.official_auto_frame = ttk.Frame(container, style="Sidebar.TFrame")
        self._add_entry(self.official_auto_frame, "Repo Root", self.official_repo_root_var)
        ttk.Button(self.official_auto_frame, text="Browse Repo", style="Ghost.TButton", command=self._pick_official_repo_root).pack(anchor="e")
        self._add_entry(self.official_auto_frame, "Model Repo", self.official_model_repo_var)
        self._add_spin(self.official_auto_frame, "Process Res", self.official_process_res_var, 128, 2048, increment=14)
        self._add_combo(self.official_auto_frame, "Device", self.official_device_var, ["auto", "cpu", "cuda"])
        self._refresh_official_mode()

    def _refresh_official_mode(self) -> None:
        self.official_manual_frame.pack_forget()
        self.official_auto_frame.pack_forget()
        if self.auto_official_var.get():
            self.official_auto_frame.pack(fill="x")
        else:
            self.official_manual_frame.pack(fill="x")
            self._update_manual_preview("official da3")

    def _add_entry(self, parent: ttk.Frame, label: str, variable: tk.Variable) -> None:
        frame = ttk.Frame(parent, style="Sidebar.TFrame")
        frame.pack(fill="x", pady=4)
        ttk.Label(frame, text=label, style="Field.TLabel").pack(anchor="w")
        ttk.Entry(frame, textvariable=variable).pack(fill="x", pady=(4, 0))

    def _add_combo(self, parent: ttk.Frame, label: str, variable: tk.Variable, values) -> None:
        frame = ttk.Frame(parent, style="Sidebar.TFrame")
        frame.pack(fill="x", pady=4)
        ttk.Label(frame, text=label, style="Field.TLabel").pack(anchor="w")
        ttk.Combobox(frame, textvariable=variable, values=list(values), state="readonly").pack(fill="x", pady=(4, 0))

    def _add_spin(
        self,
        parent: ttk.Frame,
        label: str,
        variable: tk.Variable,
        start: int,
        end: int,
        *,
        increment: int = 1,
    ) -> None:
        frame = ttk.Frame(parent, style="Sidebar.TFrame")
        frame.pack(fill="x", pady=4)
        ttk.Label(frame, text=label, style="Field.TLabel").pack(anchor="w")
        tk.Spinbox(frame, from_=start, to=end, increment=increment, textvariable=variable).pack(fill="x", pady=(4, 0))

    def _pick_source(self) -> None:
        path = filedialog.askopenfilename(title="Select source image or video")
        if path:
            self.source_var.set(path)
            if is_video_path(path):
                self.preview_panes["source"].show_image(
                    None,
                    note=f"Video source selected: {Path(path).name}\nPreview updates after extraction/run.",
                )
            else:
                self.preview_panes["source"].show_image(path)

    def _pick_qd3(self) -> None:
        path = filedialog.askopenfilename(title="Select QD3 depth image")
        if path:
            self.qd3_var.set(path)
            self.preview_panes["qd3"].show_image(path)

    def _pick_official_image(self) -> None:
        path = filedialog.askopenfilename(title="Select official DA3 image")
        if path:
            self.official_image_var.set(path)
            self.preview_panes["official"].show_image(path)

    def _pick_output_dir(self) -> None:
        path = filedialog.askdirectory(title="Select output directory")
        if path:
            self.output_dir_var.set(path)

    def _pick_model_root(self) -> None:
        path = filedialog.askdirectory(title="Select model root")
        if path:
            self.model_root_var.set(path)

    def _pick_official_repo_root(self) -> None:
        path = filedialog.askdirectory(title="Select Depth-Anything-3 repo root")
        if path:
            self.official_repo_root_var.set(path)

    def _update_manual_preview(self, key: str) -> None:
        key_map = {
            "source": ("source", self.source_var.get()),
            "qd3": ("qd3", self.qd3_var.get()),
            "official da3": ("official", self.official_image_var.get()),
        }
        if key in key_map:
            pane_key, path = key_map[key]
            if path:
                if pane_key == "source" and is_video_path(path):
                    self.preview_panes[pane_key].show_image(
                        None,
                        note=f"Video source selected: {Path(path).name}\nPreview updates after extraction/run.",
                    )
                else:
                    self.preview_panes[pane_key].show_image(path)

    def log(self, message: str) -> None:
        if self.status_text is None:
            return
        self.status_text.insert("end", message.rstrip() + "\n")
        self.status_text.see("end")

    def set_busy(self, busy: bool) -> None:
        if self.run_button is not None:
            self.run_button.configure(state="disabled" if busy else "normal")

    def build_command(self) -> list[str]:
        command = [
            sys.executable,
            str(self.compare_script),
            "--input",
            self.source_var.get(),
            "--video-time-seconds",
            str(self.source_time_var.get()),
            "--output-dir",
            self.output_dir_var.get(),
            "--model-root",
            self.model_root_var.get(),
            "--model-id",
            self.model_id_var.get(),
            "--backend",
            self.backend_var.get(),
            "--resize-mode",
            self.resize_mode_var.get(),
            "--quality",
            str(self.quality_var.get()),
            "--mapping-mode",
            self.mapping_mode_var.get(),
            "--guided-low",
            str(self.guided_low_var.get()),
            "--guided-high",
            str(self.guided_high_var.get()),
            "--guided-alpha",
            str(self.guided_alpha_var.get()),
            "--temporal-alpha",
            str(self.temporal_alpha_var.get()),
            "--edge-enhancement",
            str(self.edge_enhancement_var.get()),
        ]
        if self.qd3_var.get():
            command.extend(["--qd3-image", self.qd3_var.get()])
        if self.auto_official_var.get():
            command.extend(["--official-repo-root", self.official_repo_root_var.get()])
            command.extend(["--official-model-repo", self.official_model_repo_var.get()])
            command.extend(["--official-process-res", str(self.official_process_res_var.get())])
            command.extend(["--official-device", self.official_device_var.get()])
        elif self.official_image_var.get():
            command.extend(["--official-image", self.official_image_var.get()])
        else:
            command.append("--skip-official")
        return command

    def validate(self) -> bool:
        if not self.compare_script.exists():
            messagebox.showerror("Missing Script", f"Compare script not found: {self.compare_script}")
            return False
        if not self.source_var.get():
            messagebox.showerror("Missing Source", "Source image or video is required.")
            return False
        source_path = Path(self.source_var.get())
        if not source_path.exists():
            messagebox.showerror("Missing Source", "Selected source image or video does not exist.")
            return False
        if looks_like_harness_artifact(source_path):
            messagebox.showerror(
                "Invalid Source",
                "Select the original source image/video, not a previous harness artifact such as artifacts/.../zsoda/input_source.png.",
            )
            return False
        if self.model_root_var.get() and not Path(self.model_root_var.get()).exists():
            messagebox.showerror("Missing Model Root", "Selected model root does not exist.")
            return False
        if self.auto_official_var.get():
            repo_root_path = Path(self.official_repo_root_var.get())
            if not repo_root_path.exists():
                messagebox.showerror("Missing Official Repo", "Official repo root does not exist.")
                return False
        elif self.official_image_var.get() and not Path(self.official_image_var.get()).exists():
            messagebox.showerror("Missing Official Image", "Selected official DA3 image does not exist.")
            return False
        if self.qd3_var.get() and not Path(self.qd3_var.get()).exists():
            messagebox.showerror("Missing QD3 Image", "Selected QD3 image does not exist.")
            return False
        return True

    def run_compare(self) -> None:
        if self._worker is not None and self._worker.is_alive():
            return
        if not self.validate():
            return

        command = self.build_command()
        self.log("Running comparison bundle...")
        self.log(subprocess.list2cmdline(command))
        self.set_busy(True)

        def worker() -> None:
            try:
                completed = subprocess.run(
                    command,
                    cwd=str(repo_root()),
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.root.after(0, self._handle_compare_result, completed.returncode, completed.stdout, completed.stderr)
            except Exception as exc:  # noqa: BLE001
                self.root.after(0, self._handle_worker_exception, exc, traceback.format_exc())

        self._worker = threading.Thread(target=worker, daemon=True)
        self._worker.start()

    def _handle_compare_result(self, returncode: int, stdout: str, stderr: str) -> None:
        self.set_busy(False)
        if stdout.strip():
            self.log(stdout.strip())
        if stderr.strip():
            self.log(stderr.strip())
        if returncode != 0:
            messagebox.showerror("Comparison Failed", stderr.strip() or stdout.strip() or "Unknown error")
            return
        self.load_bundle_from_dir(Path(self.output_dir_var.get()))

    def _handle_worker_exception(self, exc: Exception, trace: str) -> None:
        self.set_busy(False)
        self.log(trace)
        messagebox.showerror("Comparison Failed", str(exc))

    def load_bundle(self) -> None:
        path = filedialog.askdirectory(title="Select comparison bundle directory")
        if not path:
            return
        self.load_bundle_from_dir(Path(path))

    def load_bundle_from_dir(self, bundle_dir: Path) -> None:
        manifest_path = bundle_dir / "comparison_manifest.json"
        if not manifest_path.exists():
            messagebox.showerror("Missing Bundle", f"comparison_manifest.json not found in {bundle_dir}")
            return
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            messagebox.showerror("Invalid Bundle", f"Failed to parse {manifest_path}:\n{exc}")
            return
        self.output_dir_var.set(str(bundle_dir))
        self.report_path = bundle_dir / "comparison_report.html"
        artifacts = manifest.get("artifacts", {})
        zsoda = artifacts.get("zsoda", {})
        refs = artifacts.get("references", {})

        self.preview_panes["source"].show_image(zsoda.get("input_copy"))
        self.preview_panes["qd3"].show_image(refs.get("qd3"), note="QD3 image not provided")
        self.preview_panes["official"].show_image(refs.get("official"), note=manifest.get("official", {}).get("status", "No official image"))
        self.preview_panes["zsoda_raw"].show_image(zsoda.get("raw_png"), note="No Z-Soda raw preview")
        self.preview_panes["zsoda_pipeline"].show_image(zsoda.get("pipeline_png"), note="No Z-Soda pipeline preview")

        if "input" in manifest:
            self.source_var.set(manifest["input"])
        source_meta = manifest.get("source", {})
        if "video_time_seconds" in source_meta:
            self.source_time_var.set(source_meta["video_time_seconds"])
        if refs.get("qd3"):
            self.qd3_var.set(refs["qd3"])
        if not self.auto_official_var.get() and refs.get("official"):
            self.official_image_var.set(refs["official"])

        self.log(f"Loaded bundle: {bundle_dir}")

    def open_report(self) -> None:
        if self.report_path is None:
            candidate = Path(self.output_dir_var.get()) / "comparison_report.html"
            if candidate.exists():
                self.report_path = candidate
        if self.report_path is None or not self.report_path.exists():
            messagebox.showinfo("No Report", "Run a comparison or load an existing bundle first.")
            return
        open_path(self.report_path)

    def open_output_dir(self) -> None:
        path = Path(self.output_dir_var.get())
        path.mkdir(parents=True, exist_ok=True)
        open_path(path)


def main() -> int:
    args = parse_args()
    root = tk.Tk()
    gui = ComparisonGui(root)
    if args.bundle_dir:
        gui.load_bundle_from_dir(Path(args.bundle_dir))
    if args.smoke_test:
        root.update_idletasks()
        root.update()
        gui.log("smoke-test=ok")
        root.destroy()
        return 0
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
