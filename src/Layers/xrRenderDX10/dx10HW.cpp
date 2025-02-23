#include "stdafx.h"

#include "dx10HW.h"
#include "xrEngine/xr_input.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrCore/xr_token.h"

#include "StateManager/dx10SamplerStateCache.h"
#include "StateManager/dx10StateCache.h"
#include "dx10TextureUtils.h"

CHW HW;

CHW::CHW()
{
    Device.seqAppActivate.Add(this);
    Device.seqAppDeactivate.Add(this);
}

CHW::~CHW()
{
    Device.seqAppActivate.Remove(this);
    Device.seqAppDeactivate.Remove(this);
}

void CHW::OnAppActivate()
{
    if (m_pSwapChain && !m_ChainDesc.Windowed)
    {
        ShowWindow(m_ChainDesc.OutputWindow, SW_RESTORE);
        m_pSwapChain->SetFullscreenState(psDeviceFlags.is(rsFullscreen), NULL);
    }
}

void CHW::OnAppDeactivate()
{
    if (m_pSwapChain && !m_ChainDesc.Windowed)
    {
        m_pSwapChain->SetFullscreenState(FALSE, NULL);
        ShowWindow(m_ChainDesc.OutputWindow, SW_MINIMIZE);
    }
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
void CHW::CreateD3D()
{
    // Минимально поддерживаемая версия Windows => Windows Vista SP2 или Windows 7.
    R_CHK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)(&m_pFactory)));
    R_CHK(m_pFactory->EnumAdapters1(0, &m_pAdapter));
}

void CHW::DestroyD3D()
{
    _SHOW_REF("refCount:m_pAdapter", m_pAdapter);
    _RELEASE(m_pAdapter);

#ifdef HAS_DX11_2
    _SHOW_REF("refCount:m_pFactory2", m_pFactory2);
    _RELEASE(m_pFactory2);
#endif

    _SHOW_REF("refCount:m_pFactory", m_pFactory);
    _RELEASE(m_pFactory);
}

void CHW::CreateDevice(SDL_Window* sdlWnd)
{
    CreateD3D();

    m_DriverType = Caps.bForceGPU_REF ? D3D_DRIVER_TYPE_REFERENCE : D3D_DRIVER_TYPE_HARDWARE;

    // Display the name of video board
    DXGI_ADAPTER_DESC1 Desc;
    R_CHK(m_pAdapter->GetDesc1(&Desc));
    //  Warning: Desc.Description is wide string
    Msg("* GPU [vendor:%X]-[device:%X]: %S", Desc.VendorId, Desc.DeviceId, Desc.Description);

    Caps.id_vendor = Desc.VendorId;
    Caps.id_device = Desc.DeviceId;

    UINT createDeviceFlags = 0;

#ifdef DEBUG
    if (xrDebug::DebuggerIsPresent())
        createDeviceFlags |= D3D_CREATE_DEVICE_DEBUG;
#endif

    HRESULT R;

#ifdef USE_DX11
    D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    constexpr auto count = std::size(featureLevels);

    const auto createDevice = [&](const D3D_FEATURE_LEVEL* level, const u32 levels)
    {
        return D3D11CreateDevice(m_pAdapter, D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, createDeviceFlags, level, levels,
            D3D11_SDK_VERSION, &pDevice, &FeatureLevel, &pContext);
    };

    R = createDevice(featureLevels, count);
    if (FAILED(R))
        R = createDevice(&featureLevels[1], count - 1);

    if (FeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        ComputeShadersSupported = true;
    else
    {
        D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS data;
        pDevice->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS,
            &data, sizeof(data));
        ComputeShadersSupported = data.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x;
    }
#else
    R = D3D10CreateDevice(m_pAdapter, m_DriverType, NULL, createDeviceFlags, D3D10_SDK_VERSION, &pDevice);

    pContext = pDevice;
    FeatureLevel = D3D_FEATURE_LEVEL_10_0;
    if (!FAILED(R))
    {
        D3DX10GetFeatureLevel1(pDevice, &pDevice1);
        FeatureLevel = D3D_FEATURE_LEVEL_10_1;
    }
    pContext1 = pDevice1;
#endif

    if (FAILED(R))
    {
        // Fatal error! Cannot create rendering device AT STARTUP !!!
        Msg("Failed to initialize graphics hardware.\n"
            "Please try to restart the game.\n"
            "CreateDevice returned 0x%08x", R);
        xrDebug::DoExit("Failed to initialize graphics hardware.\nPlease try to restart the game.");
    }

    _SHOW_REF("* CREATE: DeviceREF:", pDevice);

#ifdef HAS_DX11_3
    pDevice->QueryInterface(__uuidof(ID3D11Device3), reinterpret_cast<void**>(&pDevice3));
#endif

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);

    R_ASSERT2(SDL_GetWindowWMInfo(sdlWnd, &info), SDL_GetError());

    const HWND hwnd = info.info.win.window;

    if (!CreateSwapChain2(hwnd))
    {
        CreateSwapChain(hwnd);
    }

    //  Create render target and depth-stencil views here
    UpdateViews();

    const auto memory = Desc.DedicatedVideoMemory;
    Msg("*   Texture memory: %d M", memory / (1024 * 1024));
    //Msg("*        DDI-level: %2.1f", float(D3DXGetDriverLevel(pDevice)) / 100.f);
}

void CHW::CreateSwapChain(HWND hwnd)
{
    // Set up the presentation parameters
    DXGI_SWAP_CHAIN_DESC& sd = m_ChainDesc;
    ZeroMemory(&sd, sizeof(sd));

    // Back buffer
    sd.BufferDesc.Width = Device.dwWidth;
    sd.BufferDesc.Height = Device.dwHeight;

    //  TODO: DX10: implement dynamic format selection
    constexpr DXGI_FORMAT formats[] =
    {
        //DXGI_FORMAT_R16G16B16A16_FLOAT, // Do we even need this?
        //DXGI_FORMAT_R10G10B10A2_UNORM, // D3DX11SaveTextureToMemory fails on this format
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };

    // Select back-buffer format
    sd.BufferDesc.Format = SelectFormat(D3D_FORMAT_SUPPORT_DISPLAY, formats);
    Caps.fTarget = dx10TextureUtils::ConvertTextureFormat(sd.BufferDesc.Format);

    // Buffering
    sd.BufferCount = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    // Multisample
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;

    // Windoze
    /* XXX:
       Probably the reason of weird tearing
       glitches reported by Shoker in windowed
       mode with VSync enabled.
       XXX: Fix this windoze stuff!!!
    */
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    sd.OutputWindow = hwnd;

    sd.Windowed = !psDeviceFlags.is(rsFullscreen);

    //  Additional set up
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    R_CHK(m_pFactory->CreateSwapChain(pDevice, &sd, &m_pSwapChain));
}

bool CHW::CreateSwapChain2(HWND hwnd)
{
    if (strstr(Core.Params, "-no_dx11_2"))
        return false;

#ifdef HAS_DX11_2
    m_pAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&m_pFactory2);
    if (!m_pFactory2)
        return false;

    // Set up the presentation parameters
    DXGI_SWAP_CHAIN_DESC1 desc{};

    // Back buffer
    desc.Width = Device.dwWidth;
    desc.Height = Device.dwHeight;

    constexpr DXGI_FORMAT formats[] =
    {
        //DXGI_FORMAT_R16G16B16A16_FLOAT,
        //DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };

    // Select back-buffer format
    desc.Format = SelectFormat(D3D11_FORMAT_SUPPORT_DISPLAY, formats);
    Caps.fTarget = dx10TextureUtils::ConvertTextureFormat(desc.Format);

    // Buffering
    desc.BufferCount = 1; // For DXGI_SWAP_EFFECT_FLIP_DISCARD we need at least two
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    // Multisample
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    // Windoze
    //desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // XXX: tearing glitches with flip presentation model
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fulldesc{};
    fulldesc.Windowed = !psDeviceFlags.is(rsFullscreen);

    // Additional setup
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    IDXGISwapChain1* swapchain = nullptr;
    HRESULT result = m_pFactory2->CreateSwapChainForHwnd(pDevice, hwnd, &desc,
        fulldesc.Windowed ? nullptr : &fulldesc, nullptr, &swapchain);

    if (FAILED(result))
        return false;

    m_pSwapChain = swapchain;
    R_CHK(m_pSwapChain->GetDesc(&m_ChainDesc));

    m_pSwapChain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&m_pSwapChain2));

    if (m_pSwapChain2)
        Device.PresentationFinished = m_pSwapChain2->GetFrameLatencyWaitableObject();

    return true;
#else // #ifdef HAS_DX11_2
    UNUSED(hwnd);
#endif

    return false;
}

void CHW::DestroyDevice()
{
    //  Destroy state managers
    StateManager.Reset();
    RSManager.ClearStateArray();
    DSSManager.ClearStateArray();
    BSManager.ClearStateArray();
    SSManager.ClearStateArray();

    _SHOW_REF("refCount:pBaseZB", pBaseZB);
    _RELEASE(pBaseZB);

    _SHOW_REF("refCount:pBaseRT", pBaseRT);
    _RELEASE(pBaseRT);

    //  Must switch to windowed mode to release swap chain
    if (!m_ChainDesc.Windowed)
        m_pSwapChain->SetFullscreenState(FALSE, NULL);
    _SHOW_REF("refCount:m_pSwapChain", m_pSwapChain);
    _RELEASE(m_pSwapChain);
#ifdef HAS_DX11_2
    _SHOW_REF("refCount:m_pSwapChain2", m_pSwapChain2);
    _RELEASE(m_pSwapChain2);
#endif

#ifdef USE_DX11
    _RELEASE(pContext);
#endif

#ifdef USE_DX10
    _RELEASE(HW.pDevice1);
#endif
    _SHOW_REF("refCount:HW.pDevice:", HW.pDevice);
    _RELEASE(HW.pDevice);

#ifdef HAS_DX11_3
    _SHOW_REF("refCount:HW.pDevice3:", HW.pDevice3);
    _RELEASE(HW.pDevice3);
#endif

    DestroyD3D();
}

//////////////////////////////////////////////////////////////////////
// Resetting device
//////////////////////////////////////////////////////////////////////
void CHW::Reset()
{
    DXGI_SWAP_CHAIN_DESC& cd = m_ChainDesc;
    const bool bWindowed = !psDeviceFlags.is(rsFullscreen);
    cd.Windowed = bWindowed;
    m_pSwapChain->SetFullscreenState(!bWindowed, NULL);
    DXGI_MODE_DESC& desc = m_ChainDesc.BufferDesc;
    desc.Width = Device.dwWidth;
    desc.Height = Device.dwHeight;

    CHK_DX(m_pSwapChain->ResizeTarget(&desc));

    _SHOW_REF("refCount:pBaseZB", pBaseZB);
    _SHOW_REF("refCount:pBaseRT", pBaseRT);
    _RELEASE(pBaseZB);
    _RELEASE(pBaseRT);
    CHK_DX(m_pSwapChain->ResizeBuffers(
        cd.BufferCount, desc.Width, desc.Height, desc.Format, cd.Flags));
    UpdateViews();
}

bool CHW::CheckFormatSupport(const DXGI_FORMAT format, const UINT feature) const
{
    UINT supports;

    if (SUCCEEDED(pDevice->CheckFormatSupport(format, &supports)))
    {
        if (supports & feature)
            return true;
    }

    return false;
}

DXGI_FORMAT CHW::SelectFormat(D3D_FORMAT_SUPPORT feature, const DXGI_FORMAT formats[], size_t count) const
{
    for (size_t i = 0; i < count; ++i)
        if (CheckFormatSupport(formats[i], feature))
            return formats[i];

    return DXGI_FORMAT_UNKNOWN;
}

bool CHW::UsingFlipPresentationModel() const
{
    return m_ChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
#ifdef HAS_DXGI1_4
        || m_ChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD
#endif
    ;
}

void CHW::UpdateViews()
{
    const DXGI_SWAP_CHAIN_DESC& sd = m_ChainDesc;

    HRESULT R;

    // Create a render target view
    ID3DTexture2D* pBuffer;
    R = m_pSwapChain->GetBuffer(0, __uuidof(ID3DTexture2D), (LPVOID*)&pBuffer);
    R_CHK(R);

    R = pDevice->CreateRenderTargetView(pBuffer, NULL, &pBaseRT);
    _RELEASE(pBuffer);
    R_CHK(R);

    //  Create Depth/stencil buffer
    ID3DTexture2D* pDepthStencil = NULL;
    D3D_TEXTURE2D_DESC descDepth;
    descDepth.Width = sd.BufferDesc.Width;
    descDepth.Height = sd.BufferDesc.Height;
    descDepth.MipLevels = 1;
    descDepth.ArraySize = 1;

    // Select depth-stencil format
    constexpr DXGI_FORMAT formats[] =
    {
        DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
        DXGI_FORMAT_D24_UNORM_S8_UINT,
        DXGI_FORMAT_D32_FLOAT,
        DXGI_FORMAT_D16_UNORM
    };
    descDepth.Format = SelectFormat(D3D_FORMAT_SUPPORT_DEPTH_STENCIL, formats);
    Caps.fDepth = dx10TextureUtils::ConvertTextureFormat(descDepth.Format);

    descDepth.SampleDesc.Count = sd.SampleDesc.Count;
    descDepth.SampleDesc.Quality = sd.SampleDesc.Quality;
    descDepth.Usage = D3D_USAGE_DEFAULT;
    descDepth.BindFlags = D3D_BIND_DEPTH_STENCIL;
    descDepth.CPUAccessFlags = 0;
    descDepth.MiscFlags = 0;
    R = pDevice->CreateTexture2D(&descDepth, // Texture desc
        NULL, // Initial data
        &pDepthStencil); // [out] Texture
    R_CHK(R);

    D3D_DEPTH_STENCIL_VIEW_DESC descDSV;
    ZeroMemory(&descDSV, sizeof(descDSV));

    descDSV.Format = descDepth.Format;
    if (descDepth.SampleDesc.Count > 1)
        descDSV.ViewDimension = D3D_DSV_DIMENSION_TEXTURE2DMS;
    else
        descDSV.ViewDimension = D3D_DSV_DIMENSION_TEXTURE2D;
    descDSV.Texture2D.MipSlice = 0;

    //  Create Depth/stencil view
    R = pDevice->CreateDepthStencilView(pDepthStencil, &descDSV, &pBaseZB);
    R_CHK(R);

    _RELEASE(pDepthStencil);
}
