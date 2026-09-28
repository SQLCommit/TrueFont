// Lossless PNG through WIC; readPng decodes any image WIC reads to A8R8G8B8.
// CLSID_WICImagingFactory1: the Windows 8 SDK's CLSID_WICImagingFactory is Factory2, which Windows 7 without KB2670838 lacks.
#pragma once
#include <windows.h>
#include <wincodec.h>
#include <new>
#include <string>
#include <vector>
#include "imaging.h"

namespace tf {

inline bool writePng(const std::string& path, const Image& img, std::string& why) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    HRESULT h = CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    wchar_t wpath[MAX_PATH * 2] = {};
    if (SUCCEEDED(h) && !MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wpath, MAX_PATH * 2)) h = E_INVALIDARG;
    if (SUCCEEDED(h)) h = factory->CreateStream(&stream);
    if (SUCCEEDED(h)) h = stream->InitializeFromFilename(wpath, GENERIC_WRITE);
    if (SUCCEEDED(h)) h = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(h)) h = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(h)) h = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(h)) h = frame->Initialize(nullptr);
    if (SUCCEEDED(h)) h = frame->SetSize(img.w, img.h);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(h)) h = frame->SetPixelFormat(&format);
    if (SUCCEEDED(h) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) h = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    const UINT stride = UINT(bmpStride(img.w));
    if (SUCCEEDED(h)) h = frame->WritePixels(img.h, stride, stride * img.h, const_cast<BYTE*>(img.bgr.data()));
    if (SUCCEEDED(h)) h = frame->Commit();
    if (SUCCEEDED(h)) h = encoder->Commit();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (uninit) CoUninitialize();
    if (FAILED(h)) { char b[40]; _snprintf_s(b, sizeof b, _TRUNCATE, "WIC PNG 0x%08X", unsigned(h)); why = b; DeleteFileA(path.c_str()); }
    return SUCCEEDED(h);
}

// Any image WIC decodes, as A8R8G8B8 (0xAARRGGBB, row-major). `maxW` / `maxH` bound the decoded size (a
// corrupt file must not allocate without limit); 0 = no bound.
inline bool readPngW(const wchar_t* wpath, int& w, int& h, std::vector<uint32_t>& px, std::string& why, UINT maxW = 0, UINT maxH = 0) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(wpath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT uw = 0, uh = 0;
    if (SUCCEEDED(hr)) hr = conv->GetSize(&uw, &uh);
    if (SUCCEEDED(hr) && ((maxW && uw > maxW) || (maxH && uh > maxH))) hr = WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    if (SUCCEEDED(hr)) {
        // Up to 32 MiB (2048 x 4096): out of memory fails the read, and the objects and the file below are still released.
        try { px.assign(size_t(uw) * uh, 0); } catch (const std::bad_alloc&) { hr = E_OUTOFMEMORY; }
    }
    if (SUCCEEDED(hr)) hr = conv->CopyPixels(nullptr, uw * 4, UINT(px.size() * 4), reinterpret_cast<BYTE*>(px.data()));
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (uninit) CoUninitialize();
    if (FAILED(hr)) { char b[40]; _snprintf_s(b, sizeof b, _TRUNCATE, "WIC decode 0x%08X", unsigned(hr)); why = b; px.clear(); return false; }
    w = int(uw);
    h = int(uh);
    return true;
}

inline bool readPng(const std::string& path, int& w, int& h, std::vector<uint32_t>& px, std::string& why) {
    wchar_t wpath[MAX_PATH * 2] = {};
    if (!MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wpath, MAX_PATH * 2)) { why = "the path does not convert"; px.clear(); return false; }
    return readPngW(wpath, w, h, px, why);
}

}  // namespace tf
