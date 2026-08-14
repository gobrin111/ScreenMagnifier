#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"FPSMagnifierWgcPrototype";
constexpr int kToggleHotkeyId = 1;
constexpr int kQuitHotkeyId = 2;

bool g_running = true;
bool g_visible = true;
bool g_redraw_requested = false;
RECT g_overlay_bounds{};
POINT g_parked_position{};

struct Options {
    UINT monitor = 1;
    UINT radius = 200;
    float zoom = 2.0F;
    double duration_seconds = 0.0;
    bool list_monitors = false;
};

struct OutputInfo {
    UINT number = 0;
    HMONITOR monitor = nullptr;
    RECT bounds{};
    std::wstring device_name;
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

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto& outputs = *reinterpret_cast<std::vector<OutputInfo>*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        outputs.push_back(OutputInfo{
            static_cast<UINT>(outputs.size() + 1),
            monitor,
            info.rcMonitor,
            info.szDevice,
        });
    }
    return TRUE;
}

std::vector<OutputInfo> EnumerateOutputs() {
    std::vector<OutputInfo> outputs;
    if (!EnumDisplayMonitors(
            nullptr, nullptr, CollectMonitor,
            reinterpret_cast<LPARAM>(&outputs))) {
        Check(HRESULT_FROM_WIN32(GetLastError()), "EnumDisplayMonitors");
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
        const LONG width = output.bounds.right - output.bounds.left;
        const LONG height = output.bounds.bottom - output.bounds.top;
        std::wcout << L"  " << output.number << L": " << output.device_name
                   << L"  " << width << L"x" << height << L" at ("
                   << output.bounds.left << L", " << output.bounds.top
                   << L")\n";
    }
}

void PrintUsage() {
    std::wcout
        << L"Windows Graphics Capture Magnifier Prototype\n\n"
        << L"Usage:\n"
        << L"  WgcMagnifierPrototype.exe --list-monitors\n"
        << L"  WgcMagnifierPrototype.exe [--monitor N] [--radius PX]\n"
        << L"      [--zoom N] [--duration SECONDS]\n\n"
        << L"Captures the selected monitor, including an empty desktop.\n"
        << L"Defaults: --monitor 1 --radius 200 --zoom 2.0\n"
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
                if (g_visible) {
                    if (!SetWindowDisplayAffinity(
                            window, WDA_EXCLUDEFROMCAPTURE)) {
                        std::wcerr
                            << L"Warning: could not restore capture exclusion.\n";
                    }
                    const int width =
                        g_overlay_bounds.right - g_overlay_bounds.left;
                    const int height =
                        g_overlay_bounds.bottom - g_overlay_bounds.top;
                    if (!SetWindowPos(
                            window, HWND_TOPMOST, g_overlay_bounds.left,
                            g_overlay_bounds.top, width, height,
                            SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
                        std::wcerr << L"Warning: could not restore overlay position.\n";
                    }
                    g_redraw_requested = true;
                } else {
                    // Keep the HWND and flip-model swap chain alive. Hiding this
                    // window can detach its surface and produce a black buffer
                    // when it is shown again on some Windows/GPU combinations.
                    if (!SetWindowPos(
                            window, nullptr, g_parked_position.x,
                            g_parked_position.y, 0, 0,
                            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER)) {
                        std::wcerr << L"Warning: could not park overlay off-screen.\n";
                    }
                }
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
    g_overlay_bounds = RECT{
        left,
        top,
        left + static_cast<LONG>(overlay_size),
        top + static_cast<LONG>(overlay_size),
    };
    g_parked_position = POINT{
        GetSystemMetrics(SM_XVIRTUALSCREEN) +
            GetSystemMetrics(SM_CXVIRTUALSCREEN) +
            static_cast<LONG>(overlay_size),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
    };
    const DWORD extended_style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                 WS_EX_NOACTIVATE | WS_EX_LAYERED |
                                 WS_EX_TRANSPARENT;
    HWND window = CreateWindowExW(
        extended_style, kWindowClass, L"WGC Magnifier Prototype", WS_POPUP,
        left, top, static_cast<int>(overlay_size),
        static_cast<int>(overlay_size), nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        Check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW");
    }
    // WS_EX_TRANSPARENT provides cross-process mouse pass-through when it is
    // paired with WS_EX_LAYERED. Keep the rendered overlay fully opaque.
    if (!SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA)) {
        const DWORD error = GetLastError();
        DestroyWindow(window);
        Check(HRESULT_FROM_WIN32(error), "SetLayeredWindowAttributes");
    }
    if (!SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) {
        const DWORD error = GetLastError();
        DestroyWindow(window);
        Check(HRESULT_FROM_WIN32(error),
              "SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)");
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
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
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

class WgcMagnifier {
public:
    WgcMagnifier(HMONITOR source, HWND overlay, UINT radius)
        : overlay_(overlay), radius_(radius) {
        CreateDevice();
        CreateSwapChain();
        CreatePipeline();
        CreateCapture(source);
    }

    ~WgcMagnifier() {
        CloseCapture();
    }

    void Run(double duration_seconds) {
        capture_session_.StartCapture();
        const auto run_start = std::chrono::steady_clock::now();
        std::uint64_t received_frames = 0;
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
            if (duration_seconds > 0.0 &&
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - run_start)
                        .count() >= duration_seconds) {
                DestroyWindow(overlay_);
                break;
            }

            Direct3D11CaptureFrame frame{nullptr};
            while (auto newest = frame_pool_.TryGetNextFrame()) {
                frame = std::move(newest);
                ++received_frames;
            }
            if (!frame) {
                if (g_visible && g_redraw_requested && crop_view_) {
                    Render();
                    g_redraw_requested = false;
                }
                MsgWaitForMultipleObjectsEx(
                    0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                continue;
            }

            auto surface_access =
                frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::
                                       IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> frame_texture;
            Check(surface_access->GetInterface(IID_PPV_ARGS(&frame_texture)),
                  "IDirect3DDxgiInterfaceAccess::GetInterface");
            CopyCenterCrop(frame_texture.Get());
            // Continue presenting while parked off-screen. This prevents the
            // flip-model surface from being torn down during an F8 toggle.
            Render();
            g_redraw_requested = false;
        }

        const double elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - run_start).count();
        std::wcout << L"Received " << received_frames << L" capture frames in "
                   << elapsed_seconds << L" seconds";
        if (elapsed_seconds > 0.0) {
            std::wcout << L" (" << (received_frames / elapsed_seconds)
                       << L" fps)";
        }
        std::wcout << L".\n";
    }

private:
    void CreateDevice() {
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL level{};
        HRESULT result = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
            D3D11_SDK_VERSION, &device_, &level, &context_);
        if (result == E_INVALIDARG) {
            result = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, &levels[1], 1,
                D3D11_SDK_VERSION, &device_, &level, &context_);
        }
        Check(result, "D3D11CreateDevice");

        ComPtr<IDXGIDevice> dxgi_device;
        Check(device_.As(&dxgi_device), "QueryInterface(IDXGIDevice)");
        winrt::com_ptr<::IInspectable> inspectable;
        Check(CreateDirect3D11DeviceFromDXGIDevice(
                  dxgi_device.Get(), inspectable.put()),
              "CreateDirect3D11DeviceFromDXGIDevice");
        capture_device_ = inspectable.as<IDirect3DDevice>();
    }

    void CreateSwapChain() {
        ComPtr<IDXGIDevice> dxgi_device;
        Check(device_.As(&dxgi_device), "QueryInterface(IDXGIDevice)");
        ComPtr<IDXGIAdapter> adapter;
        Check(dxgi_device->GetAdapter(&adapter), "IDXGIDevice::GetAdapter");
        ComPtr<IDXGIFactory2> factory;
        Check(adapter->GetParent(IID_PPV_ARGS(&factory)),
              "IDXGIAdapter::GetParent");

        RECT client{};
        GetClientRect(overlay_, &client);
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
                  device_.Get(), overlay_, &description, nullptr, nullptr,
                  &swap_chain1),
              "CreateSwapChainForHwnd");
        Check(swap_chain1.As(&swap_chain_), "QueryInterface(IDXGISwapChain2)");
        Check(factory->MakeWindowAssociation(overlay_, DXGI_MWA_NO_ALT_ENTER),
              "MakeWindowAssociation");
        Check(swap_chain_->SetMaximumFrameLatency(1),
              "SetMaximumFrameLatency");

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

    void CreateCapture(HMONITOR source) {
        if (!GraphicsCaptureSession::IsSupported()) {
            throw std::runtime_error(
                "Windows Graphics Capture is not supported on this system");
        }

        const auto interop = winrt::get_activation_factory<
            GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        Check(interop->CreateForMonitor(
                  source, winrt::guid_of<GraphicsCaptureItem>(),
                  winrt::put_abi(capture_item_)),
              "IGraphicsCaptureItemInterop::CreateForMonitor");

        frame_pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            capture_device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 3,
            capture_item_.Size());
        capture_session_ = frame_pool_.CreateCaptureSession(capture_item_);
        try {
            capture_session_.IsCursorCaptureEnabled(false);
        } catch (const winrt::hresult_error&) {
        }
        try {
            capture_session_.IsBorderRequired(false);
        } catch (const winrt::hresult_error&) {
        }
    }

    void CloseCapture() noexcept {
        if (capture_session_) {
            capture_session_.Close();
            capture_session_ = nullptr;
        }
        if (frame_pool_) {
            frame_pool_.Close();
            frame_pool_ = nullptr;
        }
        capture_item_ = nullptr;
        capture_device_ = nullptr;
    }

    void EnsureCropTexture(const D3D11_TEXTURE2D_DESC& frame_description) {
        const UINT diameter = radius_ * 2;
        if (crop_texture_ && crop_width_ == diameter &&
            crop_height_ == diameter && crop_format_ == frame_description.Format) {
            return;
        }
        crop_view_.Reset();
        crop_texture_.Reset();

        D3D11_TEXTURE2D_DESC crop_description{};
        crop_description.Width = diameter;
        crop_description.Height = diameter;
        crop_description.MipLevels = 1;
        crop_description.ArraySize = 1;
        crop_description.Format = frame_description.Format;
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
        crop_format_ = frame_description.Format;
    }

    void CopyCenterCrop(ID3D11Texture2D* frame_texture) {
        D3D11_TEXTURE2D_DESC frame_description{};
        frame_texture->GetDesc(&frame_description);
        const UINT diameter = radius_ * 2;
        if (frame_description.Width < diameter ||
            frame_description.Height < diameter) {
            throw std::runtime_error(
                "The selected monitor became smaller than the capture region");
        }
        EnsureCropTexture(frame_description);
        const UINT left = (frame_description.Width - diameter) / 2;
        const UINT top = (frame_description.Height - diameter) / 2;
        const D3D11_BOX source_box{
            left, top, 0, left + diameter, top + diameter, 1,
        };
        context_->CopySubresourceRegion(crop_texture_.Get(), 0, 0, 0, 0,
                                        frame_texture, 0, &source_box);
    }

    void Render() {
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
        Check(swap_chain_->Present(1, 0), "Present");
    }

    HWND overlay_ = nullptr;
    UINT radius_ = 0;
    UINT crop_width_ = 0;
    UINT crop_height_ = 0;
    DXGI_FORMAT crop_format_ = DXGI_FORMAT_UNKNOWN;
    D3D11_VIEWPORT viewport_{};

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain2> swap_chain_;
    ComPtr<ID3D11RenderTargetView> render_target_;
    ComPtr<ID3D11Texture2D> crop_texture_;
    ComPtr<ID3D11ShaderResourceView> crop_view_;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11SamplerState> sampler_;

    IDirect3DDevice capture_device_{nullptr};
    GraphicsCaptureItem capture_item_{nullptr};
    Direct3D11CaptureFramePool frame_pool_{nullptr};
    GraphicsCaptureSession capture_session_{nullptr};
};

}  // namespace

int wmain(int argument_count, wchar_t** arguments) {
    HWND overlay = nullptr;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        EnablePhysicalPixelCoordinates();
        const Options options = ParseOptions(argument_count, arguments);

        const auto outputs = EnumerateOutputs();
        if (options.list_monitors) {
            PrintOutputs(outputs);
            return outputs.empty() ? 1 : 0;
        }
        if (outputs.empty()) {
            throw std::runtime_error("No attached monitors were found");
        }
        if (options.monitor > outputs.size()) {
            PrintOutputs(outputs);
            throw std::runtime_error("The selected monitor number does not exist");
        }
        const auto& source = outputs[options.monitor - 1];
        const UINT monitor_width = static_cast<UINT>(
            source.bounds.right - source.bounds.left);
        const UINT monitor_height = static_cast<UINT>(
            source.bounds.bottom - source.bounds.top);
        const UINT max_source_radius =
            std::min(monitor_width, monitor_height) / 2;
        const UINT max_overlay_radius = static_cast<UINT>(std::floor(
            std::min(monitor_width, monitor_height) / (2.0F * options.zoom)));
        const UINT radius =
            std::min(options.radius, std::min(max_source_radius, max_overlay_radius));
        if (radius < 16) {
            throw std::runtime_error("The selected monitor is too small");
        }
        const UINT overlay_size = static_cast<UINT>(std::lround(
            static_cast<double>(radius * 2) * options.zoom));

        overlay = CreateOverlayWindow(GetModuleHandleW(nullptr),
                                      source.bounds, overlay_size);
        std::wcout << L"Capturing monitor " << source.number << L": "
                   << source.device_name << L" (" << monitor_width << L"x"
                   << monitor_height << L")\n"
                   << L"Source crop: " << (radius * 2) << L"x" << (radius * 2)
                   << L"; overlay: " << overlay_size << L"x" << overlay_size
                   << L"; zoom: " << options.zoom << L"x\n"
                   << L"F8 toggles the overlay. F9 exits.\n";

        WgcMagnifier magnifier(source.monitor, overlay, radius);
        magnifier.Run(options.duration_seconds);

        if (overlay != nullptr && IsWindow(overlay)) {
            DestroyWindow(overlay);
        }
        return 0;
    } catch (const winrt::hresult_error& error) {
        if (overlay != nullptr && IsWindow(overlay)) {
            DestroyWindow(overlay);
        }
        std::wcerr << L"Error: " << error.message().c_str() << L" ("
                   << Hex(error.code()).c_str() << L")\n";
        return 1;
    } catch (const std::exception& error) {
        if (overlay != nullptr && IsWindow(overlay)) {
            DestroyWindow(overlay);
        }
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
