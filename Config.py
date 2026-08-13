# ─── CONFIG ──────────────────────────────────────────────────────────────────
class Config:
    def __init__(self):
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

        # ── Appearance ───────────────────────────────────────────────────
        self.BORDER_COLOR    = (0.0, 0.78, 0.0)
        self.BORDER_PX       = 2

        # True hides the overlay from screenshots/recording when Windows supports it.
        # Set False if you want screenshots to include the zoom window.
        self.EXCLUDE_FROM_CAPTURE = True

        # "bicubic" = Catmull-Rom shader, "linear" = GPU bilinear, "nearest" = pixel-perfect
        self.GPU_FILTER      = "linear"


config = Config()
