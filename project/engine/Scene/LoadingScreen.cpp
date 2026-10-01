#include "LoadingScreen.h"
#include "DirectXCommon.h"
#include "WinApp.h"
#include "TextureManager.h"
#include "GraphicsPipeline.h"
#include <algorithm>
#include <cmath>

LoadingScreen* LoadingScreen::GetInstance() {
    static LoadingScreen instance;
    return &instance;
}

void LoadingScreen::Initialize() {
    if (isInitialized_) return;

    TextureManager* texManager = TextureManager::GetInstance();
    texManager->LoadTexture("Title/white1x1.png");

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

    isInitialized_ = true;
}

void LoadingScreen::Reset() {
    time_ = 0.0f;
    currentProgress_ = 0.0f;
    targetProgress_ = 0.0f;
    fadeAlpha_ = 1.0f;
    isFadingOut_ = false;
    isTransitionCompleted_ = false;
}

void LoadingScreen::Update(float dt, float progress) {
    if (!isInitialized_) Initialize();

    time_ += (std::max)(0.0f, dt);
    targetProgress_ = (std::clamp)(progress, 0.0f, 1.0f);

    // プログレスの滑らかな補間 (イージング)
    currentProgress_ = std::lerp(currentProgress_, targetProgress_, 0.15f);
    if (std::abs(targetProgress_ - currentProgress_) < 0.001f) {
        currentProgress_ = targetProgress_;
    }

    // 100%読み込み完了後の画面フェードアウト遷移演出
    if (targetProgress_ >= 0.99f && currentProgress_ >= 0.98f) {
        isFadingOut_ = true;
    }

    if (isFadingOut_) {
        fadeAlpha_ -= dt * 2.5f; // 約0.4秒間で滑らかにフェードアウト
        if (fadeAlpha_ <= 0.0f) {
            fadeAlpha_ = 0.0f;
            isTransitionCompleted_ = true;
        }
    }

    // 右下の文字＆ゲージの長さ更新 (左から右へ光りながら溜まる)
    if (textFill_) {
        textFill_->transform.scale.x = 200.0f * currentProgress_;
        textFill_->SetColor({ 0.40f, 0.90f, 1.0f, 1.0f * fadeAlpha_ });
    }
    if (textBg_) {
        textBg_->SetColor({ 0.12f, 0.18f, 0.25f, 0.6f * fadeAlpha_ });
    }
    if (background_) {
        background_->SetColor({ 0.015f, 0.04f, 0.10f, 1.0f * fadeAlpha_ });
    }

    // 右下の回転ドットのアニメーション（残像付き周回）
    float centerX = 1205.0f;
    float centerY = 665.0f;
    float radius = 14.0f;

    for (size_t i = 0; i < dots_.size(); ++i) {
        float age = (float)i;
        float angle = time_ * 6.5f - age * 0.42f;
        float dotScale = 8.0f * (1.0f - age * 0.10f);
        float alpha = std::pow(0.62f, age) * fadeAlpha_;

        dots_[i]->transform.translate.x = centerX + radius * std::cos(angle);
        dots_[i]->transform.translate.y = centerY + radius * std::sin(angle);
        dots_[i]->transform.scale = { dotScale, dotScale, 1.0f };
        dots_[i]->SetColor({ 0.4f, 0.85f, 1.0f, alpha });
    }
}

void LoadingScreen::Draw() {
    if (!isInitialized_ || fadeAlpha_ <= 0.0f) return;

    DirectXCommon* dxCommon = DirectXCommon::GetInstance();
    ID3D12GraphicsCommandList* commandList = dxCommon->GetCommandList();

    // スプライト用パイプラインとルートシグネチャのセット
    GraphicsPipeline* pipeline = GraphicsPipeline::GetInstance();
    if (pipeline && pipeline->GetSpritePipelineState() && pipeline->GetSpriteRootSignature()) {
        commandList->SetPipelineState(pipeline->GetSpritePipelineState());
        commandList->SetGraphicsRootSignature(pipeline->GetSpriteRootSignature());
    }

    // デスクリプタヒープのセット
    ID3D12DescriptorHeap* heaps[] = { TextureManager::GetInstance()->GetSrvHeap() };
    commandList->SetDescriptorHeaps(1, heaps);

    // 正射影行列（2D画面用 1280x720）
    Matrix4x4 orthoProjection = MakeOrthographicMatrix(
        0.0f, 0.0f,
        (float)WinApp::kClientWidth, (float)WinApp::kClientHeight,
        0.0f, 1.0f
    );

    // 背景描画（半透明で、奥のプレイヤー飛行3Dシーンが透けて見える）
    if (background_) background_->Draw(commandList, orthoProjection);

    // Loading テキスト背景＆文字ゲージ描画（右下の小さいゲージで左から文字が光って溜まる演出）
    if (textBg_) textBg_->Draw(commandList, orthoProjection);
    if (textFill_ && currentProgress_ > 0.001f) textFill_->Draw(commandList, orthoProjection);

    // 右下の回転ドットスピナー描画
    for (int i = (int)dots_.size() - 1; i >= 0; --i) {
        if (dots_[i]) dots_[i]->Draw(commandList, orthoProjection);
    }
}
