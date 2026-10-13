#include "LoadingScreen.h"
#include "DirectXCommon.h"
#include "WinApp.h"
#include "TextureManager.h"
#include "SrvManager.h"
#include "GraphicsPipeline.h"
#include "D3D12Util.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
    constexpr float kSlashTitleDuration   = 0.44f; // タイトル画面を左斜め上→右斜め下へ斬る時間
    constexpr float kSplitTitleDuration   = 0.68f; // 切れたタイトル画面が上下へ移動する時間
    constexpr float kSlashLoadingDuration = 0.44f; // ロード画面を右斜め下→左斜め上へ斬る時間
    constexpr float kSplitLoadingDuration = 0.70f; // 切れたロード画面が左右へ移動する時間
}

LoadingScreen* LoadingScreen::GetInstance() {
    static LoadingScreen instance;
    return &instance;
}

void LoadingScreen::Initialize() {
    if (isInitialized_) return;

    DirectXCommon* dxCommon = DirectXCommon::GetInstance();
    ID3D12Device* device = dxCommon->GetDevice();

    TextureManager* texManager = TextureManager::GetInstance();
    texManager->LoadTexture("Title/white1x1.png");
    texManager->LoadTexture("Player2/Player_basecolor.JPEG");
    texManager->LoadTexture("test.dds");

    // 背景スプライト（完全不透明の紺色背景）
    background_ = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    background_->transform.scale = { (float)WinApp::kClientWidth, (float)WinApp::kClientHeight, 1.0f };
    background_->SetColor({ 0.015f, 0.04f, 0.10f, 1.0f });

    // Loading 右下テキスト＆ゲージ枠（背景）
    textBg_ = Sprite::Create("Title/white1x1.png", { 980.0f, 655.0f });
    textBg_->transform.scale = { 200.0f, 20.0f, 1.0f };
    textBg_->SetColor({ 0.12f, 0.18f, 0.25f, 0.6f });

    // Loading 右下テキスト＆ゲージ（前面・左から右へ光り輝きながら溜まる演出）
    textFill_ = Sprite::Create("Title/white1x1.png", { 980.0f, 655.0f });
    textFill_->transform.scale = { 0.0f, 20.0f, 1.0f };
    textFill_->SetColor({ 0.40f, 0.90f, 1.0f, 1.0f });

    // 回転ドット粒子 (6個の残像付きローディングスピナー)
    for (size_t i = 0; i < dots_.size(); ++i) {
        dots_[i] = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
        dots_[i]->transform.scale = { 8.0f, 8.0f, 1.0f };
        dots_[i]->SetColor({ 0.4f, 0.85f, 1.0f, 1.0f });
    }

    // 画面キャプチャ用テクスチャリソース (1280x720) の生成
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = WinApp::kClientWidth;
    texDesc.Height = WinApp::kClientHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr,
        IID_PPV_ARGS(&capturedTextureResource_)
    );

    capturedSrvIndex_ = SrvManager::GetInstance()->Allocate();
    SrvManager::GetInstance()->CreateSRVforTexture2D(
        capturedSrvIndex_,
        capturedTextureResource_.Get(),
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        1
    );
    capturedSrvHandleGPU_ = SrvManager::GetInstance()->GetGPUDescriptorHandle(capturedSrvIndex_);

    // キャプチャ画面描画用スプライト（全体・右上三角・左下三角）
    capturedFullSprite_ = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    capturedFullSprite_->transform.scale = { (float)WinApp::kClientWidth, (float)WinApp::kClientHeight, 1.0f };
    capturedFullSprite_->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });

    capturedUpperRightSprite_ = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    capturedUpperRightSprite_->transform.scale = { (float)WinApp::kClientWidth, (float)WinApp::kClientHeight, 1.0f };
    capturedUpperRightSprite_->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });

    capturedLowerLeftSprite_ = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    capturedLowerLeftSprite_->transform.scale = { (float)WinApp::kClientWidth, (float)WinApp::kClientHeight, 1.0f };
    capturedLowerLeftSprite_->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });

    // 斬撃ライン・切断面エッジ用スプライト
    slashGlowOuterSprite_ = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    slashGlowMidSprite_   = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    slashCoreSprite_      = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    slashEdgeSpriteA_     = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    slashEdgeSpriteB_     = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    slashEdgeCoreA_       = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    slashEdgeCoreB_       = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    slashFlashSprite_     = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    slashFlashSprite_->transform.scale = { (float)WinApp::kClientWidth, (float)WinApp::kClientHeight, 1.0f };

    // 画面を横断して斬るプレイヤー機モデル (Player2/Player.obj)
    slashFighterModel_ = std::unique_ptr<Model>(Model::LoadGLTF("Resources/Player2/Player.obj", device));

    slashFighterTransformRes_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    slashFighterTransformRes_->Map(0, nullptr, reinterpret_cast<void**>(&slashFighterTransformData_));
    slashFighterTransformData_->WVP = MakeIdentity4x4();
    slashFighterTransformData_->World = MakeIdentity4x4();
    slashFighterTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    slashFighterLightRes_ = CreateBufferResource(device, sizeof(DirectionalLight));
    slashFighterLightRes_->Map(0, nullptr, reinterpret_cast<void**>(&slashFighterLightData_));
    slashFighterLightData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    slashFighterLightData_->direction = Normalize({ -0.3f, -0.5f, 0.8f });
    slashFighterLightData_->intensity = 1.35f;

    slashFighterCameraRes_ = CreateBufferResource(device, sizeof(SlashCameraCB));
    slashFighterCameraRes_->Map(0, nullptr, reinterpret_cast<void**>(&slashFighterCameraData_));
    slashFighterCameraData_->worldPosition = { 0.0f, 0.0f, -100.0f };
    slashFighterCameraData_->padding = 0.0f;

    isInitialized_ = true;
}

void LoadingScreen::Reset() {
    phase_ = TransitionPhase::kSlashTitle;
    phaseTimer_ = 0.0f;
    time_ = 0.0f;
    currentProgress_ = 0.0f;
    targetProgress_ = 0.0f;
    hasCapturedTitle_ = false;
    hasCapturedLoading_ = false;
    isTransitionCompleted_ = false;
}

void LoadingScreen::Update(float dt, float progress) {
    if (!isInitialized_) Initialize();

    float safeDt = (std::max)(0.0f, dt);
    time_ += safeDt;
    phaseTimer_ += safeDt;
    targetProgress_ = (std::clamp)(progress, 0.0f, 1.0f);

    switch (phase_) {
    case TransitionPhase::kSlashTitle:
        // 1. 左斜め上から右斜め下へプレイヤー機が横断して画面を切る
        if (phaseTimer_ >= kSlashTitleDuration) {
            phase_ = TransitionPhase::kSplitTitleUpDown;
            phaseTimer_ = 0.0f;
        }
        break;

    case TransitionPhase::kSplitTitleUpDown:
        // 2. 切れたタイトル画面が上下へ移動し、隙間からロード画面が出る
        currentProgress_ = std::lerp(currentProgress_, 0.25f, 0.12f);
        if (phaseTimer_ >= kSplitTitleDuration) {
            phase_ = TransitionPhase::kLoadingProgress;
            phaseTimer_ = 0.0f;
        }
        break;

    case TransitionPhase::kLoadingProgress:
        // 3. ロード画面のゲージ進行
        currentProgress_ = std::lerp(currentProgress_, targetProgress_, 0.18f);
        if (std::abs(targetProgress_ - currentProgress_) < 0.005f) {
            currentProgress_ = targetProgress_;
        }
        // ロード完了かつ最低表示時間(0.35秒)経過＆ロード画面キャプチャ済みなら逆方向斬撃へ遷移
        if (targetProgress_ >= 0.99f && currentProgress_ >= 0.99f && phaseTimer_ >= 0.35f && hasCapturedLoading_) {
            phase_ = TransitionPhase::kSlashLoadingReverse;
            phaseTimer_ = 0.0f;
        }
        break;

    case TransitionPhase::kSlashLoadingReverse:
        // 4. 右斜め下から左斜め上へプレイヤー機が逆横断してロード画面を切る
        currentProgress_ = 1.0f;
        if (phaseTimer_ >= kSlashLoadingDuration) {
            phase_ = TransitionPhase::kSplitLoadingLeftRight;
            phaseTimer_ = 0.0f;
        }
        break;

    case TransitionPhase::kSplitLoadingLeftRight:
        // 5. 切れたロード画面が左右へ移動し、完全に画面外へ出たら遷移完了
        currentProgress_ = 1.0f;
        if (phaseTimer_ >= kSplitLoadingDuration) {
            phase_ = TransitionPhase::kCompleted;
            phaseTimer_ = 0.0f;
            isTransitionCompleted_ = true;
        }
        break;

    case TransitionPhase::kCompleted:
        isTransitionCompleted_ = true;
        break;
    }

    // ローディングUI（右下ゲージ・回転ドット）の更新
    if (textFill_) {
        textFill_->transform.scale.x = 200.0f * currentProgress_;
        textFill_->SetColor({ 0.40f, 0.90f, 1.0f, 1.0f });
    }
    if (textBg_) {
        textBg_->SetColor({ 0.12f, 0.18f, 0.25f, 0.6f });
    }
    if (background_) {
        background_->SetColor({ 0.015f, 0.04f, 0.10f, 1.0f });
    }

    float centerX = 1205.0f;
    float centerY = 665.0f;
    float radius = 14.0f;

    for (size_t i = 0; i < dots_.size(); ++i) {
        float age = (float)i;
        float angle = time_ * 6.5f - age * 0.42f;
        float dotScale = 8.0f * (1.0f - age * 0.10f);
        float alpha = std::pow(0.62f, age);

        dots_[i]->transform.translate.x = centerX + radius * std::cos(angle);
        dots_[i]->transform.translate.y = centerY + radius * std::sin(angle);
        dots_[i]->transform.scale = { dotScale, dotScale, 1.0f };
        dots_[i]->SetColor({ 0.4f, 0.85f, 1.0f, alpha });
    }
}

void LoadingScreen::CaptureCurrentBackBuffer(ID3D12GraphicsCommandList* commandList) {
    DirectXCommon* dxCommon = DirectXCommon::GetInstance();
    ID3D12Resource* backBuffer = dxCommon->GetCurrentBackBufferResource();
    if (!backBuffer || !capturedTextureResource_ || !commandList) return;

    D3D12_RESOURCE_BARRIER barriersToCopy[2]{};
    barriersToCopy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriersToCopy[0].Transition.pResource = backBuffer;
    barriersToCopy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriersToCopy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriersToCopy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barriersToCopy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriersToCopy[1].Transition.pResource = capturedTextureResource_.Get();
    barriersToCopy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriersToCopy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barriersToCopy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    commandList->ResourceBarrier(2, barriersToCopy);
    commandList->CopyResource(capturedTextureResource_.Get(), backBuffer);

    D3D12_RESOURCE_BARRIER barriersFromCopy[2]{};
    barriersFromCopy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriersFromCopy[0].Transition.pResource = backBuffer;
    barriersFromCopy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriersFromCopy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriersFromCopy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barriersFromCopy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriersFromCopy[1].Transition.pResource = capturedTextureResource_.Get();
    barriersFromCopy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriersFromCopy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriersFromCopy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    commandList->ResourceBarrier(2, barriersFromCopy);
}

void LoadingScreen::DrawLoadingBase(ID3D12GraphicsCommandList* commandList, const Matrix4x4& orthoProjection) {
    if (background_) background_->Draw(commandList, orthoProjection);
    if (textBg_) textBg_->Draw(commandList, orthoProjection);
    if (textFill_ && currentProgress_ > 0.001f) textFill_->Draw(commandList, orthoProjection);

    for (int i = (int)dots_.size() - 1; i >= 0; --i) {
        if (dots_[i]) dots_[i]->Draw(commandList, orthoProjection);
    }
}

void LoadingScreen::DrawLineSprite(
    Sprite* sprite,
    ID3D12GraphicsCommandList* commandList,
    const Matrix4x4& orthoProjection,
    float x0, float y0, float x1, float y1,
    float thickness, const Vector4& color) {
    if (!sprite) return;
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = std::hypot(dx, dy);
    if (len < 0.5f) return;

    // MakeRotateZMatrix の行ベクトル規約に合わせて中心線が (x0,y0) -> (x1,y1) に一致するよう配置
    float angle = -std::atan2(dy, dx);
    float nx = dy / len;
    float ny = -dx / len;

    sprite->transform.scale = { len, thickness, 1.0f };
    sprite->transform.rotate = { 0.0f, 0.0f, angle };
    sprite->transform.translate = {
        x0 + 0.5f * thickness * nx,
        y0 + 0.5f * thickness * ny,
        0.0f
    };
    sprite->SetColor(color);
    sprite->Draw(commandList, orthoProjection);
}

void LoadingScreen::DrawSlashFighter3D(
    ID3D12GraphicsCommandList* commandList,
    float screenX, float screenY,
    bool isTopLeftToBottomRight,
    float progress01) {
    if (!slashFighterModel_ || !slashFighterTransformRes_) return;

    DirectXCommon* dxCommon = DirectXCommon::GetInstance();
    GraphicsPipeline* pipeline = GraphicsPipeline::GetInstance();
    if (!pipeline || !pipeline->GetObject3dPipelineState() || !pipeline->GetObject3dRootSignature()) return;

    // 3D機体が背景の深度バッファと干渉しないよう深度をクリアして最前面に描画
    dxCommon->TransitionDepthStencilState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = dxCommon->GetCurrentBackBufferRtvHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dxCommon->GetDsvHandle();
    commandList->OMSetRenderTargets(1, &backBufferRtv, false, &dsvHandle);
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    const float fovY = 0.45f;
    const float aspect = (float)WinApp::kClientWidth / (float)WinApp::kClientHeight;
    const float camZ = -100.0f;
    const float dist = 100.0f;

    float halfH = dist * std::tan(fovY * 0.5f);
    float halfW = halfH * aspect;

    float ndcX = (screenX / ((float)WinApp::kClientWidth * 0.5f)) - 1.0f;
    float ndcY = 1.0f - (screenY / ((float)WinApp::kClientHeight * 0.5f));
    Vector3 worldPos = { ndcX * halfW, ndcY * halfH, 0.0f };

    // 対角線の3D進行方向ベクトル
    Vector3 flightDir = isTopLeftToBottomRight
        ? Normalize({ +halfW, -halfH, 0.0f })
        : Normalize({ -halfW, +halfH, 0.0f });

    // カメラ側(-Z)へ機体背面(+Y)を向けつつ進行方向へ機首を向ける直交基底
    Vector3 basisUp = { 0.0f, 0.0f, -1.0f };
    Vector3 basisRight = Normalize(Cross(basisUp, flightDir));
    basisUp = Normalize(Cross(flightDir, basisRight));

    Matrix4x4 R_basis = MakeIdentity4x4();
    R_basis.m[0][0] = basisRight.x; R_basis.m[0][1] = basisRight.y; R_basis.m[0][2] = basisRight.z;
    R_basis.m[1][0] = basisUp.x;    R_basis.m[1][1] = basisUp.y;    R_basis.m[1][2] = basisUp.z;
    R_basis.m[2][0] = flightDir.x;  R_basis.m[2][1] = flightDir.y;  R_basis.m[2][2] = flightDir.z;

    // Player.obj は初期状態で -Z 向きのため Y軸180度回転で +Z を機首にし、ダイナミックなロール角を付与
    float rollAngle = (isTopLeftToBottomRight ? 0.55f : -0.55f) + progress01 * 1.15f;
    Matrix4x4 rotY180 = MakeRotateYMatrix(std::numbers::pi_v<float>);
    Matrix4x4 rotPitch = MakeRotateXMatrix(-0.15f);
    Matrix4x4 rotRoll  = MakeRotateZMatrix(rollAngle);
    Matrix4x4 R_local  = Multiply(Multiply(rotRoll, rotPitch), rotY180);
    Matrix4x4 R_final  = Multiply(R_local, R_basis);

    Matrix4x4 scaleMat = Matrix4x4MakeScaleMatrix({ 14.5f, 14.5f, 14.5f });
    Matrix4x4 transMat = MakeTranslateMatrix(worldPos);
    Matrix4x4 worldMat = Multiply(Multiply(scaleMat, R_final), transMat);

    Matrix4x4 viewMat = Inverse(MakeTranslateMatrix({ 0.0f, 0.0f, camZ }));
    Matrix4x4 projMat = MakePerspectiveFovMatrix(fovY, aspect, 0.1f, 500.0f);
    Matrix4x4 viewProj = Multiply(viewMat, projMat);

    slashFighterTransformData_->World = worldMat;
    slashFighterTransformData_->WVP   = Multiply(worldMat, viewProj);
    slashFighterTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    ID3D12DescriptorHeap* heaps[] = { TextureManager::GetInstance()->GetSrvHeap() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetPipelineState(pipeline->GetObject3dPipelineState());
    commandList->SetGraphicsRootSignature(pipeline->GetObject3dRootSignature());

    commandList->SetGraphicsRootConstantBufferView(1, slashFighterTransformRes_->GetGPUVirtualAddress());
    if (slashFighterLightRes_)  commandList->SetGraphicsRootConstantBufferView(4, slashFighterLightRes_->GetGPUVirtualAddress());
    if (slashFighterCameraRes_) commandList->SetGraphicsRootConstantBufferView(5, slashFighterCameraRes_->GetGPUVirtualAddress());

    slashFighterModel_->DrawModel(
        commandList,
        TextureManager::GetInstance()->GetSrvHandleGPU("Player2/Player_basecolor.JPEG"),
        TextureManager::GetInstance()->GetSrvHandleGPU("test.dds")
    );
}

void LoadingScreen::Draw() {
    if (!isInitialized_ || isTransitionCompleted_) return;

    DirectXCommon* dxCommon = DirectXCommon::GetInstance();
    ID3D12GraphicsCommandList* commandList = dxCommon->GetCommandList();
    GraphicsPipeline* pipeline = GraphicsPipeline::GetInstance();
    if (!pipeline || !pipeline->GetSpritePipelineState() || !pipeline->GetSpriteRootSignature()) return;

    const float W = (float)WinApp::kClientWidth;
    const float H = (float)WinApp::kClientHeight;

    Matrix4x4 orthoProjection = MakeOrthographicMatrix(0.0f, 0.0f, W, H, 0.0f, 1.0f);

    auto bindSpritePipeline = [&]() {
        ID3D12DescriptorHeap* heaps[] = { TextureManager::GetInstance()->GetSrvHeap() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetPipelineState(pipeline->GetSpritePipelineState());
        commandList->SetGraphicsRootSignature(pipeline->GetSpriteRootSignature());
    };

    bindSpritePipeline();

    if (phase_ == TransitionPhase::kSlashTitle) {
        // 初回フレームで現在のタイトル画面をテクスチャへキャプチャ
        if (!hasCapturedTitle_) {
            CaptureCurrentBackBuffer(commandList);
            hasCapturedTitle_ = true;
        } else if (capturedFullSprite_) {
            // キャプチャ済みの静止タイトル画面を描画
            capturedFullSprite_->transform.translate = { 0.0f, 0.0f, 0.0f };
            capturedFullSprite_->transform.scale = { W, H, 1.0f };
            capturedFullSprite_->DrawCustomSrv(commandList, orthoProjection, capturedSrvHandleGPU_, 6, 0);
        }

        // 左斜め上 (-160, -90) -> 右斜め下 (1440, 810) へプレイヤー機が高速横断
        float t = std::clamp(phaseTimer_ / kSlashTitleDuration, 0.0f, 1.0f);
        float easedT = 0.25f * t + 0.75f * t * t;
        float fx = std::lerp(-160.0f, W + 160.0f, easedT);
        float fy = std::lerp(-90.0f,  H + 90.0f,  easedT);

        // プレイヤー機の軌跡に沿って伸びる斬撃ライン
        float lineEndX = std::clamp(fx, 0.0f, W);
        float lineEndY = std::clamp(fy, 0.0f, H);
        if (fx > 0.0f) {
            DrawLineSprite(slashGlowOuterSprite_.get(), commandList, orthoProjection, 0.0f, 0.0f, lineEndX, lineEndY, 26.0f, { 0.10f, 0.75f, 1.0f, 0.45f });
            DrawLineSprite(slashGlowMidSprite_.get(),   commandList, orthoProjection, 0.0f, 0.0f, lineEndX, lineEndY, 11.0f, { 0.45f, 0.95f, 1.0f, 0.85f });
            DrawLineSprite(slashCoreSprite_.get(),      commandList, orthoProjection, 0.0f, 0.0f, lineEndX, lineEndY, 4.0f,  { 1.0f,  1.0f,  1.0f, 1.0f  });
        }

        // 斬り終わり瞬間のホワイトフラッシュ
        if (t > 0.82f && slashFlashSprite_) {
            float flashAlpha = std::sin(((t - 0.82f) / 0.18f) * std::numbers::pi_v<float>) * 0.45f;
            slashFlashSprite_->SetColor({ 0.85f, 0.98f, 1.0f, flashAlpha });
            slashFlashSprite_->Draw(commandList, orthoProjection);
        }

        // 3Dプレイヤー機を斬撃先端に描画
        DrawSlashFighter3D(commandList, fx, fy, true, t);
    }
    else if (phase_ == TransitionPhase::kSplitTitleUpDown) {
        // 1. 奥（隙間）に見えるロード画面を全面に描画
        DrawLoadingBase(commandList, orthoProjection);

        // 2. 切れたタイトル画面の2つの三角形を上下（右上半分は上へ、左下半分は下へ）にスライド描画
        float t = std::clamp(phaseTimer_ / kSplitTitleDuration, 0.0f, 1.0f);
        // 最初少し溜めてから滑らかに上下へ開くイージング (SmoothStep + 加速)
        float eased = t * t * (2.2f - 1.2f * t);
        float splitOffsetY = eased * (H + 40.0f); // 760px移動で完全に画面外へ

        if (capturedUpperRightSprite_) {
            capturedUpperRightSprite_->transform.translate = { 0.0f, -splitOffsetY, 0.0f };
            capturedUpperRightSprite_->transform.scale = { W, H, 1.0f };
            capturedUpperRightSprite_->DrawCustomSrv(commandList, orthoProjection, capturedSrvHandleGPU_, 3, 3);
        }
        if (capturedLowerLeftSprite_) {
            capturedLowerLeftSprite_->transform.translate = { 0.0f, +splitOffsetY, 0.0f };
            capturedLowerLeftSprite_->transform.scale = { W, H, 1.0f };
            capturedLowerLeftSprite_->DrawCustomSrv(commandList, orthoProjection, capturedSrvHandleGPU_, 3, 0);
        }

        // 切断面の光るエッジライン（上下それぞれの境界線）
        float edgeAlpha = std::clamp(1.0f - t * 0.7f, 0.0f, 1.0f);
        DrawLineSprite(slashEdgeSpriteA_.get(), commandList, orthoProjection, 0.0f, -splitOffsetY, W, H - splitOffsetY, 10.0f, { 0.2f, 0.85f, 1.0f, 0.75f * edgeAlpha });
        DrawLineSprite(slashEdgeCoreA_.get(),   commandList, orthoProjection, 0.0f, -splitOffsetY, W, H - splitOffsetY, 3.5f,  { 1.0f, 1.0f,  1.0f, 1.0f  * edgeAlpha });
        DrawLineSprite(slashEdgeSpriteB_.get(), commandList, orthoProjection, 0.0f, +splitOffsetY, W, H + splitOffsetY, 10.0f, { 0.2f, 0.85f, 1.0f, 0.75f * edgeAlpha });
        DrawLineSprite(slashEdgeCoreB_.get(),   commandList, orthoProjection, 0.0f, +splitOffsetY, W, H + splitOffsetY, 3.5f,  { 1.0f, 1.0f,  1.0f, 1.0f  * edgeAlpha });
    }
    else if (phase_ == TransitionPhase::kLoadingProgress) {
        // ロード画面本体の描画
        DrawLoadingBase(commandList, orthoProjection);

        // ロードが100%に達したら、次の逆斬撃＆左右分割のためにロード画面をキャプチャ
        if (targetProgress_ >= 0.99f && currentProgress_ >= 0.99f && phaseTimer_ >= 0.30f && !hasCapturedLoading_) {
            CaptureCurrentBackBuffer(commandList);
            hasCapturedLoading_ = true;
        }
    }
    else if (phase_ == TransitionPhase::kSlashLoadingReverse) {
        // ロード画面を描画した上で、逆方向（右斜め下 -> 左斜め上）へプレイヤー機が横断して画面を切る
        DrawLoadingBase(commandList, orthoProjection);
        if (!hasCapturedLoading_) {
            CaptureCurrentBackBuffer(commandList);
            hasCapturedLoading_ = true;
        }

        float t = std::clamp(phaseTimer_ / kSlashLoadingDuration, 0.0f, 1.0f);
        float easedT = 0.25f * t + 0.75f * t * t;
        float fx = std::lerp(W + 160.0f, -160.0f, easedT);
        float fy = std::lerp(H + 90.0f,  -90.0f,  easedT);

        // 右下 (W, H) からプレイヤー機の現在位置まで伸びる逆斬撃ライン
        float lineEndX = std::clamp(fx, 0.0f, W);
        float lineEndY = std::clamp(fy, 0.0f, H);
        if (fx < W) {
            DrawLineSprite(slashGlowOuterSprite_.get(), commandList, orthoProjection, W, H, lineEndX, lineEndY, 26.0f, { 0.10f, 0.75f, 1.0f, 0.45f });
            DrawLineSprite(slashGlowMidSprite_.get(),   commandList, orthoProjection, W, H, lineEndX, lineEndY, 11.0f, { 0.45f, 0.95f, 1.0f, 0.85f });
            DrawLineSprite(slashCoreSprite_.get(),      commandList, orthoProjection, W, H, lineEndX, lineEndY, 4.0f,  { 1.0f,  1.0f,  1.0f, 1.0f  });
        }

        // 斬り終わり瞬間のホワイトフラッシュ
        if (t > 0.82f && slashFlashSprite_) {
            float flashAlpha = std::sin(((t - 0.82f) / 0.18f) * std::numbers::pi_v<float>) * 0.45f;
            slashFlashSprite_->SetColor({ 0.85f, 0.98f, 1.0f, flashAlpha });
            slashFlashSprite_->Draw(commandList, orthoProjection);
        }

        // 3Dプレイヤー機を逆方向（右斜め下 -> 左斜め上）の斬撃先端に描画
        DrawSlashFighter3D(commandList, fx, fy, false, t);
    }
    else if (phase_ == TransitionPhase::kSplitLoadingLeftRight) {
        // ※下層には SceneManager::Draw() で既に nextScene_ (GamePlayScene) が描画されている
        // その上に、切れたロード画面の2つの三角形を左右（左下半分は左へ、右上半分は右へ）にスライド描画
        float t = std::clamp(phaseTimer_ / kSplitLoadingDuration, 0.0f, 1.0f);
        float eased = t * t * (2.2f - 1.2f * t);
        float splitOffsetX = eased * (W + 40.0f); // 1320px移動で完全に画面外へ

        if (capturedLowerLeftSprite_) {
            capturedLowerLeftSprite_->transform.translate = { -splitOffsetX, 0.0f, 0.0f };
            capturedLowerLeftSprite_->transform.scale = { W, H, 1.0f };
            capturedLowerLeftSprite_->DrawCustomSrv(commandList, orthoProjection, capturedSrvHandleGPU_, 3, 0);
        }
        if (capturedUpperRightSprite_) {
            capturedUpperRightSprite_->transform.translate = { +splitOffsetX, 0.0f, 0.0f };
            capturedUpperRightSprite_->transform.scale = { W, H, 1.0f };
            capturedUpperRightSprite_->DrawCustomSrv(commandList, orthoProjection, capturedSrvHandleGPU_, 3, 3);
        }

        // 切断面の光るエッジライン（左右それぞれの境界線）
        float edgeAlpha = std::clamp(1.0f - t * 0.7f, 0.0f, 1.0f);
        DrawLineSprite(slashEdgeSpriteA_.get(), commandList, orthoProjection, -splitOffsetX, 0.0f, W - splitOffsetX, H, 10.0f, { 0.2f, 0.85f, 1.0f, 0.75f * edgeAlpha });
        DrawLineSprite(slashEdgeCoreA_.get(),   commandList, orthoProjection, -splitOffsetX, 0.0f, W - splitOffsetX, H, 3.5f,  { 1.0f, 1.0f,  1.0f, 1.0f  * edgeAlpha });
        DrawLineSprite(slashEdgeSpriteB_.get(), commandList, orthoProjection, +splitOffsetX, 0.0f, W + splitOffsetX, H, 10.0f, { 0.2f, 0.85f, 1.0f, 0.75f * edgeAlpha });
        DrawLineSprite(slashEdgeCoreB_.get(),   commandList, orthoProjection, +splitOffsetX, 0.0f, W + splitOffsetX, H, 3.5f,  { 1.0f, 1.0f,  1.0f, 1.0f  * edgeAlpha });
    }
}

