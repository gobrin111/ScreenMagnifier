import json
import os
from pathlib import Path


# ─── CONFIG ──────────────────────────────────────────────────────────────────
class Config:
    APP_DIRECTORY = "FPSMagnifier"
    SETTINGS_FILENAME = "settings.json"
    SETTINGS_VERSION = 1

    def __init__(self, settings_path=None):
        # ── Keybinds ─────────────────────────────────────────────────────
        self.TOGGLE_KEY      = "v"
        self.ZOOM_IN_KEY     = "="
        self.ZOOM_OUT_KEY    = "-"
        self.REGION_UP_KEY   = "]"
        self.REGION_DOWN_KEY = "["

        # ── Zoom ─────────────────────────────────────────────────────────
        self.ZOOM            = 2.0
        self.ZOOM_MIN        = 1.5
        self.ZOOM_MAX        = 6.0
        self.ZOOM_STEP       = 0.25

        # ── Capture ──────────────────────────────────────────────────────
        self.CAPTURE_RADIUS  = 200
        self.CAPTURE_MIN     = 80
        self.CAPTURE_MAX     = 400
        self.CAPTURE_STEP    = 20

        # Monitor index from mss.monitors. 1 is usually the primary display.
        self.MONITOR_INDEX    = 1

        # ── Performance ──────────────────────────────────────────────────
        self.FPS             = 45
        self.FPS_OPTIONS     = (30, 45, 60)
        self.IDLE_FPS        = 10
        self.TOPMOST_MS      = 2000
        self.LOW_PRIORITY    = True
        self.HOTKEYS_ENABLED = True

        # ── Appearance ───────────────────────────────────────────────────
        self.BORDER_COLOR    = (0.0, 0.78, 0.0)
        self.BORDER_PX       = 2

        # True hides the overlay from screenshots/recording when Windows supports it.
        # Set False if you want screenshots to include the zoom window.
        self.EXCLUDE_FROM_CAPTURE = True

        # "bicubic" = Catmull-Rom shader, "linear" = GPU bilinear, "nearest" = pixel-perfect
        self.GPU_FILTER      = "linear"

        self.settings_path = (
            Path(settings_path)
            if settings_path is not None
            else self._default_settings_path()
        )
        self.load()

    @classmethod
    def _default_settings_path(cls):
        app_data = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA")
        if app_data:
            base_directory = Path(app_data)
        else:
            base_directory = Path.home() / ".config"
        return base_directory / cls.APP_DIRECTORY / cls.SETTINGS_FILENAME

    @staticmethod
    def _key(value, default):
        if not isinstance(value, str):
            return default
        value = value.strip()
        if not value or len(value) > 64 or not value.isprintable():
            return default
        return value

    @staticmethod
    def _integer(value, default, minimum, maximum=None):
        if not isinstance(value, int) or isinstance(value, bool):
            return default
        if maximum is None:
            return max(minimum, value)
        return max(minimum, min(maximum, value))

    @staticmethod
    def _number(value, default, minimum, maximum):
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            return default
        return round(max(minimum, min(maximum, float(value))), 2)

    def load(self):
        try:
            data = json.loads(self.settings_path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            print(f"  Could not load settings; using defaults: {exc}")
            return

        if not isinstance(data, dict):
            print("  Could not load settings; using defaults: invalid file format")
            return

        self.TOGGLE_KEY = self._key(data.get("toggle_key"), self.TOGGLE_KEY)
        self.ZOOM_IN_KEY = self._key(data.get("zoom_in_key"), self.ZOOM_IN_KEY)
        self.ZOOM_OUT_KEY = self._key(data.get("zoom_out_key"), self.ZOOM_OUT_KEY)
        self.REGION_UP_KEY = self._key(
            data.get("region_up_key"),
            self.REGION_UP_KEY,
        )
        self.REGION_DOWN_KEY = self._key(
            data.get("region_down_key"),
            self.REGION_DOWN_KEY,
        )
        self.ZOOM = self._number(
            data.get("zoom"),
            self.ZOOM,
            self.ZOOM_MIN,
            self.ZOOM_MAX,
        )
        self.CAPTURE_RADIUS = self._integer(
            data.get("capture_radius"),
            self.CAPTURE_RADIUS,
            self.CAPTURE_MIN,
            self.CAPTURE_MAX,
        )
        self.MONITOR_INDEX = self._integer(
            data.get("monitor_index"),
            self.MONITOR_INDEX,
            1,
        )

        fps = data.get("fps")
        if (
            isinstance(fps, int)
            and not isinstance(fps, bool)
            and fps in self.FPS_OPTIONS
        ):
            self.FPS = fps

        hotkeys_enabled = data.get("hotkeys_enabled")
        if isinstance(hotkeys_enabled, bool):
            self.HOTKEYS_ENABLED = hotkeys_enabled

        gpu_filter = data.get("gpu_filter")
        if (
            isinstance(gpu_filter, str)
            and gpu_filter in {"bicubic", "linear", "nearest"}
        ):
            self.GPU_FILTER = gpu_filter

    def as_dict(self):
        return {
            "version": self.SETTINGS_VERSION,
            "toggle_key": self.TOGGLE_KEY,
            "zoom_in_key": self.ZOOM_IN_KEY,
            "zoom_out_key": self.ZOOM_OUT_KEY,
            "region_up_key": self.REGION_UP_KEY,
            "region_down_key": self.REGION_DOWN_KEY,
            "zoom": self.ZOOM,
            "capture_radius": self.CAPTURE_RADIUS,
            "monitor_index": self.MONITOR_INDEX,
            "fps": self.FPS,
            "hotkeys_enabled": self.HOTKEYS_ENABLED,
            "gpu_filter": self.GPU_FILTER,
        }

    def save(self):
        temporary_path = self.settings_path.with_name(
            f"{self.settings_path.name}.{os.getpid()}.tmp"
        )
        try:
            self.settings_path.parent.mkdir(parents=True, exist_ok=True)
            temporary_path.write_text(
                json.dumps(self.as_dict(), indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            temporary_path.replace(self.settings_path)
            return True
        except OSError as exc:
            print(f"  Could not save settings: {exc}")
            try:
                temporary_path.unlink(missing_ok=True)
            except OSError:
                pass
            return False


config = Config()
