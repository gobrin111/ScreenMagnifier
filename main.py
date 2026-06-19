import customtkinter as ctk
import threading
import keyboard

from magnifier import Magnifier
from Config import config

# ─── Theme ───────────────────────────────────────────────────────────────────

ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("dark-blue")

ACCENT       = "#1f6aa5"
ACCENT_HOVER = "#144870"
BG_CARD      = "#2b2b2b"
TEXT_DIM     = "#888888"

# ─── App ─────────────────────────────────────────────────────────────────────

mag = Magnifier()
threading.Thread(target=mag.run, daemon=True).start()


class KeybindButton(ctk.CTkButton):
    """A button that captures a new key when clicked."""

    def __init__(self, master, label, config_attr, hook_name, **kwargs):
        self.config_attr = config_attr      # e.g. "TOGGLE_KEY"
        self.hook_name   = hook_name        # e.g. "toggle"
        self.label_text  = label
        self._rebind_hook = None

        current_key = getattr(config, config_attr)
        super().__init__(
            master,
            text=f"{label}:  {current_key.upper()}",
            command=self._start_rebind,
            font=ctk.CTkFont(size=13),
            height=36,
            corner_radius=8,
            fg_color=BG_CARD,
            hover_color=ACCENT_HOVER,
            border_width=1,
            border_color="#444444",
            **kwargs,
        )

    def _start_rebind(self):
        self.configure(text=f"{self.label_text}:  ···", fg_color=ACCENT)
        self._rebind_hook = keyboard.on_press(self._on_key)

    def _on_key(self, event):
        new_key = event.name

        # Update config
        setattr(config, self.config_attr, new_key)

        # Rebind in magnifier
        mag.rebind_key(self.hook_name, new_key)

        # Update button text
        self.configure(
            text=f"{self.label_text}:  {new_key.upper()}",
            fg_color=BG_CARD,
        )

        # Remove the temporary listener
        if self._rebind_hook is not None:
            keyboard.unhook(self._rebind_hook)
            self._rebind_hook = None


class App(ctk.CTk):
    def __init__(self):
        super().__init__()

        self.title("FPS Magnifier")
        self.geometry("340x620")
        self.resizable(False, False)

        # ── Header ───────────────────────────────────────────────────────
        header = ctk.CTkFrame(self, fg_color="transparent")
        header.pack(fill="x", padx=20, pady=(18, 4))

        ctk.CTkLabel(
            header,
            text="FPS Magnifier",
            font=ctk.CTkFont(size=22, weight="bold"),
        ).pack(side="left")

        self.status_label = ctk.CTkLabel(
            header,
            text="● OFF",
            font=ctk.CTkFont(size=13),
            text_color="#ff4444",
        )
        self.status_label.pack(side="right")

        ctk.CTkLabel(
            self,
            text="GPU-accelerated screen magnifier",
            font=ctk.CTkFont(size=12),
            text_color=TEXT_DIM,
        ).pack(anchor="w", padx=20, pady=(0, 12))

        # ── Toggle button ────────────────────────────────────────────────
        self.toggle_btn = ctk.CTkButton(
            self,
            text="Toggle Magnifier",
            command=self._toggle,
            height=40,
            corner_radius=10,
            font=ctk.CTkFont(size=14, weight="bold"),
        )
        self.toggle_btn.pack(fill="x", padx=20, pady=(0, 16))

        # ── Zoom slider ──────────────────────────────────────────────────
        self._section_label("Zoom")

        zoom_frame = ctk.CTkFrame(self, fg_color="transparent")
        zoom_frame.pack(fill="x", padx=20)

        self.zoom_val = ctk.CTkLabel(
            zoom_frame,
            text=f"{config.ZOOM:.1f}x",
            font=ctk.CTkFont(size=13, weight="bold"),
            width=44,
        )
        self.zoom_val.pack(side="right")

        self.zoom_slider = ctk.CTkSlider(
            zoom_frame,
            from_=config.ZOOM_MIN,
            to=config.ZOOM_MAX,
            number_of_steps=int((config.ZOOM_MAX - config.ZOOM_MIN) / config.ZOOM_STEP),
            command=self._on_zoom,
        )
        self.zoom_slider.set(config.ZOOM)
        self.zoom_slider.pack(side="left", fill="x", expand=True, padx=(0, 8))

        # ── Region slider ────────────────────────────────────────────────
        self._section_label("Capture Region")

        region_frame = ctk.CTkFrame(self, fg_color="transparent")
        region_frame.pack(fill="x", padx=20)

        self.region_val = ctk.CTkLabel(
            region_frame,
            text=f"{config.CAPTURE_RADIUS * 2}px",
            font=ctk.CTkFont(size=13, weight="bold"),
            width=50,
        )
        self.region_val.pack(side="right")

        self.region_slider = ctk.CTkSlider(
            region_frame,
            from_=config.CAPTURE_MIN,
            to=config.CAPTURE_MAX,
            number_of_steps=int(
                (config.CAPTURE_MAX - config.CAPTURE_MIN) / config.CAPTURE_STEP
            ),
            command=self._on_region,
        )
        self.region_slider.set(config.CAPTURE_RADIUS)
        self.region_slider.pack(side="left", fill="x", expand=True, padx=(0, 8))

        # ── Filter toggle ────────────────────────────────────────────────
        filter_frame = ctk.CTkFrame(self, fg_color="transparent")
        filter_frame.pack(fill="x", padx=20, pady=(12, 4))

        ctk.CTkLabel(
            filter_frame,
            text="GPU Filter",
            font=ctk.CTkFont(size=13),
            text_color=TEXT_DIM,
        ).pack(side="left")

        self.filter_var = ctk.StringVar(value=config.GPU_FILTER)
        self.filter_menu = ctk.CTkSegmentedButton(
            filter_frame,
            values=["bicubic", "linear", "nearest"],
            variable=self.filter_var,
            command=self._on_filter,
            font=ctk.CTkFont(size=12),
        )
        self.filter_menu.pack(side="right")

        # ── Crosshair toggle ─────────────────────────────────────────────
        cross_frame = ctk.CTkFrame(self, fg_color="transparent")
        cross_frame.pack(fill="x", padx=20, pady=(8, 4))

        ctk.CTkLabel(
            cross_frame,
            text="Crosshair",
            font=ctk.CTkFont(size=13),
            text_color=TEXT_DIM,
        ).pack(side="left")

        self.cross_var = ctk.BooleanVar(value=config.CROSSHAIR)
        self.cross_switch = ctk.CTkSwitch(
            cross_frame,
            text="",
            variable=self.cross_var,
            command=self._on_crosshair,
            width=46,
        )
        self.cross_switch.pack(side="right")

        # ── Keybinds section ─────────────────────────────────────────────
        self._section_label("Keybinds", top_pad=16)

        keybind_frame = ctk.CTkFrame(self, fg_color=BG_CARD, corner_radius=10)
        keybind_frame.pack(fill="x", padx=20, pady=(0, 12))

        binds = [
            ("Toggle",      "TOGGLE_KEY",      "toggle"),
            ("Zoom In",     "ZOOM_IN_KEY",     "zoom_in"),
            ("Zoom Out",    "ZOOM_OUT_KEY",    "zoom_out"),
            ("Region +",    "REGION_UP_KEY",   "region_up"),
            ("Region −",    "REGION_DOWN_KEY", "region_down"),
        ]

        for i, (label, attr, hook) in enumerate(binds):
            btn = KeybindButton(keybind_frame, label, attr, hook)
            btn.pack(fill="x", padx=8, pady=(8 if i == 0 else 2, 8 if i == len(binds) - 1 else 2))

        # ── Footer ───────────────────────────────────────────────────────
        ctk.CTkLabel(
            self,
            text="Game must be in Borderless Windowed",
            font=ctk.CTkFont(size=11),
            text_color="#666666",
        ).pack(side="bottom", pady=(0, 10))

        # ── Poll magnifier state for status indicator ────────────────────
        self._poll_status()

    # ── helpers ───────────────────────────────────────────────────────────

    def _section_label(self, text, top_pad=8):
        ctk.CTkLabel(
            self,
            text=text,
            font=ctk.CTkFont(size=12),
            text_color=TEXT_DIM,
        ).pack(anchor="w", padx=20, pady=(top_pad, 2))

    # ── callbacks ─────────────────────────────────────────────────────────

    def _toggle(self):
        mag.toggle()

    def _on_zoom(self, val):
        mag.zoom = round(float(val), 2)
        self.zoom_val.configure(text=f"{mag.zoom:.1f}x")

    def _on_region(self, val):
        mag.radius = int(float(val))
        self.region_val.configure(text=f"{mag.radius * 2}px")

    def _on_filter(self, val):
        config.GPU_FILTER = val
        # Shader is compiled at overlay init — tell magnifier to recreate
        mag._rebuild_overlay = True

    def _on_crosshair(self):
        config.CROSSHAIR = self.cross_var.get()

    def _poll_status(self):
        if mag.on:
            self.status_label.configure(text="● ON", text_color="#44ff44")
            self.toggle_btn.configure(fg_color="#2d8a4e", hover_color="#1f6b38")
        else:
            self.status_label.configure(text="● OFF", text_color="#ff4444")
            self.toggle_btn.configure(fg_color=ACCENT, hover_color=ACCENT_HOVER)

        # Sync slider to current zoom/region (could change via hotkey)
        self.zoom_slider.set(mag.zoom)
        self.zoom_val.configure(text=f"{mag.zoom:.1f}x")
        self.region_slider.set(mag.radius)
        self.region_val.configure(text=f"{mag.radius * 2}px")

        self.after(200, self._poll_status)


# ─── MAIN ────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    app = App()
    app.mainloop()