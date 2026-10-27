#include "DirectXCommon.h"
#include "WinApp.h"
#include "D3D12Util.h" 
#include <cassert>
#include <format>
#include <string>
#include <fstream>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

// main.cppで定義されている関数のプロトタイプ宣言
std::string ConvertString(const std::wstring& str);
void Log(std::ostream& os, const std::string& message);

DirectXCommon* DirectXCommon::GetInstance() {
    static DirectXCommon instance;
    return &instance;
}

void DirectXCommon::Initialize(WinApp* winApp) {
    winApp_ = winApp;
#ifdef _DEBUG
    Microsoft::WRL::ComPtr<ID3D12Debug1> debugController = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
#ifdef _DEBUG
        // debugController->SetEnableGPUBasedValidation(TRUE); // GPU-Based Validationは描画負荷が極めて高くフレームレートが著しく低下するため無効化
#endif
    }

#endif

    HRESULT hr = CreateDXGIFactory(IID_PPV_ARGS(&dxgiFactory_));
    assert(SUCCEEDED(hr));

    InitializeWithAdapter(0);

    // ビューポートとシザー矩形の設定
    viewport_.Width = static_cast<float>(winApp->kClientWidth);
    viewport_.Height = static_cast<float>(winApp->kClientHeight);
    viewport_.TopLeftX = 0;
    viewport_.TopLeftY = 0;
    viewport_.MinDepth = 0.0f;
    viewport_.MaxDepth = 1.0f;

    scissorRect_.left = 0;
    scissorRect_.right = winApp->kClientWidth;
    scissorRect_.top = 0;
    scissorRect_.bottom = winApp->kClientHeight;
}

void DirectXCommon::Finalize() {
    // GPUの処理完了を待つ
    commandQueue_->Signal(fence_.Get(), ++fenceValue_);
    if (fence_->GetCompletedValue() < fenceValue_) {
        fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
    CloseHandle(fenceEvent_);
}


void DirectXCommon::PreDraw() {
    UINT backBufferIndex = swapChain_->GetCurrentBackBufferIndex();

    // 深度バッファをDEPTH_WRITE状態へ安全に遷移（まだDEPTH_WRITEの場合はスキップ）
    TransitionDepthStencilState(D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // TransitionBarrierの設定
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = backBuffers_[backBufferIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // TransitionBarrierを張る
    commandList_->ResourceBarrier(1, &barrier);

    // 描画先のRTVとDSVを設定する
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart();
    commandList_->OMSetRenderTargets(1, &rtvHandles_[backBufferIndex], false, &dsvHandle);

    // 指定した色で画面全体をクリアする
    commandList_->ClearRenderTargetView(rtvHandles_[backBufferIndex], clearColor_, 0, nullptr);

    // 深度バッファをクリア
    commandList_->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    commandList_->RSSetViewports(1, &viewport_);
    commandList_->RSSetScissorRects(1, &scissorRect_);
}

void DirectXCommon::PostDraw() {
    HRESULT hr;
    UINT backBufferIndex = swapChain_->GetCurrentBackBufferIndex();

    // 画面に表示するリソースを遷移
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition.pResource = backBuffers_[backBufferIndex].Get();
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &barrier);

    // コマンドリストをクローズ
    hr = commandList_->Close();
    assert(SUCCEEDED(hr));

    // GPUにコマンドリストの実行を行わせる
    ID3D12CommandList* commandLists[] = { commandList_.Get() };
    commandQueue_->ExecuteCommandLists(1, commandLists);

    // 画面に表示
    swapChain_->Present(1, 0);

    // Fenceの値を更新
    fenceValue_++;
    commandQueue_->Signal(fence_.Get(), fenceValue_);

    // 次のフレームの準備
    if (fence_->GetCompletedValue() < fenceValue_) {
        fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }

    hr = commandAllocator_->Reset();
    assert(SUCCEEDED(hr));
    hr = commandList_->Reset(commandAllocator_.Get(), nullptr);
    assert(SUCCEEDED(hr));
}

void DirectXCommon::InitializeWithAdapter(UINT startAdapterIndex) {
    // 既存のリソースがあれば一旦すべてリセット（フォールバック再初期化対応）
    for (UINT i = 0; i < kBackBufferCount_; ++i) {
        backBuffers_[i].Reset();
    }
    depthStencilResource_.Reset();
    dsvDescriptorHeap_.Reset();
    rtvDescriptorHeap_.Reset();
    swapChain_.Reset();
    commandList_.Reset();
    commandAllocator_.Reset();
    commandQueue_.Reset();
    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
    fence_.Reset();
    device_.Reset();
    useAdapter_.Reset();

    CreateDevice(startAdapterIndex);

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue = nullptr;
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
#ifdef _DEBUG
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false);
#endif
        D3D12_MESSAGE_ID denyIds[] = {
            D3D12_MESSAGE_ID_RESOURCE_BARRIER_MISMATCHING_COMMAND_LIST_TYPE
        };
        D3D12_MESSAGE_SEVERITY severities[] = { D3D12_MESSAGE_SEVERITY_INFO };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs = _countof(denyIds);
        filter.DenyList.pIDList = denyIds;
        filter.DenyList.NumSeverities = _countof(severities);
        filter.DenyList.pSeverityList = severities;
        infoQueue->PushStorageFilter(&filter);
    }

    CreateCommandQueue();
    CreateSwapChain(winApp_);
    CreateRenderTarget();
    CreateDepthBuffer(winApp_);
    CreateFence();
}

bool DirectXCommon::FallbackAdapterIfDeviceRemoved() {
    if (!device_ || SUCCEEDED(device_->GetDeviceRemovedReason())) {
        return false;
    }
    char buf[256];
    sprintf_s(buf, "[DirectXCommon] Device was removed (reason=0x%08X) on adapter index %u! Reinitializing with fallback adapter...\n",
        (UINT)device_->GetDeviceRemovedReason(), currentAdapterIndex_);
    OutputDebugStringA(buf);

    InitializeWithAdapter(currentAdapterIndex_ + 1);
    return true;
}

void DirectXCommon::CreateDevice(UINT startAdapterIndex) {
    HRESULT hr = S_OK;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0 };
    const char* featureLevelNames[] = { "12.1", "12.0" };

    for (UINT i = startAdapterIndex; dxgiFactory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&useAdapter_)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC3 adapterDesc{};
        hr = useAdapter_->GetDesc3(&adapterDesc);
        assert(SUCCEEDED(hr));
        if (!(adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            std::string adapterName = ConvertString(adapterDesc.Description);
            OutputDebugStringA(("[DirectXCommon] Trying Adapter [" + std::to_string(i) + "]: " + adapterName + "\n").c_str());
            for (size_t levelIndex = 0; levelIndex < _countof(featureLevels); ++levelIndex) {
                hr = D3D12CreateDevice(useAdapter_.Get(), featureLevels[levelIndex], IID_PPV_ARGS(&device_));
                if (SUCCEEDED(hr) && device_ != nullptr) {
                    D3D12_DESCRIPTOR_HEAP_DESC testHeapDesc{};
                    testHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                    testHeapDesc.NumDescriptors = 1;
                    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> testHeap;
                    HRESULT testHr = device_->CreateDescriptorHeap(&testHeapDesc, IID_PPV_ARGS(&testHeap));
                    if (SUCCEEDED(testHr) && SUCCEEDED(device_->GetDeviceRemovedReason())) {
                        currentAdapterIndex_ = i;
                        OutputDebugStringA(("[DirectXCommon] Selected Adapter [" + std::to_string(i) + "]: " + adapterName + " (FeatureLevel " + featureLevelNames[levelIndex] + ")\n").c_str());
                        break;
                    }
                    OutputDebugStringA(("[DirectXCommon] Adapter failed verification, falling back: " + adapterName + "\n").c_str());
                    device_.Reset();
                }
            }
            if (device_ != nullptr) {
                break;
            }
        }
        useAdapter_.Reset();
    }

    // ハードウェアアダプタで作成できなかった場合はWARPアダプタにフォールバック
    if (device_ == nullptr) {
        hr = dxgiFactory_->EnumWarpAdapter(IID_PPV_ARGS(&useAdapter_));
        if (SUCCEEDED(hr)) {
            for (size_t levelIndex = 0; levelIndex < _countof(featureLevels); ++levelIndex) {
                hr = D3D12CreateDevice(useAdapter_.Get(), featureLevels[levelIndex], IID_PPV_ARGS(&device_));
                if (SUCCEEDED(hr)) {
                    currentAdapterIndex_ = 99;
                    OutputDebugStringA("[DirectXCommon] Fallback to WARP Adapter\n");
                    break;
                }
            }
        }
    }

    assert(device_ != nullptr);
}

void DirectXCommon::CreateCommandQueue() {
    D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
    HRESULT hr = device_->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&commandQueue_));
    assert(SUCCEEDED(hr));

    hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator_));
    assert(SUCCEEDED(hr));

    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator_.Get(), nullptr, IID_PPV_ARGS(&commandList_));
    assert(SUCCEEDED(hr));
}

void DirectXCommon::CreateSwapChain(WinApp* winApp) {
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width = winApp->kClientWidth;
    swapChainDesc.Height = winApp->kClientHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = kBackBufferCount_;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    HRESULT hr = dxgiFactory_->CreateSwapChainForHwnd(commandQueue_.Get(), winApp->GetHwnd(), &swapChainDesc, nullptr, nullptr, reinterpret_cast<IDXGISwapChain1**>(swapChain_.GetAddressOf()));
    assert(SUCCEEDED(hr));
}

void DirectXCommon::CreateRenderTarget() {
    rtvDescriptorHeap_ = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvDescriptorCount, false);

    rtvDesc_.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rtvDesc_.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvStartHandle = rtvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart();
    UINT descriptorSize = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < kBackBufferCount_; ++i) {
        HRESULT hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]));
        assert(SUCCEEDED(hr));

        rtvHandles_[i].ptr = rtvStartHandle.ptr + (static_cast<UINT64>(descriptorSize) * i);
        device_->CreateRenderTargetView(backBuffers_[i].Get(), &rtvDesc_, rtvHandles_[i]);
    }

    // RenderTexture用のハンドルもあらかじめ計算しておく（リソース自体は後で作成）
    rtvHandles_[2].ptr = rtvStartHandle.ptr + (static_cast<UINT64>(descriptorSize) * 2);
}

void DirectXCommon::CreateDepthBuffer(WinApp* winApp) {
    depthStencilResource_ = CreateDepthStencilTextureResource(device_.Get(), winApp->kClientWidth, winApp->kClientHeight);

    dsvDescriptorHeap_ = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    device_->CreateDepthStencilView(depthStencilResource_.Get(), &dsvDesc, dsvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart());
}

void DirectXCommon::CreateFence() {
    HRESULT hr = device_->CreateFence(fenceValue_, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    assert(SUCCEEDED(hr));

    fenceEvent_ = CreateEvent(NULL, FALSE, FALSE, NULL);
    assert(fenceEvent_ != nullptr);
}

// DirectXCommon.cpp の末尾などに追加してください

Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DirectXCommon::CreateDescriptorHeap(
    D3D12_DESCRIPTOR_HEAP_TYPE type, UINT numDescriptors, bool shaderVisible) {

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
    descriptorHeapDesc.Type = type;
    descriptorHeapDesc.NumDescriptors = numDescriptors;
    descriptorHeapDesc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = device_->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&descriptorHeap));
    assert(SUCCEEDED(hr));

    return descriptorHeap;
}

void DirectXCommon::TransitionDepthStencilState(D3D12_RESOURCE_STATES newState) {
    // 現在の状態と同じならバリア不要（不整合を防止）
    if (depthStencilState_ == newState) {
        return;
    }

    D3D12_RESOURCE_BARRIER depthBarrier{};
    depthBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    depthBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    depthBarrier.Transition.pResource = depthStencilResource_.Get();
    depthBarrier.Transition.StateBefore = depthStencilState_;
    depthBarrier.Transition.StateAfter = newState;
    depthBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList_->ResourceBarrier(1, &depthBarrier);

    depthStencilState_ = newState;
}