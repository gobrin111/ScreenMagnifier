#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr wchar_t kWindowClass[] = L"FPSMagnifierDwmPrototype";
constexpr int kToggleHotkeyId = 1;
constexpr int kQuitHotkeyId = 2;

bool g_running = true;
bool g_visible = true;

struct Options {
    UINT radius = 200;
    float zoom = 2.0F;
    double pick_delay_seconds = 3.0;
    double duration_seconds = 0.0;
};

std::string Hex(HRESULT result) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase
           << static_cast<std::uint32_t>(result);
    return stream.str();
}

void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed (" +
                                 Hex(result) + ")");
    }
}

void EnablePhysicalPixelCoordinates() {
    using SetDpiContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto user32 = GetModuleHandleW(L"user32.dll");
    const auto set_context = reinterpret_cast<SetDpiContext>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_context != nullptr &&
        set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return;
    }
    SetProcessDPIAware();
}

void PrintUsage() {
    std::wcout
        << L"DWM Window Magnifier Prototype\n\n"
        << L"Usage:\n"
        << L"  DwmMagnifierPrototype.exe [--pick-delay SECONDS]\n"
        << L"      [--radius PX] [--zoom N] [--duration SECONDS]\n\n"
        << L"After launch, focus the game window before the pick delay expires.\n"
        << L"Defaults: --pick-delay 3 --radius 200 --zoom 2.0\n"
        << L"Hotkeys: F8 toggles the overlay; F9 exits.\n";
}

Options ParseOptions(int argument_count, wchar_t** arguments) {
    Options options;
    for (int index = 1; index < argument_count; ++index) {
        const std::wstring argument = arguments[index];
        if (argument == L"--help" || argument == L"-h") {
            PrintUsage();
            std::wcout.flush();
            ExitProcess(0);
        }
        if (index + 1 >= argument_count) {
            throw std::runtime_error("Missing value after command-line option");
        }

        const std::wstring value = arguments[++index];
        wchar_t* end = nullptr;
        if (argument == L"--radius") {
            const unsigned long parsed = wcstoul(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != L'\0' || parsed < 16 ||
                parsed > 8192) {
                throw std::runtime_error(
                    "--radius must be between 16 and 8192 pixels");
            }
            options.radius = static_cast<UINT>(parsed);
        } else if (argument == L"--zoom") {
            const double parsed = wcstod(value.c_str(), &end);
            if (end == value.c_str() || *end != L'\0' || !std::isfinite(parsed) ||
                parsed < 1.0 || parsed > 10.0) {
                throw std::runtime_error("--zoom must be between 1.0 and 10.0");
            }
            options.zoom = static_cast<float>(parsed);
        } else if (argument == L"--pick-delay") {
            const double parsed = wcstod(value.c_str(), &end);
            if (end == value.c_str() || *end != L'\0' || !std::isfinite(parsed) ||
                parsed < 0.0 || parsed > 30.0) {
                throw std::runtime_error(
                    "--pick-delay must be between 0 and 30 seconds");
            }
            options.pick_delay_seconds = parsed;
        } else if (argument == L"--duration") {
            const double parsed = wcstod(value.c_str(), &end);
            if (end == value.c_str() || *end != L'\0' || !std::isfinite(parsed) ||
                parsed < 0.25 || parsed > 3600.0) {
                throw std::runtime_error(
                    "--duration must be between 0.25 and 3600 seconds");
            }
            options.duration_seconds = parsed;
        } else {
            throw std::runtime_error("Unknown command-line option");
        }
    }
    return options;
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam,
                                 LPARAM lparam) {
    switch (message) {
        case WM_HOTKEY:
            if (wparam == kToggleHotkeyId) {
                g_visible = !g_visible;
                ShowWindow(window, g_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
                return 0;
            }
            if (wparam == kQuitHotkeyId) {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateOverlayWindow(HINSTANCE instance, const RECT& monitor_rect,
                         UINT overlay_size) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground =
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        Check(HRESULT_FROM_WIN32(GetLastError()), "RegisterClassExW");
    }

    const LONG monitor_width = monitor_rect.right - monitor_rect.left;
    const LONG monitor_height = monitor_rect.bottom - monitor_rect.top;
    const int left = monitor_rect.left +
                     (monitor_width - static_cast<LONG>(overlay_size)) / 2;
    const int top = monitor_rect.top +
                    (monitor_height - static_cast<LONG>(overlay_size)) / 2;
    const DWORD extended_style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                 WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    HWND window = CreateWindowExW(
        extended_style, kWindowClass, L"DWM Magnifier Prototype", WS_POPUP,
        left, top, static_cast<int>(overlay_size),
        static_cast<int>(overlay_size), nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        Check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW");
    }

    if (!RegisterHotKey(window, kToggleHotkeyId, MOD_NOREPEAT, VK_F8)) {
        std::wcerr << L"Warning: F8 is already registered by another app.\n";
    }
    if (!RegisterHotKey(window, kQuitHotkeyId, MOD_NOREPEAT, VK_F9)) {
        std::wcerr << L"Warning: F9 is already registered by another app.\n";
    }
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    return window;
}

std::wstring WindowTitle(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring title(
        static_cast<std::size_t>(std::max(0, length)) + 1,
        L'\0');
    if (length > 0) {
        GetWindowTextW(window, title.data(), length + 1);
    }
    title.resize(static_cast<std::size_t>(std::max(0, length)));
    return title.empty() ? L"<untitled>" : title;
}

}  // namespace

int wmain(int argument_count, wchar_t** arguments) {
    HTHUMBNAIL thumbnail = nullptr;
    HWND overlay = nullptr;
    try {
        EnablePhysicalPixelCoordinates();
        const Options options = ParseOptions(argument_count, arguments);

        std::wcout << L"Focus the game window now. Selecting it in "
                   << options.pick_delay_seconds << L" seconds...\n";
        std::wcout.flush();
        std::this_thread::sleep_for(
            std::chrono::duration<double>(options.pick_delay_seconds));

        const HWND source = GetForegroundWindow();
        if (source == nullptr || !IsWindow(source) || IsIconic(source)) {
            throw std::runtime_error(
                "The selected foreground window is unavailable or minimized");
        }
        DWORD source_process = 0;
        GetWindowThreadProcessId(source, &source_process);
        if (source_process == GetCurrentProcessId()) {
            throw std::runtime_error("The prototype cannot magnify itself");
        }

        RECT source_client{};
        if (!GetClientRect(source, &source_client)) {
            Check(HRESULT_FROM_WIN32(GetLastError()), "GetClientRect(source)");
        }
        const UINT source_width =
            static_cast<UINT>(source_client.right - source_client.left);
        const UINT source_height =
            static_cast<UINT>(source_client.bottom - source_client.top);
        const UINT max_radius = std::min(source_width, source_height) / 2;
        const UINT radius = std::min(options.radius, max_radius);
        if (radius < 16) {
            throw std::runtime_error("The selected window is too small");
        }
        const UINT diameter = radius * 2;
        const UINT overlay_size = static_cast<UINT>(std::lround(
            static_cast<double>(diameter) * options.zoom));

        const HMONITOR monitor =
            MonitorFromWindow(source, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        if (!GetMonitorInfoW(monitor, &monitor_info)) {
            Check(HRESULT_FROM_WIN32(GetLastError()), "GetMonitorInfoW");
        }

        overlay = CreateOverlayWindow(GetModuleHandleW(nullptr),
                                      monitor_info.rcMonitor, overlay_size);
        Check(DwmRegisterThumbnail(overlay, source, &thumbnail),
              "DwmRegisterThumbnail");

        const LONG source_left =
            (static_cast<LONG>(source_width) - static_cast<LONG>(diameter)) / 2;
        const LONG source_top =
            (static_cast<LONG>(source_height) - static_cast<LONG>(diameter)) / 2;
        DWM_THUMBNAIL_PROPERTIES properties{};
        properties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE |
                             DWM_TNP_OPACITY | DWM_TNP_VISIBLE |
                             DWM_TNP_SOURCECLIENTAREAONLY;
        properties.rcDestination = {
            0,
            0,
            static_cast<LONG>(overlay_size),
            static_cast<LONG>(overlay_size),
        };
        properties.rcSource = {
            source_left,
            source_top,
            source_left + static_cast<LONG>(diameter),
            source_top + static_cast<LONG>(diameter),
        };
        properties.opacity = 255;
        properties.fVisible = TRUE;
        properties.fSourceClientAreaOnly = TRUE;
        Check(DwmUpdateThumbnailProperties(thumbnail, &properties),
              "DwmUpdateThumbnailProperties");

        std::wcout << L"Magnifying: " << WindowTitle(source) << L"\n"
                   << L"Source crop: " << diameter << L"x" << diameter
                   << L"; overlay: " << overlay_size << L"x" << overlay_size
                   << L"; zoom: " << options.zoom << L"x\n"
                   << L"F8 toggles the overlay. F9 exits.\n";

        const auto run_start = std::chrono::steady_clock::now();
        MSG message{};
        while (g_running) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    g_running = false;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!g_running) {
                break;
            }
            if (!IsWindow(source)) {
                throw std::runtime_error("The selected source window closed");
            }
            if (options.duration_seconds > 0.0 &&
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - run_start)
                        .count() >= options.duration_seconds) {
                DestroyWindow(overlay);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (thumbnail != nullptr) {
            DwmUnregisterThumbnail(thumbnail);
            thumbnail = nullptr;
        }
        if (overlay != nullptr && IsWindow(overlay)) {
            DestroyWindow(overlay);
        }
        return 0;
    } catch (const std::exception& error) {
        if (thumbnail != nullptr) {
            DwmUnregisterThumbnail(thumbnail);
        }
        if (overlay != nullptr && IsWindow(overlay)) {
            DestroyWindow(overlay);
        }
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
