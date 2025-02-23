#pragma once

#include "Layers/xrRender/HWCaps.h"
#include "xrCore/ModuleLookup.hpp"
#include "SDL.h"
#include "SDL_syswm.h"

#if !defined(_MAYA_EXPORT)
#include "Layers/xrRender/stats_manager.h"
#endif

class CHW
    : public pureAppActivate,
      public pureAppDeactivate
{
public:
    CHW();
    ~CHW();

    void CreateD3D();
    void DestroyD3D();

    void CreateDevice(SDL_Window* sdlWnd);
    void DestroyDevice();

    void Reset();

    void Validate() {}

    bool CheckFormatSupport(DXGI_FORMAT format, UINT feature) const;
    DXGI_FORMAT SelectFormat(D3D_FORMAT_SUPPORT feature, const DXGI_FORMAT formats[], size_t count) const;
    template <size_t count>
    inline DXGI_FORMAT SelectFormat(D3D_FORMAT_SUPPORT feature, const DXGI_FORMAT (&formats)[count]) const
    {
        return SelectFormat(feature, formats, count);
    }
    bool UsingFlipPresentationModel() const;

    void OnAppActivate() override;
    void OnAppDeactivate() override;

private:
    void CreateSwapChain(HWND hwnd);
    bool CreateSwapChain2(HWND hwnd);

    void UpdateViews();

public:
    CHWCaps Caps;

    ID3DDevice* pDevice = nullptr; // render device
    ID3DRenderTargetView* pBaseRT = nullptr; // base render target
    ID3DDepthStencilView* pBaseZB = nullptr; // base depth-stencil buffer

    D3D_DRIVER_TYPE m_DriverType;

    IDXGIFactory1* m_pFactory = nullptr;
    IDXGIAdapter1* m_pAdapter = nullptr; // pD3D equivalent
    ID3DDeviceContext* pContext = nullptr;
    IDXGISwapChain* m_pSwapChain = nullptr;
    DXGI_SWAP_CHAIN_DESC m_ChainDesc; // DevPP equivalent
    D3D_FEATURE_LEVEL FeatureLevel;
    bool ComputeShadersSupported;
#ifdef HAS_DX11_2
    IDXGIFactory2* m_pFactory2 = nullptr;
    IDXGISwapChain2* m_pSwapChain2 = nullptr;
#endif
#ifdef HAS_DX11_3
    ID3D11Device3* pDevice3 = nullptr;
#endif
#if defined(USE_DX10)
    ID3D10Device1* pDevice1 = nullptr;
    ID3D10Device1* pContext1 = nullptr;
#endif

#if !defined(_MAYA_EXPORT)
    stats_manager stats_manager;
#endif
};

extern ECORE_API CHW HW;
