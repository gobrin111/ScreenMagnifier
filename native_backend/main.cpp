#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"FPSMagnifierGpuPrototype";
constexpr int kToggleHotkeyId = 1;
constexpr int kQuitHotkeyId = 2;

struct Options {
    UINT monitor = 1;
    UINT radius = 200;
    float zoom = 2.0F;
    double duration_seconds = 0.0;
    bool vsync_present = true;
    bool stats = false;
    bool list_monitors = false;
};

struct OutputInfo {
    UINT number = 0;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC description{};
};

bool g_running = true;
bool g_visible = true;

struct WindowCompositionAttributeData {
    int attribute;
    void* data;
    SIZE_T data_size;
};

using SetWindowCompositionAttributeFunction =
    BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

bool ExcludeWindowFromDesktopDuplication(HWND window) {
    constexpr int kExcludedFromDesktopDuplication = 24;
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto set_composition_attribute =
        reinterpret_cast<SetWindowCompositionAttributeFunction>(
            GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (set_composition_attribute == nullptr) {
        return false;
    }

    BOOL enabled = TRUE;
    WindowCompositionAttributeData data{
        kExcludedFromDesktopDuplication,
        &enabled,
        sizeof(enabled),
    };
    return set_composition_attribute(window, &data) != FALSE;
}

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

std::vector<OutputInfo> EnumerateOutputs() {
    ComPtr<IDXGIFactory1> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    std::vector<OutputInfo> outputs;
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapter_result =
            factory->EnumAdapters1(adapter_index, &adapter);
        if (adapter_result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        Check(adapter_result, "EnumAdapters1");

        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            const HRESULT output_result =
                adapter->EnumOutputs(output_index, &output);
            if (output_result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            Check(output_result, "EnumOutputs");

            DXGI_OUTPUT_DESC description{};
            Check(output->GetDesc(&description), "IDXGIOutput::GetDesc");
            if (!description.AttachedToDesktop) {
                continue;
            }

            outputs.push_back(OutputInfo{
                static_cast<UINT>(outputs.size() + 1),
                adapter,
                output,
                description,
            });
        }
    }
    return outputs;
}

void PrintOutputs(const std::vector<OutputInfo>& outputs) {
    if (outputs.empty()) {
        std::wcout << L"No attached monitors were found.\n";
        return;
    }

    std::wcout << L"Attached monitors:\n";
    for (const auto& output : outputs) {
        const RECT rect = output.description.DesktopCoordinates;
        std::wcout << L"  " << output.number << L": "
                   << output.description.DeviceName << L"  "
                   << (rect.right - rect.left) << L"x"
                   << (rect.bottom - rect.top) << L" at ("
                   << rect.left << L", " << rect.top << L")";
        if (output.description.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
            output.description.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
            std::wcout << L" [rotated; not supported by this prototype]";
        }
        std::wcout << L"\n";
    }
}

void PrintUsage() {
    std::wcout
        << L"GPU Magnifier Prototype\n\n"
        << L"Usage:\n"
        << L"  GpuMagnifierPrototype.exe --list-monitors\n"
        << L"  GpuMagnifierPrototype.exe [--monitor N] [--radius PX] [--zoom N]\n"
        << L"      [--present immediate|vsync] [--stats] [--duration SECONDS]\n\n"
        << L"Defaults: --monitor 1 --radius 200 --zoom 2.0\n"
        << L"          --present vsync; statistics disabled\n"
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
        if (argument == L"--list-monitors") {
            options.list_monitors = true;
            continue;
        }
        if (argument == L"--stats") {
            options.stats = true;
            continue;
        }

        if (index + 1 >= argument_count) {
            throw std::runtime_error("Missing value after command-line option");
        }
        const std::wstring value = arguments[++index];
        wchar_t* end = nullptr;

        if (argument == L"--monitor") {
            const unsigned long parsed = wcstoul(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != L'\0' || parsed == 0 ||
                parsed > 64) {
                throw std::runtime_error("--monitor must be between 1 and 64");
            }
            options.monitor = static_cast<UINT>(parsed);
        } else if (argument == L"--radius") {
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
        } else if (argument == L"--duration") {
            const double parsed = wcstod(value.c_str(), &end);
            if (end == value.c_str() || *end != L'\0' || !std::isfinite(parsed) ||
                parsed < 0.25 || parsed > 3600.0) {
                throw std::runtime_error(
                    "--duration must be between 0.25 and 3600 seconds");
            }
            options.duration_seconds = parsed;
        } else if (argument == L"--present") {
            if (value == L"immediate") {
                options.vsync_present = false;
            } else if (value == L"vsync") {
                options.vsync_present = true;
            } else {
                throw std::runtime_error(
                    "--present must be either immediate or vsync");
            }
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
        case WM_DISPLAYCHANGE:
            std::wcerr
                << L"Display configuration changed; restart the prototype.\n";
            DestroyWindow(window);
            return 0;
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

HWND CreateOverlayWindow(HINSTANCE instance, const RECT& output_rect,
                         UINT overlay_size) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        Check(HRESULT_FROM_WIN32(GetLastError()), "RegisterClassExW");
    }

    const LONG output_width = output_rect.right - output_rect.left;
    const LONG output_height = output_rect.bottom - output_rect.top;
    const int left = output_rect.left +
                     (output_width - static_cast<LONG>(overlay_size)) / 2;
    const int top = output_rect.top +
                    (output_height - static_cast<LONG>(overlay_size)) / 2;

    const DWORD extended_style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                 WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    HWND window = CreateWindowExW(
        extended_style, kWindowClass, L"GPU Magnifier Prototype", WS_POPUP,
        left, top, static_cast<int>(overlay_size),
        static_cast<int>(overlay_size), nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        Check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW");
    }

    // Use the compositor's Desktop Duplication-specific exclusion. General
    // display affinity can substitute a protected black rectangle in DDA,
    // which is exactly where this centered overlay reads its source pixels.
    if (!ExcludeWindowFromDesktopDuplication(window)) {
        DestroyWindow(window);
        throw std::runtime_error(
            "Windows could not exclude the overlay from Desktop Duplication");
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

ComPtr<ID3DBlob> CompileShader(const char* source, const char* entry_point,
                               const char* target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source, strlen(source), nullptr, nullptr, nullptr, entry_point, target,
        flags, 0, &shader, &errors);
    if (FAILED(result)) {
        std::string message = "D3DCompile failed (" + Hex(result) + ")";
        if (errors) {
            message += ": ";
            message.append(static_cast<const char*>(errors->GetBufferPointer()),
                           errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }
    return shader;
}

class GpuMagnifier {
public:
    GpuMagnifier(const OutputInfo& output, HWND window, UINT radius)
        : window_(window), radius_(radius) {
        CreateDevice(output);
        CreateSwapChain(output);
        CreatePipeline();
        CreateDuplicator(output);
    }

    ~GpuMagnifier() {
        if (frame_latency_handle_ != nullptr) {
            CloseHandle(frame_latency_handle_);
        }
    }

    void Run(double duration_seconds, bool vsync_present, bool stats_enabled) {
        MSG message{};
        const auto run_start = std::chrono::steady_clock::now();
        auto last_present = run_start;
        LARGE_INTEGER performance_frequency{};
        QueryPerformanceFrequency(&performance_frequency);
        LONGLONG last_source_present = 0;
        std::uint64_t acquired_events = 0;
        std::uint64_t presented_frames = 0;
        std::uint64_t coalesced_frames = 0;
        std::uint64_t pointer_only_events = 0;
        double maximum_present_gap_ms = 0.0;
        double maximum_source_gap_ms = 0.0;

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
            if (duration_seconds > 0.0 &&
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - run_start)
                        .count() >= duration_seconds) {
                DestroyWindow(window_);
                break;
            }

            // Wait until DXGI has room for the next overlay frame before
            // acquiring source pixels. Capturing first can leave a fresh frame
            // sitting idle until the previous overlay present completes.
            if (g_visible && frame_latency_handle_ != nullptr) {
                const DWORD wait_result =
                    WaitForSingleObjectEx(frame_latency_handle_, 1000, TRUE);
                if (wait_result == WAIT_TIMEOUT || wait_result == WAIT_IO_COMPLETION) {
                    continue;
                }
                if (wait_result != WAIT_OBJECT_0) {
                    throw std::runtime_error(
                        "Waiting for the DXGI presentation slot failed");
                }
            }

            DXGI_OUTDUPL_FRAME_INFO frame_info{};
            ComPtr<IDXGIResource> desktop_resource;
            HRESULT result = duplicator_->AcquireNextFrame(
                100, &frame_info, &desktop_resource);
            if (result == DXGI_ERROR_WAIT_TIMEOUT) {
                continue;
            }
            if (result == DXGI_ERROR_ACCESS_LOST) {
                throw std::runtime_error(
                    "Desktop Duplication access was lost; restart after changing "
                    "display mode or leaving an exclusive-fullscreen game");
            }
            Check(result, "AcquireNextFrame");
            if (stats_enabled) {
                ++acquired_events;
            }

            try {
                // AcquireNextFrame also wakes for pointer-only changes. In that
                // case the desktop texture has not changed, so presenting it
                // again only adds GPU/compositor work and destabilizes pacing.
                const bool desktop_updated =
                    frame_info.LastPresentTime.QuadPart != 0 &&
                    frame_info.AccumulatedFrames != 0;
                if (stats_enabled && desktop_updated) {
                    if (frame_info.AccumulatedFrames > 1) {
                        coalesced_frames += frame_info.AccumulatedFrames - 1;
                    }
                    if (last_source_present != 0 &&
                        performance_frequency.QuadPart > 0) {
                        maximum_source_gap_ms = std::max(
                            maximum_source_gap_ms,
                            1000.0 * static_cast<double>(
                                         frame_info.LastPresentTime.QuadPart -
                                         last_source_present) /
                                static_cast<double>(
                                    performance_frequency.QuadPart));
                    }
                    last_source_present = frame_info.LastPresentTime.QuadPart;
                }
                if (!desktop_updated && stats_enabled) {
                    ++pointer_only_events;
                }

                // The resource returned for a pointer-only wake still represents
                // the current desktop surface. Always use it: the first such
                // resource is not guaranteed to be a previously initialized
                // frame, so skipping it can leave the overlay permanently black.
                ComPtr<ID3D11Texture2D> desktop_texture;
                Check(desktop_resource.As(&desktop_texture),
                      "QueryInterface(ID3D11Texture2D)");
                CopyCenterCrop(desktop_texture.Get());

                if (g_visible) {
                    Render(vsync_present);
                    ++presented_frames;
                    if (stats_enabled) {
                        const auto present_time =
                            std::chrono::steady_clock::now();
                        maximum_present_gap_ms = std::max(
                            maximum_present_gap_ms,
                            std::chrono::duration<double, std::milli>(
                                present_time - last_present)
                                .count());
                        last_present = present_time;
                    }
                }
            } catch (...) {
                duplicator_->ReleaseFrame();
                throw;
            }

            // Keep ownership through rendering, then release immediately before
            // the next acquire. This minimizes redundant desktop update work.
            Check(duplicator_->ReleaseFrame(), "ReleaseFrame");
        }

        if (stats_enabled) {
            const double seconds = std::chrono::duration<double>(
                                       std::chrono::steady_clock::now() - run_start)
                                       .count();
            std::wcout << std::fixed << std::setprecision(1)
                       << L"Run statistics: " << acquired_events
                       << L" acquire events; "
                       << (static_cast<double>(presented_frames) / seconds)
                       << L" desktop frames/sec; max source gap "
                       << maximum_source_gap_ms << L" ms; max present gap "
                       << maximum_present_gap_ms
                       << L" ms; coalesced source frames " << coalesced_frames
                       << L"; ignored pointer-only events "
                       << pointer_only_events << L"\n";
        }
    }

private:
    void CreateDevice(const OutputInfo& output) {
        constexpr D3D_FEATURE_LEVEL feature_levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL feature_level{};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        HRESULT result = D3D11CreateDevice(
            output.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
            feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
            &device_, &feature_level, &context_);
        if (result == E_INVALIDARG) {
            result = D3D11CreateDevice(
                output.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                &feature_levels[1], 1, D3D11_SDK_VERSION, &device_,
                &feature_level, &context_);
        }
        Check(result, "D3D11CreateDevice");
    }

    void CreateSwapChain(const OutputInfo& output) {
        ComPtr<IDXGIFactory2> factory;
        Check(output.adapter->GetParent(IID_PPV_ARGS(&factory)),
              "IDXGIAdapter::GetParent");

        RECT client{};
        GetClientRect(window_, &client);
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = static_cast<UINT>(client.right - client.left);
        description.Height = static_cast<UINT>(client.bottom - client.top);
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        ComPtr<IDXGISwapChain1> swap_chain1;
        Check(factory->CreateSwapChainForHwnd(
                  device_.Get(), window_, &description, nullptr,
                  output.output.Get(), &swap_chain1),
              "CreateSwapChainForHwnd");
        Check(swap_chain1.As(&swap_chain_), "QueryInterface(IDXGISwapChain2)");
        Check(factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER),
              "MakeWindowAssociation");

        Check(swap_chain_->SetMaximumFrameLatency(1),
              "SetMaximumFrameLatency");
        frame_latency_handle_ = swap_chain_->GetFrameLatencyWaitableObject();

        ComPtr<ID3D11Texture2D> back_buffer;
        Check(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)),
              "IDXGISwapChain::GetBuffer");
        Check(device_->CreateRenderTargetView(back_buffer.Get(), nullptr,
                                              &render_target_),
              "CreateRenderTargetView");

        viewport_.TopLeftX = 0.0F;
        viewport_.TopLeftY = 0.0F;
        viewport_.Width = static_cast<float>(description.Width);
        viewport_.Height = static_cast<float>(description.Height);
        viewport_.MinDepth = 0.0F;
        viewport_.MaxDepth = 1.0F;
    }

    void CreatePipeline() {
        static constexpr char shader_source[] = R"(
Texture2D source_texture : register(t0);
SamplerState source_sampler : register(s0);

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput VertexMain(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

float4 PixelMain(VertexOutput input) : SV_Target {
    return source_texture.Sample(source_sampler, input.uv);
}
)";

        const auto vertex_blob =
            CompileShader(shader_source, "VertexMain", "vs_5_0");
        const auto pixel_blob =
            CompileShader(shader_source, "PixelMain", "ps_5_0");

        Check(device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                          vertex_blob->GetBufferSize(), nullptr,
                                          &vertex_shader_),
              "CreateVertexShader");
        Check(device_->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                         pixel_blob->GetBufferSize(), nullptr,
                                         &pixel_shader_),
              "CreatePixelShader");

        D3D11_SAMPLER_DESC sampler_description{};
        sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device_->CreateSamplerState(&sampler_description, &sampler_),
              "CreateSamplerState");
    }

    void CreateDuplicator(const OutputInfo& output) {
        ComPtr<IDXGIOutput1> output1;
        Check(output.output.As(&output1), "QueryInterface(IDXGIOutput1)");
        Check(output1->DuplicateOutput(device_.Get(), &duplicator_),
              "DuplicateOutput");
    }

    void EnsureCropTexture(const D3D11_TEXTURE2D_DESC& desktop_description) {
        const UINT diameter = radius_ * 2;
        if (crop_texture_ && crop_width_ == diameter &&
            crop_height_ == diameter &&
            crop_format_ == desktop_description.Format) {
            return;
        }

        crop_view_.Reset();
        crop_texture_.Reset();

        D3D11_TEXTURE2D_DESC crop_description{};
        crop_description.Width = diameter;
        crop_description.Height = diameter;
        crop_description.MipLevels = 1;
        crop_description.ArraySize = 1;
        crop_description.Format = desktop_description.Format;
        crop_description.SampleDesc.Count = 1;
        crop_description.Usage = D3D11_USAGE_DEFAULT;
        crop_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        Check(device_->CreateTexture2D(&crop_description, nullptr,
                                       &crop_texture_),
              "CreateTexture2D(crop)");
        Check(device_->CreateShaderResourceView(crop_texture_.Get(), nullptr,
                                                &crop_view_),
              "CreateShaderResourceView(crop)");
        crop_width_ = diameter;
        crop_height_ = diameter;
        crop_format_ = desktop_description.Format;
    }

    void CopyCenterCrop(ID3D11Texture2D* desktop_texture) {
        D3D11_TEXTURE2D_DESC desktop_description{};
        desktop_texture->GetDesc(&desktop_description);
        const UINT diameter = radius_ * 2;
        if (desktop_description.Width < diameter ||
            desktop_description.Height < diameter) {
            throw std::runtime_error(
                "The requested capture region is larger than the monitor");
        }

        EnsureCropTexture(desktop_description);
        const UINT left = (desktop_description.Width - diameter) / 2;
        const UINT top = (desktop_description.Height - diameter) / 2;
        const D3D11_BOX source_box{
            left,
            top,
            0,
            left + diameter,
            top + diameter,
            1,
        };
        context_->CopySubresourceRegion(crop_texture_.Get(), 0, 0, 0, 0,
                                        desktop_texture, 0, &source_box);
    }

    void Render(bool vsync_present) {
        if (!crop_view_) {
            return;
        }

        ID3D11RenderTargetView* target = render_target_.Get();
        ID3D11ShaderResourceView* view = crop_view_.Get();
        ID3D11SamplerState* sampler = sampler_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->RSSetViewports(1, &viewport_);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, &view);
        context_->PSSetSamplers(0, 1, &sampler);
        context_->Draw(3, 0);

        ID3D11ShaderResourceView* no_view = nullptr;
        context_->PSSetShaderResources(0, 1, &no_view);
        Check(swap_chain_->Present(vsync_present ? 1 : 0, 0), "Present");
    }

    HWND window_ = nullptr;
    UINT radius_ = 0;
    UINT crop_width_ = 0;
    UINT crop_height_ = 0;
    DXGI_FORMAT crop_format_ = DXGI_FORMAT_UNKNOWN;
    HANDLE frame_latency_handle_ = nullptr;
    D3D11_VIEWPORT viewport_{};

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain2> swap_chain_;
    ComPtr<IDXGIOutputDuplication> duplicator_;
    ComPtr<ID3D11RenderTargetView> render_target_;
    ComPtr<ID3D11Texture2D> crop_texture_;
    ComPtr<ID3D11ShaderResourceView> crop_view_;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11SamplerState> sampler_;
};

}  // namespace

int wmain(int argument_count, wchar_t** arguments) {
    try {
        EnablePhysicalPixelCoordinates();
        const Options options = ParseOptions(argument_count, arguments);
        const auto outputs = EnumerateOutputs();

        if (options.list_monitors) {
            PrintOutputs(outputs);
            return outputs.empty() ? 1 : 0;
        }
        if (options.monitor > outputs.size()) {
            PrintOutputs(outputs);
            throw std::runtime_error("The selected monitor does not exist");
        }

        const OutputInfo& output = outputs[options.monitor - 1];
        if (output.description.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
            output.description.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
            throw std::runtime_error(
                "Rotated monitors are not supported by this minimal prototype");
        }

        const RECT rect = output.description.DesktopCoordinates;
        const UINT output_width = static_cast<UINT>(rect.right - rect.left);
        const UINT output_height = static_cast<UINT>(rect.bottom - rect.top);
        const UINT max_radius = static_cast<UINT>(std::floor(
            std::min(output_width, output_height) / (2.0F * options.zoom)));
        const UINT radius = std::min(options.radius, max_radius);
        if (radius < 16) {
            throw std::runtime_error(
                "The zoom and monitor size leave no usable capture region");
        }
        const UINT overlay_size = static_cast<UINT>(
            std::lround(static_cast<double>(radius * 2) * options.zoom));

        std::wcout << L"Using monitor " << options.monitor << L" ("
                   << output.description.DeviceName << L")\n"
                   << L"Center crop: " << (radius * 2) << L"x"
                   << (radius * 2) << L"; overlay: " << overlay_size << L"x"
                   << overlay_size << L"; zoom: " << options.zoom << L"x\n"
                   << L"Present mode: "
                   << (options.vsync_present ? L"vsync" : L"immediate")
                   << (options.stats ? L"; statistics enabled\n"
                                     : L"; statistics disabled\n")
                   << L"F8 toggles the overlay. F9 exits.\n";

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const HWND window =
            CreateOverlayWindow(instance, rect, overlay_size);
        GpuMagnifier magnifier(output, window, radius);
        magnifier.Run(options.duration_seconds, options.vsync_present,
                      options.stats);

        UnregisterHotKey(window, kToggleHotkeyId);
        UnregisterHotKey(window, kQuitHotkeyId);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
