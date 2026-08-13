"""
FPS Screen Magnifier — GPU Accelerated + Bicubic Shader
========================================================
Captures the center of your screen with mss, uploads to an OpenGL texture,
and renders with either:
  - Catmull-Rom bicubic interpolation (GLSL shader, 4 bilinear taps)
  - GPU bilinear filtering
  - Nearest-neighbor (pixel-perfect)

Requirements:
    pip install mss keyboard pywin32 PyOpenGL

Your game MUST be in Borderless Windowed mode.
"""

import ctypes
import time

import mss
import keyboard

import win32gui
import win32con
import win32api

from OpenGL.GL import *

# Use ctypes for WGL — PyOpenGL's WGL wrappers choke on pywin32 handle types
opengl32 = ctypes.windll.opengl32

from Config import config

# ─── Win32 / WGL constants ──────────────────────────────────────────────────

user32 = ctypes.windll.user32
gdi32  = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32

_vp = ctypes.c_void_p

user32.GetDC.restype         = _vp
user32.GetDC.argtypes        = [_vp]
user32.ReleaseDC.argtypes    = [_vp, _vp]
user32.GetWindowLongW.argtypes  = [_vp, ctypes.c_int]
user32.SetWindowLongW.argtypes  = [_vp, ctypes.c_int, ctypes.c_long]
user32.SetLayeredWindowAttributes.argtypes = [_vp, ctypes.c_uint, ctypes.c_ubyte, ctypes.c_uint]
user32.SetWindowDisplayAffinity.restype = ctypes.c_bool
user32.SetWindowDisplayAffinity.argtypes = [_vp, ctypes.c_uint]

gdi32.ChoosePixelFormat.restype  = ctypes.c_int
gdi32.ChoosePixelFormat.argtypes = [_vp, ctypes.c_void_p]
gdi32.SetPixelFormat.restype     = ctypes.c_bool
gdi32.SetPixelFormat.argtypes    = [_vp, ctypes.c_int, ctypes.c_void_p]
gdi32.SwapBuffers.restype        = ctypes.c_bool
gdi32.SwapBuffers.argtypes       = [_vp]

opengl32.wglCreateContext.restype  = _vp
opengl32.wglCreateContext.argtypes = [_vp]
opengl32.wglMakeCurrent.restype    = ctypes.c_bool
opengl32.wglMakeCurrent.argtypes   = [_vp, _vp]
opengl32.wglDeleteContext.restype  = ctypes.c_bool
opengl32.wglDeleteContext.argtypes = [_vp]

kernel32.GetCurrentThread.restype = _vp
kernel32.SetThreadPriority.restype = ctypes.c_bool
kernel32.SetThreadPriority.argtypes = [_vp, ctypes.c_int]

WDA_EXCLUDEFROMCAPTURE = 0x00000011
THREAD_PRIORITY_BELOW_NORMAL = -1

PFD_DRAW_TO_WINDOW = 0x00000004
PFD_SUPPORT_OPENGL = 0x00000020
PFD_DOUBLEBUFFER   = 0x00000001
PFD_TYPE_RGBA      = 0

GL_BGRA_EXT = 0x80E1

CLASS_NAME = "FPSMagOverlayGL"
_registered = False


def _set_dpi_awareness():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        try:
            user32.SetProcessDPIAware()
        except Exception:
            pass


_set_dpi_awareness()


# ─── GLSL Shaders ────────────────────────────────────────────────────────────

VERTEX_SHADER = """
#version 130
in vec2 position;
in vec2 texcoord;
out vec2 v_uv;

void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    v_uv = texcoord;
}
"""

# Catmull-Rom bicubic using 4 bilinear taps (instead of 16 nearest taps).
# GL_LINEAR does the 2×2 averaging per tap for free, so we get 4×4 = 16
# texel coverage with only 4 texture fetches.
BICUBIC_FRAGMENT_SHADER = """
#version 130
in vec2 v_uv;
out vec4 fragColor;

uniform sampler2D tex;
uniform vec2 texSize;      // (width, height) in pixels

// Catmull-Rom weight function for 4 texels along one axis.
// Returns weights for texels at: floor(x)-1, floor(x), floor(x)+1, floor(x)+2
// where x is the sub-texel coordinate.
vec4 cubic_weights(float t) {
    // t is the fractional part (0..1) between the two center texels
    float t2 = t * t;
    float t3 = t2 * t;
    // Catmull-Rom basis (alpha = 0.5)
    //   w0 = -0.5t³ + t² - 0.5t
    //   w1 =  1.5t³ - 2.5t² + 1
    //   w2 = -1.5t³ + 2t² + 0.5t
    //   w3 =  0.5t³ - 0.5t²
    vec4 w;
    w.x = -0.5 * t3 + t2 - 0.5 * t;
    w.y =  1.5 * t3 - 2.5 * t2 + 1.0;
    w.z = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    w.w =  0.5 * t3 - 0.5 * t2;
    return w;
}

vec4 textureBicubic(sampler2D s, vec2 uv) {
    vec2 pixCoord = uv * texSize - 0.5;  // continuous pixel coordinate
    vec2 f = fract(pixCoord);             // fractional part
    vec2 p = floor(pixCoord) + 0.5;       // center of nearest texel

    // Catmull-Rom weights for x and y
    vec4 wx = cubic_weights(f.x);
    vec4 wy = cubic_weights(f.y);

    // Combine pairs of weights for the bilinear tap trick:
    //   tap0 uses texels (floor-1, floor) with combined weight w0+w1
    //   tap1 uses texels (floor+1, floor+2) with combined weight w2+w3
    // Offset within each pair is w1/(w0+w1) and w3/(w2+w3)
    vec2 w01 = vec2(wx.x + wx.y, wy.x + wy.y);
    vec2 w23 = vec2(wx.z + wx.w, wy.z + wy.w);

    // Bilinear sampling offsets (in texels, relative to `p`)
    vec2 s01 = vec2(wx.y, wy.y) / w01 - 1.0;
    vec2 s23 = vec2(wx.w, wy.w) / w23 + 1.0;

    // Convert to UV
    vec2 uv01 = (p + s01) / texSize;
    vec2 uv23 = (p + s23) / texSize;

    // 4 bilinear taps covering 4×4 texels
    vec4 c00 = texture(s, vec2(uv01.x, uv01.y)) * w01.x * w01.y;
    vec4 c10 = texture(s, vec2(uv23.x, uv01.y)) * w23.x * w01.y;
    vec4 c01 = texture(s, vec2(uv01.x, uv23.y)) * w01.x * w23.y;
    vec4 c11 = texture(s, vec2(uv23.x, uv23.y)) * w23.x * w23.y;

    return c00 + c10 + c01 + c11;
}

void main() {
    fragColor = textureBicubic(tex, v_uv);
}
"""

# Simple passthrough for bilinear/nearest (texture filtering does the work)
PASSTHROUGH_FRAGMENT_SHADER = """
#version 130
in vec2 v_uv;
out vec4 fragColor;

uniform sampler2D tex;

void main() {
    fragColor = texture(tex, v_uv);
}
"""


# ─── Structures ──────────────────────────────────────────────────────────────

class PIXELFORMATDESCRIPTOR(ctypes.Structure):
    _fields_ = [
        ("nSize",           ctypes.c_ushort),
        ("nVersion",        ctypes.c_ushort),
        ("dwFlags",         ctypes.c_ulong),
        ("iPixelType",      ctypes.c_ubyte),
        ("cColorBits",      ctypes.c_ubyte),
        ("cRedBits",        ctypes.c_ubyte),
        ("cRedShift",       ctypes.c_ubyte),
        ("cGreenBits",      ctypes.c_ubyte),
        ("cGreenShift",     ctypes.c_ubyte),
        ("cBlueBits",       ctypes.c_ubyte),
        ("cBlueShift",      ctypes.c_ubyte),
        ("cAlphaBits",      ctypes.c_ubyte),
        ("cAlphaShift",     ctypes.c_ubyte),
        ("cAccumBits",      ctypes.c_ubyte),
        ("cAccumRedBits",   ctypes.c_ubyte),
        ("cAccumGreenBits", ctypes.c_ubyte),
        ("cAccumBlueBits",  ctypes.c_ubyte),
        ("cAccumAlphaBits", ctypes.c_ubyte),
        ("cDepthBits",      ctypes.c_ubyte),
        ("cStencilBits",    ctypes.c_ubyte),
        ("cAuxBuffers",     ctypes.c_ubyte),
        ("iLayerType",      ctypes.c_ubyte),
        ("bReserved",       ctypes.c_ubyte),
        ("dwLayerMask",     ctypes.c_ulong),
        ("dwVisibleMask",   ctypes.c_ulong),
        ("dwDamageMask",    ctypes.c_ulong),
    ]


# ─── WndProc ────────────────────────────────────────────────────────────────

def _wndproc(hwnd, msg, wp, lp):
    if msg == win32con.WM_NCHITTEST:
        return -1
    if msg == win32con.WM_SETCURSOR:
        return 1
    if msg == win32con.WM_ERASEBKGND:
        return 1
    if msg == win32con.WM_PAINT:
        win32gui.ValidateRect(hwnd, None)
        return 0
    if msg == win32con.WM_DESTROY:
        win32gui.PostQuitMessage(0)
        return 0
    return win32gui.DefWindowProc(hwnd, msg, wp, lp)


# ─── Shader helpers ──────────────────────────────────────────────────────────

def _compile_shader(source, shader_type):
    shader = glCreateShader(shader_type)
    glShaderSource(shader, source)
    glCompileShader(shader)
    if glGetShaderiv(shader, GL_COMPILE_STATUS) != GL_TRUE:
        log = glGetShaderInfoLog(shader).decode()
        raise RuntimeError(f"Shader compile error:\n{log}")
    return shader

def _link_program(vert, frag):
    prog = glCreateProgram()
    glAttachShader(prog, vert)
    glAttachShader(prog, frag)
    glLinkProgram(prog)
    if glGetProgramiv(prog, GL_LINK_STATUS) != GL_TRUE:
        log = glGetProgramInfoLog(prog).decode()
        raise RuntimeError(f"Program link error:\n{log}")
    glDeleteShader(vert)
    glDeleteShader(frag)
    return prog


# ─── OpenGL Overlay Window ──────────────────────────────────────────────────

class OverlayWindow:
    def __init__(self, x, y, w, h):
        global _registered
        hinst = win32api.GetModuleHandle(None)

        if not _registered:
            wc = win32gui.WNDCLASS()
            wc.style = win32con.CS_OWNDC
            wc.lpfnWndProc = _wndproc
            wc.hInstance = hinst
            wc.hCursor = 0
            wc.hbrBackground = win32gui.GetStockObject(win32con.BLACK_BRUSH)
            wc.lpszClassName = CLASS_NAME
            win32gui.RegisterClass(wc)
            _registered = True

        ex = (0x00000008 |   # WS_EX_TOPMOST
              0x00000080 |   # WS_EX_TOOLWINDOW
              0x08000000)    # WS_EX_NOACTIVATE

        self.hwnd = win32gui.CreateWindowEx(
            ex, CLASS_NAME, "Magnifier", 0x80000000,
            x, y, w, h, 0, 0, hinst, None
        )

        if config.EXCLUDE_FROM_CAPTURE:
            self.exclude_ok = bool(
                user32.SetWindowDisplayAffinity(self.hwnd, WDA_EXCLUDEFROMCAPTURE)
            )
        else:
            self.exclude_ok = False
        if config.EXCLUDE_FROM_CAPTURE and not self.exclude_ok:
            print("  WDA_EXCLUDEFROMCAPTURE unavailable; using layered-window filtering")

        # ── OpenGL context ───────────────────────────────────────────────
        self.hdc = user32.GetDC(self.hwnd)

        pfd = PIXELFORMATDESCRIPTOR()
        pfd.nSize = ctypes.sizeof(PIXELFORMATDESCRIPTOR)
        pfd.nVersion = 1
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER
        pfd.iPixelType = PFD_TYPE_RGBA
        pfd.cColorBits = 32

        fmt = gdi32.ChoosePixelFormat(self.hdc, ctypes.byref(pfd))
        if fmt == 0:
            raise RuntimeError("ChoosePixelFormat failed")
        if not gdi32.SetPixelFormat(self.hdc, fmt, ctypes.byref(pfd)):
            raise RuntimeError("SetPixelFormat failed")

        self.hglrc = opengl32.wglCreateContext(self.hdc)
        if not self.hglrc:
            raise RuntimeError("wglCreateContext failed")
        if not opengl32.wglMakeCurrent(self.hdc, self.hglrc):
            raise RuntimeError("wglMakeCurrent failed")

        # ── Retrofit layered + transparent for click-through ─────────────
        GWL_EXSTYLE        = -20
        WS_EX_LAYERED      = 0x00080000
        WS_EX_TRANSPARENT  = 0x00000020
        LWA_ALPHA          = 0x02

        old_ex = user32.GetWindowLongW(self.hwnd, GWL_EXSTYLE)
        user32.SetWindowLongW(
            self.hwnd, GWL_EXSTYLE,
            old_ex | WS_EX_LAYERED | WS_EX_TRANSPARENT
        )
        user32.SetLayeredWindowAttributes(self.hwnd, 0, 255, LWA_ALPHA)

        # Texture — always GL_LINEAR for bicubic (the shader relies on it)
        self.tex = glGenTextures(1)
        glBindTexture(GL_TEXTURE_2D, self.tex)

        if config.GPU_FILTER == "nearest":
            gl_filter = GL_NEAREST
        else:
            # Both "linear" and "bicubic" need GL_LINEAR on the texture
            gl_filter = GL_LINEAR

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE)

        self._tex_w = 0
        self._tex_h = 0
        self.w = w
        self.h = h
        glViewport(0, 0, self.w, self.h)

        # ── Compile shaders ──────────────────────────────────────────────
        vert = _compile_shader(VERTEX_SHADER, GL_VERTEX_SHADER)

        if config.GPU_FILTER == "bicubic":
            frag = _compile_shader(BICUBIC_FRAGMENT_SHADER, GL_FRAGMENT_SHADER)
        else:
            frag = _compile_shader(PASSTHROUGH_FRAGMENT_SHADER, GL_FRAGMENT_SHADER)

        self.program = _link_program(vert, frag)

        # Uniform locations
        u_tex          = glGetUniformLocation(self.program, "tex")
        self.u_texSize = glGetUniformLocation(self.program, "texSize")

        glUseProgram(self.program)
        glUniform1i(u_tex, 0)
        glUseProgram(0)

        # Attribute locations
        self.a_position = glGetAttribLocation(self.program, "position")
        self.a_texcoord = glGetAttribLocation(self.program, "texcoord")

        # ── Fullscreen quad VAO/VBO ──────────────────────────────────────
        #   position (x,y)  texcoord (u,v)
        import numpy as np
        quad = np.array([
            # pos        uv
            -1, -1,      0, 1,      # bottom-left
            1, -1,      1, 1,      # bottom-right
            1,  1,      1, 0,      # top-right
            -1,  1,      0, 0,      # top-left
        ], dtype=np.float32)

        indices = np.array([0, 1, 2, 0, 2, 3], dtype=np.uint32)

        self.vao = glGenVertexArrays(1)
        glBindVertexArray(self.vao)

        vbo = glGenBuffers(1)
        glBindBuffer(GL_ARRAY_BUFFER, vbo)
        glBufferData(GL_ARRAY_BUFFER, quad.nbytes, quad, GL_STATIC_DRAW)

        ebo = glGenBuffers(1)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo)
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.nbytes, indices, GL_STATIC_DRAW)

        stride = 4 * 4  # 4 floats × 4 bytes
        # position attribute
        glEnableVertexAttribArray(self.a_position)
        glVertexAttribPointer(
            self.a_position,
            2,
            GL_FLOAT,
            GL_FALSE,
            stride,
            ctypes.c_void_p(0),
        )
        # texcoord attribute
        glEnableVertexAttribArray(self.a_texcoord)
        glVertexAttribPointer(
            self.a_texcoord,
            2,
            GL_FLOAT,
            GL_FALSE,
            stride,
            ctypes.c_void_p(8),
        )

        glBindVertexArray(0)

        self.visible = False
        self.topmost()

    # ── window management ────────────────────────────────────────────────

    def topmost(self):
        win32gui.SetWindowPos(
            self.hwnd, win32con.HWND_TOPMOST, 0, 0, 0, 0,
            win32con.SWP_NOMOVE | win32con.SWP_NOSIZE |
            win32con.SWP_NOACTIVATE | win32con.SWP_SHOWWINDOW
        )
        self.visible = True

    def hide(self):
        if not self.visible:
            return
        win32gui.SetWindowPos(
            self.hwnd, win32con.HWND_TOPMOST,
            -32000, -32000, 1, 1,
            win32con.SWP_NOACTIVATE
        )
        self.visible = False

    def move(self, x, y, w, h):
        self.w, self.h = w, h
        glViewport(0, 0, self.w, self.h)
        win32gui.SetWindowPos(
            self.hwnd, win32con.HWND_TOPMOST, x, y, w, h,
            win32con.SWP_NOACTIVATE | win32con.SWP_SHOWWINDOW
        )
        self.visible = True

    # ── rendering ────────────────────────────────────────────────────────

    def render(self, raw_bgra, cap_w, cap_h):
        # Upload texture
        glBindTexture(GL_TEXTURE_2D, self.tex)
        texture_resized = cap_w != self._tex_w or cap_h != self._tex_h
        if texture_resized:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cap_w, cap_h, 0,
                         GL_BGRA_EXT, GL_UNSIGNED_BYTE, raw_bgra)
            self._tex_w = cap_w
            self._tex_h = cap_h
        else:
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cap_w, cap_h,
                            GL_BGRA_EXT, GL_UNSIGNED_BYTE, raw_bgra)

        # Draw fullscreen quad with shader
        glUseProgram(self.program)
        if texture_resized and self.u_texSize >= 0:
            glUniform2f(self.u_texSize, float(cap_w), float(cap_h))

        glBindVertexArray(self.vao)
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, None)
        glBindVertexArray(0)
        glUseProgram(0)

        # ── Decorations (fixed-function overlay) ─────────────────────────
        # Switch to legacy mode for simple line drawing
        glMatrixMode(GL_PROJECTION)
        glLoadIdentity()
        glMatrixMode(GL_MODELVIEW)
        glLoadIdentity()

        px = 2.0 / self.w
        py = 2.0 / self.h

        # Border
        if config.BORDER_PX > 0:
            r, g, b = config.BORDER_COLOR
            glColor3f(r, g, b)
            glLineWidth(config.BORDER_PX)
            inset_x = config.BORDER_PX * 0.5 * px
            inset_y = config.BORDER_PX * 0.5 * py
            glBegin(GL_LINE_LOOP)
            glVertex2f(-1 + inset_x, -1 + inset_y)
            glVertex2f( 1 - inset_x, -1 + inset_y)
            glVertex2f( 1 - inset_x,  1 - inset_y)
            glVertex2f(-1 + inset_x,  1 - inset_y)
            glEnd()

        gdi32.SwapBuffers(self.hdc)

    def destroy(self):
        opengl32.wglMakeCurrent(self.hdc, None)
        opengl32.wglDeleteContext(self.hglrc)
        user32.ReleaseDC(self.hwnd, self.hdc)
        win32gui.DestroyWindow(self.hwnd)


# ─── MAGNIFIER ───────────────────────────────────────────────────────────────

class Magnifier:
    def __init__(self):
        self.on = False
        self.alive = True
        self.zoom = config.ZOOM
        self.radius = config.CAPTURE_RADIUS
        self.fps = config.FPS
        self.win = None
        self._last_topmost = 0
        self._last_size = 0
        self._needs_reposition = False
        self._rebuild_overlay = False
        self._needs_monitor_update = True
        self.hotkeys_enabled = True

        self._hooks = {}
        self.sct = None
        self.monitor_index = config.MONITOR_INDEX

    def _update_monitor_geometry(self):
        if self.sct is None:
            return

        monitors = self.sct.monitors
        if self.monitor_index >= len(monitors):
            self.monitor_index = 1

        m = monitors[self.monitor_index]
        self.screen_left = int(m["left"])
        self.screen_top = int(m["top"])
        width = int(m["width"])
        height = int(m["height"])
        self.screen_width = width
        self.screen_height = height
        self.screen_right = self.screen_left + width
        self.screen_bottom = self.screen_top + height
        self.cx = self.screen_left + width // 2
        self.cy = self.screen_top + height // 2

    @property
    def capture_radius(self):
        """Capture only the source pixels that can actually be visible."""
        max_visible_radius = int(
            min(self.screen_width, self.screen_height) / (2 * self.zoom)
        )
        return min(self.radius, max(1, max_visible_radius))

    @property
    def size(self):
        return int(self.capture_radius * 2 * self.zoom)

    def overlay_rect(self):
        sz = self.size
        return self.cx - sz // 2, self.cy - sz // 2, sz

    def grab(self):
        if self.sct is None:
            raise RuntimeError("Screen capture is not initialized")

        r = self.capture_radius
        left = max(self.screen_left, min(self.cx - r, self.screen_right - r * 2))
        top = max(self.screen_top, min(self.cy - r, self.screen_bottom - r * 2))
        region = {
            "left": left, "top": top,
            "width": r * 2, "height": r * 2,
        }
        return self.sct.grab(region)

    def ensure_window(self):
        ox, oy, sz = self.overlay_rect()
        if self.win is None:
            self.win = OverlayWindow(ox, oy, sz, sz)
            self._last_size = sz
        elif sz != self._last_size or self._needs_reposition:
            self.win.move(ox, oy, sz, sz)
            self._last_size = sz
        self._needs_reposition = False

    def _destroy_overlay(self):
        if self.win is None:
            return
        self.win.destroy()
        self.win = None
        self._last_size = 0

    def set_filter(self, value):
        if value == config.GPU_FILTER:
            return
        config.GPU_FILTER = value
        self._rebuild_overlay = True

    def set_fps(self, value):
        self.fps = max(1, int(value))

    # ── hotkey management ────────────────────────────────────────────────

    def _bind_key(self, name, key, callback):
        current_hook = self._hooks.get(name)
        if current_hook is not None:
            keyboard.unhook(current_hook)
        self._hooks[name] = keyboard.on_press_key(key, callback, suppress=False)

    def _hotkey_callbacks(self):
        return {
            "toggle": (config.TOGGLE_KEY, lambda _: self.toggle()),
            "zoom_in": (
                config.ZOOM_IN_KEY,
                lambda _: self.adj_zoom(config.ZOOM_STEP),
            ),
            "zoom_out": (
                config.ZOOM_OUT_KEY,
                lambda _: self.adj_zoom(-config.ZOOM_STEP),
            ),
            "region_up": (
                config.REGION_UP_KEY,
                lambda _: self.adj_radius(config.CAPTURE_STEP),
            ),
            "region_down": (
                config.REGION_DOWN_KEY,
                lambda _: self.adj_radius(-config.CAPTURE_STEP),
            ),
        }

    def _bind_hotkeys(self):
        for name, (key, callback) in self._hotkey_callbacks().items():
            self._bind_key(name, key, callback)

    def _unbind_hotkeys(self):
        for name, hook in self._hooks.items():
            if hook is not None:
                keyboard.unhook(hook)
                self._hooks[name] = None

    def set_hotkeys_enabled(self, enabled):
        enabled = bool(enabled)
        if enabled == self.hotkeys_enabled:
            return

        self.hotkeys_enabled = enabled
        if enabled:
            self._bind_hotkeys()
            print("  Hotkeys enabled")
        else:
            self._unbind_hotkeys()
            print("  Hotkeys disabled")

    def rebind_key(self, name, new_key):
        callbacks = self._hotkey_callbacks()
        if self.hotkeys_enabled and name in callbacks:
            self._bind_key(name, new_key, callbacks[name][1])

    # ── main loop ────────────────────────────────────────────────────────

    def run(self):
        self.sct = mss.MSS()
        try:
            if config.LOW_PRIORITY:
                kernel32.SetThreadPriority(
                    kernel32.GetCurrentThread(),
                    THREAD_PRIORITY_BELOW_NORMAL,
                )

            self._update_monitor_geometry()
            self._needs_monitor_update = False

            if self.hotkeys_enabled:
                self._bind_hotkeys()

            print("+-------------------------------------------+")
            print("|           FPS Screen Magnifier            |")
            print("+-------------------------------------------+")
            print(f"|  Toggle:   {config.TOGGLE_KEY:<30s} |")
            print(f"|  Zoom:     +/-  ({self.zoom:.1f}x)                    |")
            print(f"|  Region:   [/]  ({self.radius*2}px)                  |")
            print(f"|  Filter:   {config.GPU_FILTER:<30s} |")
            print(f"|  FPS:      {self.fps:<30d} |")
            print("+-------------------------------------------+")
            print("|  Game must be Borderless Windowed         |")
            print("+-------------------------------------------+")
            print()

            while self.alive:
                t0 = time.perf_counter()

                if self._rebuild_overlay:
                    self._destroy_overlay()
                    self._rebuild_overlay = False
                    self._needs_reposition = True

                if self.on:
                    if self._needs_monitor_update:
                        self._update_monitor_geometry()
                        self._needs_monitor_update = False
                        self._needs_reposition = True

                    self.ensure_window()

                    now = time.perf_counter() * 1000
                    if now - self._last_topmost > config.TOPMOST_MS:
                        self.win.topmost()
                        self._last_topmost = now

                    shot = self.grab()
                    self.win.render(shot.raw, shot.width, shot.height)
                elif self.win is not None:
                    self.win.hide()

                win32gui.PumpWaitingMessages()

                target_fps = self.fps if self.on else config.IDLE_FPS
                dt = 1.0 / target_fps
                elapsed = time.perf_counter() - t0
                if elapsed < dt:
                    time.sleep(dt - elapsed)
        finally:
            self.on = False
            self.alive = False
            self._destroy_overlay()
            self._unbind_hotkeys()
            self.sct.close()
            self.sct = None

    # ── callbacks ────────────────────────────────────────────────────────

    def toggle(self):
        self.on = not self.on
        if self.on:
            self._needs_monitor_update = True
            self._needs_reposition = True
        s = "ON " if self.on else "OFF"
        print(f"  > Magnifier {s}  |  {self.zoom:.1f}x  |  {self.radius*2}px")

    def adj_zoom(self, d):
        if not self.on:
            return
        self.zoom = round(
            max(config.ZOOM_MIN, min(config.ZOOM_MAX, self.zoom + d)),
            2,
        )
        print(f"    zoom -> {self.zoom:.1f}x")

    def adj_radius(self, d):
        if not self.on:
            return
        self.radius = max(config.CAPTURE_MIN, min(config.CAPTURE_MAX, self.radius + d))
        print(f"    region -> {self.radius*2}px")

    def quit(self):
        print("  Shutting down...")
        self.alive = False


# ─── MAIN ────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    Magnifier().run()
