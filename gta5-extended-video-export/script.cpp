// script.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "custom-hooks.h"
#include "script.h"
#include "MFUtility.h"
#include "encoder.h"
#include "logger.h"
#include "config.h"
#include "util.h"
#include "yara-patterns.h"

#include "..\DirectXTex\DirectXTex\DirectXTex.h"
#include "hook-def.h"

using namespace Microsoft::WRL;

namespace {
	std::shared_ptr<PLH::VFuncDetour> hkIMFTransform_ProcessInput(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkIMFTransform_ProcessMessage(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkIMFSinkWriter_AddStream(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkIMFSinkWriter_SetInputMediaType(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkIMFSinkWriter_WriteSample(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkIMFSinkWriter_Finalize(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkOMSetRenderTargets(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkCreateRenderTargetView(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkCreateDepthStencilView(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkCreateTexture2D(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkResizeBuffers(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkSetFullscreenState(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkResizeTarget(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkGetBuffer(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkRSSetViewports(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkRSSetScissorRects(new PLH::VFuncDetour);
	

	std::shared_ptr<PLH::IATHook> hkCoCreateInstance(new PLH::IATHook);
	std::shared_ptr<PLH::IATHook> hkMFCreateSinkWriterFromURL(new PLH::IATHook);

	std::unique_ptr<Encoder::Session> session;
	std::mutex mxSession;

	std::thread::id mainThreadId;
	ComPtr<IDXGISwapChain> mainSwapChain;

	ComPtr<IDXGIFactory> pDXGIFactory;

	//ComPtr<IDXGISwapChain> pHiddenSwapChain;
	ComPtr<ID3D11Texture2D> pHiddenBuffer;
	bool swapHiddenBuffer = false;
	//std::shared_ptr<HWND__> pHiddenWindow;

	struct ExportContext {

		ExportContext() :
			pHiddenWindow(nullptr, [](HWND handle) {if (handle) ::SendMessageA(handle, WM_CLOSE, NULL, NULL); })
		{
			PRE();			
			POST();
		}

		~ExportContext() {
			PRE();
			POST();
		}

		HRESULT attachSwapChain(ComPtr<IDXGISwapChain> swapChain) {
			try {
				//REQUIRE(swapChain->ResizeBuffers(0, width, height, desc.BufferDesc.Format, desc.Flags), "Could not resize the hidden swap buffer.");
				this->pHiddenSwapChain = swapChain;
			} catch (std::exception& ex) {
				LOG(LL_ERR, ex.what());
				return E_FAIL;
			}
			return S_OK;
		}

		bool captureRenderTargetViewReference = false;
		bool captureDepthStencilViewReference = false;
		
		ComPtr<ID3D11RenderTargetView> pExportRenderTargetView;
		ComPtr<ID3D11Texture2D> pExportRenderTarget;
		ComPtr<ID3D11Texture2D> pExportDepthStencil;
		ComPtr<ID3D11DeviceContext> pDeviceContext;
		ComPtr<ID3D11Device> pDevice;
		ComPtr<IDXGIFactory> pDXGIFactory;
		ComPtr<IDXGISwapChain> pHiddenSwapChain;
		std::shared_ptr<HWND__> pHiddenWindow;

		std::shared_ptr<DirectX::ScratchImage> latestImage = std::make_shared<DirectX::ScratchImage>();
		//DirectX::ScratchImage latestImage;

		/*UINT width;
		UINT height;*/
		//UINT outputWidth;
		//UINT outputHeight;

		UINT pts = 0;

		ComPtr<IMFMediaType> videoMediaType;
	};

	std::shared_ptr<ExportContext> exportContext;
	YR_COMPILER* pYrCompiler;
}

tCoCreateInstance oCoCreateInstance;
tMFCreateSinkWriterFromURL oMFCreateSinkWriterFromURL;
tIMFSinkWriter_SetInputMediaType oIMFSinkWriter_SetInputMediaType;
tIMFSinkWriter_AddStream oIMFSinkWriter_AddStream;
tIMFSinkWriter_WriteSample oIMFSinkWriter_WriteSample;
tIMFSinkWriter_Finalize oIMFSinkWriter_Finalize;
tOMSetRenderTargets oOMSetRenderTargets;
tCreateTexture2D oCreateTexture2D;
tCreateRenderTargetView oCreateRenderTargetView;
tCreateDepthStencilView oCreateDepthStencilView;
tRSSetViewports oRSSetViewports;
tRSSetScissorRects oRSSetScissorRects;
tGetBuffer oGetBuffer;
tResizeBuffers oResizeBuffers;
tResizeTarget oResizeTarget;
tSetFullscreenState oSetFullscreenState;

HRESULT createHiddenBuffer(IDXGISwapChain *pSwapChain) {
	try {
		ComPtr<ID3D11Device> pDevice;
		ComPtr<ID3D11Texture2D> pTexture;
		REQUIRE(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)pDevice.GetAddressOf()), "Failed to get ID3D11Device from IDXGISwapBuffer");
		REQUIRE(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)pTexture.GetAddressOf()), "Failed to get ID3D11Texture2D from IDXGISwapBuffer");
		D3D11_TEXTURE2D_DESC desc;
		pTexture->GetDesc(&desc);
		REQUIRE(pDevice->CreateTexture2D(&desc, NULL, pHiddenBuffer.GetAddressOf()), "Failed to create back buffer ID3D11Texture2D");
		LOG(LL_DBG, "Back buffer created: w:", desc.Width, " h:", desc.Height);
	} catch (std::exception& ex) {
		LOG(LL_ERR, ex.what());
	}

	return S_OK;
}

void avlog_callback(void *ptr, int level, const char* fmt, va_list vargs) {
	static char msg[8192];
	vsnprintf_s(msg, sizeof(msg), fmt, vargs);
	Logger::instance().write(
		Logger::instance().getTimestamp(),
		" ",
		Logger::instance().getLogLevelString(LL_NON),
		" ",
		Logger::instance().getThreadId(), " AVCODEC: ");
	if (ptr)
	{
		AVClass *avc = *(AVClass**)ptr;
		Logger::instance().write("(");
		Logger::instance().write(avc->item_name(ptr));
		Logger::instance().write("): ");
	}
	Logger::instance().write(msg);
}

void onPresent(IDXGISwapChain *swapChain) {
	mainSwapChain = swapChain;
	static bool initialized = false;
	if (!initialized) {
		initialized = true;
		try {
			ComPtr<ID3D11Device> pDevice;
			ComPtr<ID3D11DeviceContext> pDeviceContext;
			ComPtr<ID3D11Texture2D> texture;
			DXGI_SWAP_CHAIN_DESC desc;


			swapChain->GetDesc(&desc);


			LOG(LL_NFO, "BUFFER COUNT: ", desc.BufferCount);
			REQUIRE(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)texture.GetAddressOf()), "Failed to get the texture buffer");
			//pPrimaryRenderTarget = texture;
			REQUIRE(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)pDevice.GetAddressOf()), "Failed to get the D3D11 device");
			pDevice->GetImmediateContext(pDeviceContext.GetAddressOf());
			NOT_NULL(pDeviceContext.Get(), "Failed to get D3D11 device context");
			REQUIRE(hookVirtualFunction(pDevice.Get(), 5, &Hook_CreateTexture2D, &oCreateTexture2D, hkCreateTexture2D), "Failed to hook ID3DDeviceContext::Map");
			REQUIRE(hookVirtualFunction(pDevice.Get(), 9, &Hook_CreateRenderTargetView, &oCreateRenderTargetView, hkCreateRenderTargetView), "Failed to hook ID3DDeviceContext::CreateRenderTargetView");
			REQUIRE(hookVirtualFunction(pDevice.Get(), 10, &Hook_CreateDepthStencilView, &oCreateDepthStencilView, hkCreateDepthStencilView), "Failed to hook ID3DDeviceContext::CreateDepthStencilView");
			REQUIRE(hookVirtualFunction(pDeviceContext.Get(), 33, &Hook_OMSetRenderTargets, &oOMSetRenderTargets, hkOMSetRenderTargets), "Failed to hook ID3DDeviceContext::OMSetRenderTargets");
			REQUIRE(hookVirtualFunction(pDeviceContext.Get(), 44, &RSSetViewports, &oRSSetViewports, hkRSSetViewports), "Failed to hook ID3DDeviceContext::RSSetViewports"); 
			REQUIRE(hookVirtualFunction(pDeviceContext.Get(), 45, &RSSetScissorRects, &oRSSetScissorRects, hkRSSetScissorRects), "Failed to hook ID3DDeviceContext::RSSetViewports"); 


			/*D3D11_TEXTURE2D_DESC backbufferDesc;
			texture->GetDesc(&backbufferDesc);
			REQUIRE(pDevice->CreateTexture2D(&backbufferDesc, NULL, pHiddenBuffer.GetAddressOf()), "Failed to create back buffer texture");*/
			//LOG(LL_DBG, "Back buffer created: w:", desc.Width, " h:", desc.Height);
			createHiddenBuffer(swapChain);

			//createHiddenBuffer(pDevice, desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format);
			//pHiddenWindow.reset(::CreateWindowA("STATIC", "dummy", 0, 0, 0, 1, 1, NULL, NULL, NULL, NULL));
			/*DXGI_SWAP_CHAIN_DESC newDesc = desc;
			newDesc.OutputWindow = pHiddenWindow.get();*/
			//desc.Flags = 0;
			/*newDesc.BufferCount = 1;
			newDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;*/
			//REQUIRE(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)factory.GetAddressOf()), "Failed to create IDXGIFactory instance.");
			
			REQUIRE(hookVirtualFunction(swapChain, 9, &Hook_GetBuffer, &oGetBuffer, hkGetBuffer), "Failed to hook DXGISwapBuffer::GetBuffer");
			REQUIRE(hookVirtualFunction(swapChain, 10, &Hook_SetFullscreenState, &oSetFullscreenState, hkSetFullscreenState), "Failed to hook DXGISwapBuffer::SetFullscreenState");
			REQUIRE(hookVirtualFunction(swapChain, 13, &Hook_ResizeBuffers, &oResizeBuffers, hkResizeBuffers), "Failed to hook DXGISwapBuffer::ResizeBuffers");
			REQUIRE(hookVirtualFunction(swapChain, 14, &Hook_ResizeTarget, &oResizeTarget, hkResizeTarget), "Failed to hook DXGISwapBuffer::ResizeTarget");

			ComPtr<IDXGIDevice> pDXGIDevice;
			REQUIRE(pDevice.As(&pDXGIDevice), "Failed to get IDXGIDevice from ID3D11Device");
			
			ComPtr<IDXGIAdapter> pDXGIAdapter;
			REQUIRE(pDXGIDevice->GetParent(__uuidof(IDXGIAdapter), (void**)pDXGIAdapter.GetAddressOf()), "Failed to get IDXGIAdapter");

			REQUIRE(pDXGIAdapter->GetParent(__uuidof(IDXGIFactory), (void**)pDXGIFactory.GetAddressOf()), "Failed to get IDXGIFactory");

			//REQUIRE(pDXGIFactory->CreateSwapChain(pDevice.Get(), &newDesc, pHiddenSwapChain.GetAddressOf()), "Failed to create hidden swap chain.");

			//REQUIRE(swapChain->SetFullscreenState(desc.Windowed ? FALSE : TRUE, NULL), "Failed to revert fullscreen status of original swap chain")

			//REQUIRE(pHiddenSwapChain->Present(0, DXGI_PRESENT_TEST), "Failed to present the hidden swap chain.");

		} catch (std::exception& ex) {
			LOG(LL_ERR, ex.what());
		}
	}

	// FIXME: This is a hack that makes sure that ReShade is initialized before exporting video
	/*if (pHiddenSwapChain) {
		pHiddenSwapChain->Present(0, DXGI_PRESENT_TEST);
	}*/
}

void initialize() {
	PRE();
	try {
		mainThreadId = std::this_thread::get_id();

		REQUIRE(hookNamedFunction("mfreadwrite.dll", "MFCreateSinkWriterFromURL", &Hook_MFCreateSinkWriterFromURL, &oMFCreateSinkWriterFromURL, hkMFCreateSinkWriterFromURL), "Failed to hook MFCreateSinkWriterFromURL in mfreadwrite.dll");
		REQUIRE(hookNamedFunction("ole32.dll", "CoCreateInstance", &Hook_CoCreateInstance, &oCoCreateInstance, hkCoCreateInstance), "Failed to hook CoCreateInstance in ole32.dll");

			
		LOG_CALL(LL_DBG, yr_initialize());
		REQUIRE(yr_compiler_create(&pYrCompiler), "Failed to create yara compiler.");
		REQUIRE(yr_compiler_add_string(pYrCompiler, yara_resolution_fields.c_str(), NULL), "Failed to compile yara rule");
		YR_RULES *rules;
		REQUIRE(yr_compiler_get_rules(pYrCompiler, &rules), "Failed to get yara rules");
		//yr_rules_scan_mem(rules, )

		LOG_CALL(LL_DBG, av_register_all());
		LOG_CALL(LL_DBG, avcodec_register_all());
		LOG_CALL(LL_DBG, av_log_set_level(AV_LOG_TRACE));
		LOG_CALL(LL_DBG, av_log_set_callback(&avlog_callback));
	} catch (std::exception& ex) {
		// TODO cleanup
		POST();
		throw ex;
	}
	POST();
}

void ScriptMain() {
	PRE();
	LOG(LL_NFO, "Starting main loop");
	while (true) {
		WAIT(0);
	}
	POST();
}

static HRESULT Hook_CoCreateInstance(
	REFCLSID  rclsid,
	LPUNKNOWN pUnkOuter,
	DWORD     dwClsContext,
	REFIID    riid,
	LPVOID    *ppv
	) {
	PRE();
	HRESULT result = oCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
	char buffer[64];
	GUIDToString(rclsid, buffer, 64);
	LOG(LL_NFO, "CoCreateInstance: ", buffer);
	POST();
	return result;
}

static HRESULT Hook_ResizeTarget(
	IDXGISwapChain       *pThis,
	const DXGI_MODE_DESC *pNewTargetParameters
	) {
	PRE();
	if (exportContext != NULL) {
		LOG(LL_NON, "BYPASS");
		POST();
		return S_OK;
	}

	POST();
	return oResizeTarget(pThis, pNewTargetParameters);
}

static HRESULT Hook_GetBuffer(
	IDXGISwapChain *pThis,
	UINT           Buffer,
	REFIID         riid,
	void           **ppSurface
	) {
	if (swapHiddenBuffer) {
		(*ppSurface) = pHiddenBuffer.Get();
		return S_OK;
	}
	return oGetBuffer(pThis, Buffer, riid, ppSurface);
}

static HRESULT Hook_SetFullscreenState(
	IDXGISwapChain *pThis,
	BOOL           Fullscreen,
	IDXGIOutput    *pTarget
	) {
	PRE();
	/*if (exportContext != NULL) {
		LOG(LL_NON, "BYPASS");
		POST();
		return S_OK;
	}*/

	/*if (pThis != pHiddenSwapChain.Get()) {
		oSetFullscreenState(pHiddenSwapChain.Get(), Fullscreen, NULL);
	}*/

	POST();
	return oSetFullscreenState(pThis, Fullscreen, pTarget);
}

static HRESULT Hook_ResizeBuffers(
	IDXGISwapChain* pThis,
	UINT			BufferCount,
	UINT			Width,
	UINT			Height,
	DXGI_FORMAT		NewFormat,
	UINT			SwapChainFlags
	) {
	PRE();
	//if (exportContext != NULL) {
	//	LOG(LL_NON, "BYPASS");
	//	POST();
	//	return S_OK;
	//}

	//try {
	//	if (pThis != pHiddenSwapChain.Get()) {
	//		REQUIRE(oResizeBuffers(pHiddenSwapChain.Get(), 1, Width, Height, NewFormat, SwapChainFlags), "Failed to resize hidden swap chain buffers");
	//		//REQUIRE(pHiddenSwapChain->Present(0, DXGI_PRESENT_TEST), "Failed to present the hidden swap chain.");
	//	}
	//} catch (std::exception& ex) {
	//	LOG(LL_ERR, ex.what());
	//}

	

	HRESULT result = oResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);

	if (SUCCEEDED(result)) {
		try {
			//ComPtr<ID3D11Device> pDevice;
			//REQUIRE(pThis->GetDevice(__uuidof(ID3D11Device), (void**)pDevice.GetAddressOf()), "Failed to get ID3D11Device from IDXGISwapChain");
			createHiddenBuffer(pThis);
		}
		catch (std::exception& ex) {
			LOG(LL_ERR, ex.what());
		}
	}

	POST();
	return result;
}

static HRESULT Hook_CreateRenderTargetView(
	ID3D11Device                  *pThis,
	ID3D11Resource                *pResource,
	const D3D11_RENDER_TARGET_VIEW_DESC *pDesc,
	ID3D11RenderTargetView        **ppRTView
	) {
	//PRE();
	if ((exportContext != NULL) && exportContext->captureRenderTargetViewReference && (std::this_thread::get_id() == mainThreadId)) {
		try {
			LOG(LL_NFO, "Capturing export render target view...");
			exportContext->captureRenderTargetViewReference = false;
			exportContext->pDevice = pThis;
			exportContext->pDevice->GetImmediateContext(exportContext->pDeviceContext.GetAddressOf());

			ComPtr<ID3D11Texture2D> pOldTexture;
			REQUIRE(pResource->QueryInterface(pOldTexture.GetAddressOf()), "Failed to convert ID3D11Resource to ID3D11Texture2D");
			D3D11_TEXTURE2D_DESC desc;
			pOldTexture->GetDesc(&desc);

			DXGI_SWAP_CHAIN_DESC swapChainDesc;
			mainSwapChain->GetDesc(&swapChainDesc);

			desc.Width = swapChainDesc.BufferDesc.Width;
			desc.Height = swapChainDesc.BufferDesc.Height;

			LOG(LL_DBG, "Creating render target view: w:", desc.Width, " h:", desc.Height);

			ComPtr<ID3D11Texture2D> pTexture;
			pThis->CreateTexture2D(&desc, NULL, pTexture.GetAddressOf());
			ComPtr<ID3D11Resource> pRes;
			REQUIRE(pTexture.As(&pRes), "Failed to convert ID3D11Texture2D to ID3D11Resource");
			HRESULT result = oCreateRenderTargetView(pThis, pRes.Get(), pDesc, ppRTView);
			exportContext->pExportRenderTarget = pTexture;
			exportContext->pExportRenderTargetView = *(ppRTView);
			POST();
			return result;
		} catch (std::exception& ex) {
			LOG(LL_ERR, ex.what());
		}
	}
	//POST();
	return oCreateRenderTargetView(pThis, pResource, pDesc, ppRTView);
}

static HRESULT Hook_CreateDepthStencilView(
	ID3D11Device                  *pThis,
	ID3D11Resource                *pResource,
	const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
	ID3D11DepthStencilView        **ppDepthStencilView
	) {
	if ((exportContext != NULL) && exportContext->captureDepthStencilViewReference && mainThreadId == std::this_thread::get_id()) {
		try {
			exportContext->captureDepthStencilViewReference = false;
			LOG(LL_NFO, "Capturing export depth stencil view...");
			ComPtr<ID3D11Texture2D> pOldTexture;
			REQUIRE(pResource->QueryInterface(pOldTexture.GetAddressOf()), "Failed to get depth stencil texture");
			D3D11_TEXTURE2D_DESC desc;
			pOldTexture->GetDesc(&desc);
			/*if ((exportContext->width != 0) && (exportContext->height != 0)) {
				desc.Width = exportContext->width;
				desc.Height = exportContext->height;
			}*/
			ComPtr<ID3D11Texture2D> pTexture;
			pThis->CreateTexture2D(&desc, NULL, pTexture.GetAddressOf());

			ComPtr<ID3D11Resource> pRes;
			REQUIRE(pTexture.As(&pRes), "Failed to convert ID3D11Texture to ID3D11Resource.");
			exportContext->pExportDepthStencil = pTexture;
			return oCreateDepthStencilView(pThis, pRes.Get(), pDesc, ppDepthStencilView);
		} catch (...) {
			LOG(LL_ERR, "Failed to capture depth stencil view");
		}
	}

	HRESULT result = oCreateDepthStencilView(pThis, pResource, pDesc, ppDepthStencilView);
	return result;
}

static void RSSetViewports(
	ID3D11DeviceContext  *pThis,
	UINT                 NumViewports,
	const D3D11_VIEWPORT *pViewports
	) {
	if ((exportContext != NULL) && isCurrentRenderTargetView(pThis, exportContext->pExportRenderTargetView)) {
		D3D11_VIEWPORT* pNewViewports = new D3D11_VIEWPORT[NumViewports];
		for (UINT i = 0; i < NumViewports; i++) {
			pNewViewports[i] = pViewports[i];
			DXGI_SWAP_CHAIN_DESC desc;
			exportContext->pHiddenSwapChain->GetDesc(&desc);
			pNewViewports[i].Width = desc.BufferDesc.Width;
			pNewViewports[i].Height = desc.BufferDesc.Height;
			//if ((exportContext->width != 0) && (exportContext->height != 0)) {
			//	/*pNewViewports[i].Width = (float)exportContext->width;
			//	pNewViewports[i].Height = (float)exportContext->height;*/
			//}
		}
		return oRSSetViewports(pThis, NumViewports, pNewViewports);
	}

	return oRSSetViewports(pThis, NumViewports, pViewports);
}

static void RSSetScissorRects(
	ID3D11DeviceContext *pThis,
	UINT          NumRects,
	const D3D11_RECT *  pRects
	) {
	if ((exportContext != NULL) && isCurrentRenderTargetView(pThis, exportContext->pExportRenderTargetView)) {
		D3D11_RECT* pNewRects = new D3D11_RECT[NumRects];
		for (UINT i = 0; i < NumRects; i++) {
			pNewRects[i] = pRects[i];
			DXGI_SWAP_CHAIN_DESC desc;
			exportContext->pHiddenSwapChain->GetDesc(&desc);
			pNewRects[i].right = desc.BufferDesc.Width;
			pNewRects[i].bottom = desc.BufferDesc.Height;
			//if ((exportContext->width != 0) && (exportContext->height != 0)) {
			//	/*pNewRects[i].right  = exportContext->width;
			//	pNewRects[i].bottom = exportContext->height;*/
			//}
		}
		return oRSSetScissorRects(pThis, NumRects, pNewRects);
	}

	return oRSSetScissorRects(pThis, NumRects, pRects);
}

static void Hook_OMSetRenderTargets(
	ID3D11DeviceContext           *pThis,
	UINT                          NumViews,
	ID3D11RenderTargetView *const *ppRenderTargetViews,
	ID3D11DepthStencilView        *pDepthStencilView
	) {
	if ((exportContext != NULL) && isCurrentRenderTargetView(pThis, exportContext->pExportRenderTargetView)) {
		std::lock_guard<std::mutex> sessionLock(mxSession);
		if ((session != NULL) && (session->isCapturing)) {
			// Time to capture rendered frame
			try {
				NOT_NULL(exportContext, "No export context detected! Cannot capture lossless frame.");

				//ComPtr<ID3D11Texture2D> pHiddenSwapChainBuffer;
				//exportContext->pHiddenSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)pHiddenSwapChainBuffer.GetAddressOf());
				swapHiddenBuffer = true;
				exportContext->pDeviceContext->CopyResource(pHiddenBuffer.Get(), exportContext->pExportRenderTarget.Get());
				exportContext->pHiddenSwapChain->Present(0, DXGI_PRESENT_TEST);
				swapHiddenBuffer = false;

				auto& image_ref = *(exportContext->latestImage);
				LOG_CALL(LL_DBG, DirectX::CaptureTexture(exportContext->pDevice.Get(), exportContext->pDeviceContext.Get(), pHiddenBuffer.Get(), image_ref));
				if (exportContext->latestImage->GetImageCount() == 0) {
					LOG(LL_ERR, "There is no image to capture.");
					throw std::exception();
				}
				const DirectX::Image* image = exportContext->latestImage->GetImage(0, 0, 0);
				NOT_NULL(image, "Could not get current frame.");
				NOT_NULL(image->pixels, "Could not get current frame.");

				REQUIRE(session->enqueueVideoFrame(image->pixels, (int)(image->width * image->height * 4), exportContext->pts++), "Failed to enqueue frame");
				exportContext->latestImage->Release();
			} catch (std::exception&) {
				LOG(LL_ERR, "Reading video frame from D3D Device failed.");
				exportContext->latestImage->Release();
				session.reset();
				exportContext.reset();
			}
		}
	}
	oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView);
}

static HRESULT Hook_MFCreateSinkWriterFromURL(
	LPCWSTR       pwszOutputURL,
	IMFByteStream *pByteStream,
	IMFAttributes *pAttributes,
	IMFSinkWriter **ppSinkWriter
	) {
	PRE();
	HRESULT result = oMFCreateSinkWriterFromURL(pwszOutputURL, pByteStream, pAttributes, ppSinkWriter);
	if (SUCCEEDED(result)) {
		try {
			REQUIRE(hookVirtualFunction(*ppSinkWriter, 3, &Hook_IMFSinkWriter_AddStream, &oIMFSinkWriter_AddStream, hkIMFSinkWriter_AddStream), "Failed to hook IMFSinkWriter::AddStream");
			REQUIRE(hookVirtualFunction(*ppSinkWriter, 4, &IMFSinkWriter_SetInputMediaType, &oIMFSinkWriter_SetInputMediaType, hkIMFSinkWriter_SetInputMediaType), "Failed to hook IMFSinkWriter::SetInputMediaType");
			REQUIRE(hookVirtualFunction(*ppSinkWriter, 6, &Hook_IMFSinkWriter_WriteSample, &oIMFSinkWriter_WriteSample, hkIMFSinkWriter_WriteSample), "Failed to hook IMFSinkWriter::WriteSample");
			REQUIRE(hookVirtualFunction(*ppSinkWriter, 11, &Hook_IMFSinkWriter_Finalize, &oIMFSinkWriter_Finalize, hkIMFSinkWriter_Finalize), "Failed to hook IMFSinkWriter::Finalize");
		} catch (...) {
			LOG(LL_ERR, "Hooking IMFSinkWriter functions failed");
		}
	}
	POST();
	return result;
}

static HRESULT Hook_IMFSinkWriter_AddStream(
	IMFSinkWriter *pThis,
	IMFMediaType  *pTargetMediaType,
	DWORD         *pdwStreamIndex
	) {
	PRE();
	LOG(LL_NFO, "IMFSinkWriter::AddStream: ", GetMediaTypeDescription(pTargetMediaType).c_str());
	POST();
	return oIMFSinkWriter_AddStream(pThis, pTargetMediaType, pdwStreamIndex);
}

static HRESULT IMFSinkWriter_SetInputMediaType(
	IMFSinkWriter *pThis,
	DWORD         dwStreamIndex,
	IMFMediaType  *pInputMediaType,
	IMFAttributes *pEncodingParameters
	) {
	PRE();
	LOG(LL_NFO, "IMFSinkWriter::SetInputMediaType: ", GetMediaTypeDescription(pInputMediaType).c_str());

	GUID majorType;
	if (SUCCEEDED(pInputMediaType->GetMajorType(&majorType))) {
		if (IsEqualGUID(majorType, MFMediaType_Video)) {
			exportContext->videoMediaType = pInputMediaType;
		} else if (IsEqualGUID(majorType, MFMediaType_Audio)) {
			try {
				std::lock_guard<std::mutex> sessionLock(mxSession);

				// Create Video Context
				{
					UINT width, height, fps_num, fps_den;
					MFGetAttribute2UINT32asUINT64(exportContext->videoMediaType.Get(), MF_MT_FRAME_SIZE, &width, &height);
					MFGetAttributeRatio(exportContext->videoMediaType.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den);

					GUID pixelFormat;
					exportContext->videoMediaType->GetGUID(MF_MT_SUBTYPE, &pixelFormat);

					DXGI_SWAP_CHAIN_DESC desc;
					exportContext->pHiddenSwapChain->GetDesc(&desc);

					REQUIRE(session->createVideoContext(desc.BufferDesc.Width, desc.BufferDesc.Height, "bgra", fps_num, fps_den, Config::instance().videoFmt(), Config::instance().videoEnc(), Config::instance().videoCfg()), "Failed to create video context");
				}
				
				// Create Audio Context
				{
					UINT32 blockAlignment, numChannels, sampleRate, bitsPerSample;
					GUID subType;

					pInputMediaType->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &blockAlignment);
					pInputMediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &numChannels);
					pInputMediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
					pInputMediaType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample);
					pInputMediaType->GetGUID(MF_MT_SUBTYPE, &subType);

					if (IsEqualGUID(subType, MFAudioFormat_PCM)) {
						REQUIRE(session->createAudioContext(numChannels, sampleRate, bitsPerSample, AV_SAMPLE_FMT_S16, blockAlignment, Config::instance().audioRate(), Config::instance().audioFmt(), Config::instance().audioEnc(), Config::instance().audioCfg()), "Failed to create audio context.");
					} else {
						char buffer[64];
						GUIDToString(subType, buffer, 64);
						LOG(LL_ERR, "Unsupported input audio format: ", buffer);
						throw std::runtime_error("Unsupported input audio format");
					}
				}

				// Create Format Context
				{
					char buffer[128];
					std::string output_file = Config::instance().outputDir();

					output_file += "\\XVX-";
					time_t rawtime;
					struct tm timeinfo;
					time(&rawtime);
					localtime_s(&timeinfo, &rawtime);
					strftime(buffer, 128, "%Y%m%d%H%M%S", &timeinfo);
					output_file += buffer;
					output_file += ".mkv";

					std::string filename = std::regex_replace(output_file, std::regex("\\\\+"), "\\");

					LOG(LL_NFO, "Output file: ", filename);

					REQUIRE(session->createFormatContext(filename.c_str()), "Failed to create format context");
				}
			} catch (std::exception&) {
				session.reset();
				exportContext.reset();
			}
		}
	}

	POST();
	return oIMFSinkWriter_SetInputMediaType(pThis, dwStreamIndex, pInputMediaType, pEncodingParameters);
}

static HRESULT Hook_IMFSinkWriter_WriteSample(
	IMFSinkWriter *pThis,
	DWORD         dwStreamIndex,
	IMFSample     *pSample
	) {
	std::lock_guard<std::mutex> sessionLock(mxSession);
	if ((session != NULL) && (dwStreamIndex == 1)) {
		ComPtr<IMFMediaBuffer> pBuffer = NULL;
		try {
			LONGLONG sampleTime;
			pSample->GetSampleTime(&sampleTime);
			pSample->ConvertToContiguousBuffer(pBuffer.GetAddressOf());

			DWORD length;
			pBuffer->GetCurrentLength(&length);
			BYTE *buffer;
			if (SUCCEEDED(pBuffer->Lock(&buffer, NULL, NULL))) {
				LOG_CALL(LL_DBG, session->writeAudioFrame(buffer, length, sampleTime));
			}
			
		} catch (std::exception& ex) {
			LOG(LL_ERR, ex.what());
			session.reset();
			exportContext.reset();
			if (pBuffer != NULL) {
				pBuffer->Unlock();
			}
		}
	}
	return S_OK;
}

static HRESULT Hook_IMFSinkWriter_Finalize(
	IMFSinkWriter *pThis
	) {
	PRE();
	std::lock_guard<std::mutex> sessionLock(mxSession);
	try {
		if (session != NULL) {
			LOG_CALL(LL_DBG, session->finishAudio());
			LOG_CALL(LL_DBG, session->finishVideo());
			LOG_CALL(LL_DBG, session->endSession());
		}
	} catch (std::exception& ex) {
		LOG(LL_ERR, ex.what());
	}

	session.reset();
	exportContext.reset();
	POST();
	return S_OK;
}

void finalize() {
	PRE();
	LOG_CALL(LL_DBG, yr_compiler_destroy(pYrCompiler));
	LOG_CALL(LL_DBG, yr_finalize());
	hkCoCreateInstance->UnHook();
	hkMFCreateSinkWriterFromURL->UnHook();
	hkIMFTransform_ProcessInput->UnHook();
	hkIMFTransform_ProcessMessage->UnHook();
	hkIMFSinkWriter_AddStream->UnHook();
	hkIMFSinkWriter_SetInputMediaType->UnHook();
	hkIMFSinkWriter_WriteSample->UnHook();
	hkIMFSinkWriter_Finalize->UnHook();
	POST();
}

static HRESULT Hook_CreateTexture2D(
	ID3D11Device*				 *pThis,
	const D3D11_TEXTURE2D_DESC   *pDesc,
	const D3D11_SUBRESOURCE_DATA *pInitialData,
	ID3D11Texture2D        **ppTexture2D
	) {
	// Detect export buffer creation
	if (pDesc && (pDesc->CPUAccessFlags & D3D11_CPU_ACCESS_READ) && (std::this_thread::get_id() == mainThreadId) && !((pDesc->Width == 512) && (pDesc->Height == 256)) && !((pDesc->Width == 4) && (pDesc->Height == 4))) {
		std::lock_guard<std::mutex> sessionLock(mxSession);
		exportContext.reset();
		session.reset();
		try {
			LOG(LL_NFO, "Creating session...");
			LOG(LL_NFO, " fmt:", conv_dxgi_format_to_string(pDesc->Format),
				" w:", pDesc->Width,
				" h:", pDesc->Height);
			if (Config::instance().isAutoReloadEnabled()) {
				LOG_CALL(LL_DBG,Config::instance().reload());
			}


			session.reset(new Encoder::Session());
			NOT_NULL(session, "Could not create the session");
			exportContext.reset(new ExportContext());
			exportContext->attachSwapChain(mainSwapChain);
			exportContext->captureRenderTargetViewReference = true;
			exportContext->captureDepthStencilViewReference = true;
			/*exportContext->width = Config::instance().exportResolution().first;
			exportContext->height = Config::instance().exportResolution().second;*/
			/*exportContext->width = 800;
			exportContext->height = 600;*/
			D3D11_TEXTURE2D_DESC desc = *(pDesc);
			DXGI_SWAP_CHAIN_DESC swapChainDesc;
			exportContext->pHiddenSwapChain->GetDesc(&swapChainDesc);
			desc.Width = swapChainDesc.BufferDesc.Width;
			desc.Height = swapChainDesc.BufferDesc.Height;
			/*desc.Width = exportContext->width;
			desc.Height = exportContext->height;*/
			return oCreateTexture2D(pThis, &desc, pInitialData, ppTexture2D);
		} catch (std::exception&) {
			session.reset();
			exportContext.reset();
		}
	}

	return oCreateTexture2D(pThis, pDesc, pInitialData, ppTexture2D);
}