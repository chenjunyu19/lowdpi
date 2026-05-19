#define NOMINMAX

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr double kBaseDpi = 96.0;
constexpr double kNoResizeEpsilon = 0.0001;
constexpr DWORD kClipboardOpenRetryDelayMs = 20;
constexpr DWORD kClipboardOpenRetryCount = 25;

bool TryOpenClipboard(HWND owner)
{
    for (DWORD attempt = 0; attempt != kClipboardOpenRetryCount; ++attempt)
    {
        if (OpenClipboard(owner) != FALSE)
        {
            return true;
        }

        Sleep(kClipboardOpenRetryDelayMs);
    }

    return false;
}

class ScopedClipboard
{
public:
    explicit ScopedClipboard(HWND owner) : opened_(TryOpenClipboard(owner)) {}

    ~ScopedClipboard()
    {
        if (opened_)
        {
            CloseClipboard();
        }
    }

    ScopedClipboard(const ScopedClipboard&) = delete;
    ScopedClipboard& operator=(const ScopedClipboard&) = delete;

    bool IsOpen() const { return opened_; }

private:
    bool opened_;
};

class ScopedScreenDc
{
public:
    ScopedScreenDc() : hwnd_(nullptr), dc_(GetDC(hwnd_)) {}

    ~ScopedScreenDc()
    {
        if (dc_ != nullptr)
        {
            ReleaseDC(hwnd_, dc_);
        }
    }

    ScopedScreenDc(const ScopedScreenDc&) = delete;
    ScopedScreenDc& operator=(const ScopedScreenDc&) = delete;

    HDC Get() const { return dc_; }

private:
    HWND hwnd_;
    HDC dc_;
};

std::wstring FormatErrorMessage(HRESULT hr)
{
    wchar_t* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD language = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
    const DWORD length = FormatMessageW(
        flags,
        nullptr,
        static_cast<DWORD>(hr),
        language,
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);

    std::wstring text;
    if (length != 0 && message != nullptr)
    {
        text.assign(message, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
        {
            text.pop_back();
        }
    }
    else
    {
        wchar_t fallback[64] = {};
        wsprintfW(fallback, L"HRESULT 0x%08X", static_cast<unsigned int>(hr));
        text = fallback;
    }

    if (message != nullptr)
    {
        LocalFree(message);
    }

    return text;
}

HRESULT GetLastErrorAsHresult(HRESULT fallback = E_FAIL)
{
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? fallback : HRESULT_FROM_WIN32(error);
}

void ShowErrorBox(const wchar_t* title, HRESULT hr)
{
    const std::wstring body = FormatErrorMessage(hr);
    MessageBoxW(nullptr, body.c_str(), title, MB_ICONERROR | MB_OK);
}

void EnableBestEffortDpiAwareness()
{
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr)
    {
        return;
    }

    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto set_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_context != nullptr)
    {
        set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return;
    }

    using SetProcessDPIAwareFn = BOOL(WINAPI*)();
    auto legacy = reinterpret_cast<SetProcessDPIAwareFn>(GetProcAddress(user32, "SetProcessDPIAware"));
    if (legacy != nullptr)
    {
        legacy();
    }
}

double GetSystemScale()
{
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr)
    {
        using GetDpiForSystemFn = UINT(WINAPI*)();
        auto get_dpi_for_system = reinterpret_cast<GetDpiForSystemFn>(
            GetProcAddress(user32, "GetDpiForSystem"));
        if (get_dpi_for_system != nullptr)
        {
            const UINT dpi = get_dpi_for_system();
            if (dpi != 0)
            {
                return static_cast<double>(dpi) / kBaseDpi;
            }
        }
    }

    ScopedScreenDc screen_dc;
    if (screen_dc.Get() == nullptr)
    {
        return 1.0;
    }

    const int dpi = GetDeviceCaps(screen_dc.Get(), LOGPIXELSX);
    if (dpi <= 0)
    {
        return 1.0;
    }

    return static_cast<double>(dpi) / kBaseDpi;
}

size_t CalculateClipboardDibBitsOffset(const BITMAPINFOHEADER& header)
{
    if (header.biCompression == BI_JPEG || header.biCompression == BI_PNG)
    {
        return 0;
    }

    size_t offset = header.biSize;

    if (header.biSize == sizeof(BITMAPINFOHEADER) && header.biCompression == BI_BITFIELDS)
    {
        offset += 3 * sizeof(DWORD);
    }

    UINT color_table_entries = 0;
    if (header.biClrUsed != 0)
    {
        color_table_entries = header.biClrUsed;
    }
    else if (header.biBitCount <= 8)
    {
        color_table_entries = 1u << header.biBitCount;
    }

    offset += static_cast<size_t>(color_table_entries) * sizeof(RGBQUAD);
    return offset;
}

HRESULT CreateBitmapSourceFromHBitmap(
    IWICImagingFactory* factory,
    HBITMAP hbitmap,
    IWICBitmapSource** bitmap_source)
{
    if (factory == nullptr || hbitmap == nullptr || bitmap_source == nullptr)
    {
        return E_INVALIDARG;
    }

    *bitmap_source = nullptr;

    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = factory->CreateBitmapFromHBITMAP(hbitmap, nullptr, WICBitmapUsePremultipliedAlpha, &bitmap);
    if (FAILED(hr))
    {
        hr = factory->CreateBitmapFromHBITMAP(hbitmap, nullptr, WICBitmapIgnoreAlpha, &bitmap);
        if (FAILED(hr))
        {
            return hr;
        }
    }

    ComPtr<IWICBitmap> cached_bitmap;
    hr = factory->CreateBitmapFromSource(bitmap.Get(), WICBitmapCacheOnLoad, &cached_bitmap);
    if (FAILED(hr))
    {
        return hr;
    }

    *bitmap_source = cached_bitmap.Detach();
    return S_OK;
}

HRESULT CreateBitmapSourceFromClipboardDib(
    IWICImagingFactory* factory,
    HANDLE dib_handle,
    IWICBitmapSource** bitmap_source)
{
    if (factory == nullptr || dib_handle == nullptr || bitmap_source == nullptr)
    {
        return E_INVALIDARG;
    }

    *bitmap_source = nullptr;

    const SIZE_T dib_size = GlobalSize(dib_handle);
    if (dib_size < sizeof(BITMAPINFOHEADER))
    {
        return E_FAIL;
    }

    const void* dib_data = GlobalLock(dib_handle);
    if (dib_data == nullptr)
    {
        return GetLastErrorAsHresult();
    }

    const auto* header = static_cast<const BITMAPINFOHEADER*>(dib_data);
    if (header->biSize < sizeof(BITMAPINFOHEADER))
    {
        GlobalUnlock(dib_handle);
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    const size_t bits_offset = CalculateClipboardDibBitsOffset(*header);
    if (bits_offset == 0 || bits_offset >= dib_size)
    {
        GlobalUnlock(dib_handle);
        return E_FAIL;
    }

    const auto* bitmap_info = reinterpret_cast<const BITMAPINFO*>(header);
    const auto* bits = static_cast<const BYTE*>(dib_data) + bits_offset;

    ScopedScreenDc screen_dc;
    if (screen_dc.Get() == nullptr)
    {
        GlobalUnlock(dib_handle);
        return GetLastErrorAsHresult();
    }

    HBITMAP hbitmap = CreateDIBitmap(
        screen_dc.Get(),
        header,
        CBM_INIT,
        bits,
        bitmap_info,
        DIB_RGB_COLORS);

    GlobalUnlock(dib_handle);

    if (hbitmap == nullptr)
    {
        return GetLastErrorAsHresult();
    }

    HRESULT hr = CreateBitmapSourceFromHBitmap(factory, hbitmap, bitmap_source);
    DeleteObject(hbitmap);
    return hr;
}

HRESULT LoadClipboardImage(
    IWICImagingFactory* factory,
    IWICBitmapSource** bitmap_source)
{
    if (factory == nullptr || bitmap_source == nullptr)
    {
        return E_INVALIDARG;
    }

    *bitmap_source = nullptr;

    ScopedClipboard clipboard(nullptr);
    if (!clipboard.IsOpen())
    {
        return GetLastErrorAsHresult();
    }

    if (IsClipboardFormatAvailable(CF_DIBV5))
    {
        HANDLE dibv5 = GetClipboardData(CF_DIBV5);
        if (dibv5 != nullptr)
        {
            HRESULT hr = CreateBitmapSourceFromClipboardDib(factory, dibv5, bitmap_source);
            if (SUCCEEDED(hr))
            {
                return S_OK;
            }
        }
    }

    if (IsClipboardFormatAvailable(CF_DIB))
    {
        HANDLE dib = GetClipboardData(CF_DIB);
        if (dib != nullptr)
        {
            HRESULT hr = CreateBitmapSourceFromClipboardDib(factory, dib, bitmap_source);
            if (SUCCEEDED(hr))
            {
                return S_OK;
            }
        }
    }

    if (IsClipboardFormatAvailable(CF_BITMAP))
    {
        HBITMAP hbitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
        if (hbitmap != nullptr)
        {
            return CreateBitmapSourceFromHBitmap(factory, hbitmap, bitmap_source);
        }
    }

    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT ResizeBitmapForScale(
    IWICImagingFactory* factory,
    IWICBitmapSource* input,
    double scale,
    IWICBitmapSource** output)
{
    if (factory == nullptr || input == nullptr || output == nullptr)
    {
        return E_INVALIDARG;
    }

    *output = nullptr;

    UINT width = 0;
    UINT height = 0;
    HRESULT hr = input->GetSize(&width, &height);
    if (FAILED(hr))
    {
        return hr;
    }

    if (width == 0 || height == 0)
    {
        return E_FAIL;
    }

    const UINT target_width = (scale <= 1.0)
        ? width
        : static_cast<UINT>(std::max(1L, std::lround(static_cast<double>(width) / scale)));
    const UINT target_height = (scale <= 1.0)
        ? height
        : static_cast<UINT>(std::max(1L, std::lround(static_cast<double>(height) / scale)));

    if (target_width == width && target_height == height)
    {
        ComPtr<IWICBitmap> cached_bitmap;
        hr = factory->CreateBitmapFromSource(input, WICBitmapCacheOnLoad, &cached_bitmap);
        if (FAILED(hr))
        {
            return hr;
        }

        *output = cached_bitmap.Detach();
        return S_OK;
    }

    ComPtr<IWICBitmapScaler> scaler;
    hr = factory->CreateBitmapScaler(&scaler);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = scaler->Initialize(input, target_width, target_height, WICBitmapInterpolationModeFant);
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<IWICBitmapSource> scaled_source;
    hr = scaler.As(&scaled_source);
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<IWICBitmap> cached_bitmap;
    hr = factory->CreateBitmapFromSource(scaled_source.Get(), WICBitmapCacheOnLoad, &cached_bitmap);
    if (FAILED(hr))
    {
        return hr;
    }

    *output = cached_bitmap.Detach();
    return S_OK;
}

HRESULT ConvertSourceToBgraPixels(
    IWICImagingFactory* factory,
    IWICBitmapSource* input,
    UINT* width,
    UINT* height,
    UINT* stride,
    std::vector<BYTE>* pixels)
{
    if (factory == nullptr || input == nullptr || width == nullptr || height == nullptr ||
        stride == nullptr || pixels == nullptr)
    {
        return E_INVALIDARG;
    }

    *width = 0;
    *height = 0;
    *stride = 0;
    pixels->clear();

    HRESULT hr = input->GetSize(width, height);
    if (FAILED(hr))
    {
        return hr;
    }

    if (*width == 0 || *height == 0)
    {
        return E_FAIL;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = converter->Initialize(
        input,
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        return hr;
    }

    *stride = *width * 4;
    const size_t image_size = static_cast<size_t>(*stride) * *height;
    pixels->resize(image_size);

    return converter->CopyPixels(nullptr, *stride, static_cast<UINT>(image_size), pixels->data());
}

HRESULT CreateClipboardBitmap(
    UINT width,
    UINT height,
    UINT stride,
    const BYTE* pixels,
    HBITMAP* hbitmap)
{
    if (pixels == nullptr || hbitmap == nullptr || width == 0 || height == 0 || stride == 0)
    {
        return E_INVALIDARG;
    }

    *hbitmap = nullptr;

    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = static_cast<LONG>(width);
    header.bV5Height = -static_cast<LONG>(height);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    header.bV5CSType = LCS_sRGB;

    void* dib_bits = nullptr;
    ScopedScreenDc screen_dc;
    if (screen_dc.Get() == nullptr)
    {
        return GetLastErrorAsHresult();
    }

    HBITMAP bitmap = CreateDIBSection(
        screen_dc.Get(),
        reinterpret_cast<BITMAPINFO*>(&header),
        DIB_RGB_COLORS,
        &dib_bits,
        nullptr,
        0);
    if (bitmap == nullptr || dib_bits == nullptr)
    {
        return GetLastErrorAsHresult();
    }

    const size_t bytes = static_cast<size_t>(stride) * height;
    std::memcpy(dib_bits, pixels, bytes);
    *hbitmap = bitmap;
    return S_OK;
}

HRESULT WriteBitmapToClipboard(IWICImagingFactory* factory, IWICBitmapSource* input)
{
    if (factory == nullptr || input == nullptr)
    {
        return E_INVALIDARG;
    }

    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    std::vector<BYTE> pixels;
    HRESULT hr = ConvertSourceToBgraPixels(factory, input, &width, &height, &stride, &pixels);
    if (FAILED(hr))
    {
        return hr;
    }

    const size_t image_size = static_cast<size_t>(stride) * height;
    const size_t total_size = sizeof(BITMAPV5HEADER) + image_size;
    HGLOBAL dib_handle = GlobalAlloc(GMEM_MOVEABLE, total_size);
    if (dib_handle == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    void* dib_memory = GlobalLock(dib_handle);
    if (dib_memory == nullptr)
    {
        const HRESULT lock_hr = GetLastErrorAsHresult();
        GlobalFree(dib_handle);
        return lock_hr;
    }

    auto* header = static_cast<BITMAPV5HEADER*>(dib_memory);
    ZeroMemory(header, sizeof(*header));
    header->bV5Size = sizeof(*header);
    header->bV5Width = static_cast<LONG>(width);
    header->bV5Height = -static_cast<LONG>(height);
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5RedMask = 0x00FF0000;
    header->bV5GreenMask = 0x0000FF00;
    header->bV5BlueMask = 0x000000FF;
    header->bV5AlphaMask = 0xFF000000;
    header->bV5SizeImage = static_cast<DWORD>(image_size);
    header->bV5CSType = LCS_sRGB;

    BYTE* bitmap_bits = reinterpret_cast<BYTE*>(header + 1);
    std::memcpy(bitmap_bits, pixels.data(), image_size);
    GlobalUnlock(dib_handle);

    HBITMAP clipboard_bitmap = nullptr;
    hr = CreateClipboardBitmap(width, height, stride, pixels.data(), &clipboard_bitmap);
    if (FAILED(hr))
    {
        GlobalFree(dib_handle);
        return hr;
    }

    ScopedClipboard clipboard(nullptr);
    if (!clipboard.IsOpen())
    {
        DeleteObject(clipboard_bitmap);
        GlobalFree(dib_handle);
        return GetLastErrorAsHresult();
    }

    if (!EmptyClipboard())
    {
        const HRESULT empty_hr = GetLastErrorAsHresult();
        DeleteObject(clipboard_bitmap);
        GlobalFree(dib_handle);
        return empty_hr;
    }

    if (SetClipboardData(CF_DIBV5, dib_handle) == nullptr)
    {
        const HRESULT set_hr = GetLastErrorAsHresult();
        DeleteObject(clipboard_bitmap);
        GlobalFree(dib_handle);
        return set_hr;
    }

    dib_handle = nullptr;

    if (SetClipboardData(CF_BITMAP, clipboard_bitmap) == nullptr)
    {
        DeleteObject(clipboard_bitmap);
    }

    return S_OK;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    EnableBestEffortDpiAwareness();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        ShowErrorBox(L"lowdpi", hr);
        return 1;
    }

    int exit_code = 0;

    do
    {
        const double scale = GetSystemScale();
        if (scale <= 1.0 + kNoResizeEpsilon)
        {
            break;
        }

        ComPtr<IWICImagingFactory> factory;
        hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (FAILED(hr))
        {
            exit_code = 1;
            ShowErrorBox(L"Failed to create WIC factory", hr);
            break;
        }

        ComPtr<IWICBitmapSource> clipboard_image;
        hr = LoadClipboardImage(factory.Get(), &clipboard_image);
        if (FAILED(hr))
        {
            exit_code = 2;
            ShowErrorBox(L"No clipboard image found", hr);
            break;
        }

        ComPtr<IWICBitmapSource> resized_image;
        hr = ResizeBitmapForScale(factory.Get(), clipboard_image.Get(), scale, &resized_image);
        if (FAILED(hr))
        {
            exit_code = 3;
            ShowErrorBox(L"Failed to resize image", hr);
            break;
        }

        hr = WriteBitmapToClipboard(factory.Get(), resized_image.Get());
        if (FAILED(hr))
        {
            exit_code = 4;
            ShowErrorBox(L"Failed to write image back to clipboard", hr);
            break;
        }
    } while (false);

    CoUninitialize();
    return exit_code;
}
