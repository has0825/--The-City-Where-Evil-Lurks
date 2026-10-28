#include "Game.h"
#include "SceneManager.h"
#include "SceneFactory.h"
#include "Input.h"
#include "TextureManager.h"
#include "SrvManager.h"
 
#ifdef USE_IMGUI
#include "externals/imgui/imgui.h"
#include "externals/imgui/imgui_impl_dx12.h"
#endif


void Game::Initialize() {
    Framework::Initialize();
    Input::GetInstance()->Initialize(winApp_);

    // TextureManagerの初期化 (全シーン・LoadingScreen共通)
    TextureManager::GetInstance()->Initialize(dxCommon_->GetDevice(), "Resources/");

    sceneFactory_ = std::make_unique<SceneFactory>();

    // get() で生ポインタを渡す（所有権は移さない）
    SceneManager::GetInstance()->SetFactory(sceneFactory_.get());
    SceneManager::GetInstance()->ChangeScene("TITLE");

    // 初回シーン初期化中にGPUアダプタがPnP停止（Device Removed）になった場合は別アダプタへフォールバックして再構築する
    if (dxCommon_->FallbackAdapterIfDeviceRemoved()) {
        SceneManager::GetInstance()->Reset();
        SrvManager::GetInstance()->Initialize(dxCommon_);
        if (graphicsPipeline_) {
            graphicsPipeline_->Reset();
            graphicsPipeline_->Initialize(dxCommon_->GetDevice());
        }
        srvDescriptorHeap_ = CreateDescriptorHeap(dxCommon_->GetDevice(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 128, true);
        descriptorSizeSRV_ = dxCommon_->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
#ifdef USE_IMGUI
        ImGui_ImplDX12_Shutdown();
        imguiDescriptorHeap_ = CreateDescriptorHeap(dxCommon_->GetDevice(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
        if (imguiDescriptorHeap_) {
            ImGui_ImplDX12_Init(dxCommon_->GetDevice(), dxCommon_->GetBackBufferCount(),
                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, imguiDescriptorHeap_.Get(),
                imguiDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(),
                imguiDescriptorHeap_->GetGPUDescriptorHandleForHeapStart());
        }
#endif
        TextureManager::GetInstance()->Initialize(dxCommon_->GetDevice(), "Resources/");
        SceneManager::GetInstance()->ChangeScene("TITLE");
    }
}

void Game::Finalize() {

    Framework::Finalize();
}

void Game::Update() {
    Framework::Update();

    // 入力情報の更新 (これで全シーンで入力が効くようになります)
    Input::GetInstance()->Update();

    SceneManager::GetInstance()->Update();
}

void Game::Draw() {
    dxCommon_->PreDraw();
    SceneManager::GetInstance()->Draw();

#ifdef USE_IMGUI
    // ImGui描画
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData) {
        ID3D12DescriptorHeap* descriptorHeaps[] = { imguiDescriptorHeap_.Get() };
        dxCommon_->GetCommandList()->SetDescriptorHeaps(1, descriptorHeaps);
        ImGui_ImplDX12_RenderDrawData(drawData, dxCommon_->GetCommandList());
    }
#endif


    dxCommon_->PostDraw();
}