#include "TitleScene.h"
#include "SceneManager.h"
#include "WinApp.h"
#include "TextureManager.h"
#include "MathUtil.h"
#include "DirectXCommon.h"
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

void TitleScene::Initialize() {
    // 前のシーンのリソースを即座に解放
    SceneManager::GetInstance()->ClearPreviousScene();

    inputDelay_ = 24; // 起動時等の誤トリガーを防ぐため、開始時はディレイを設ける
    isTransitioningToGame_ = false;
    transitionTimer_ = 0.0f;
    titleAnimTimer_ = 0.0f;

    input_ = Input::GetInstance();
    graphicsPipeline_ = GraphicsPipeline::GetInstance();
    DirectXCommon* dxCommon = DirectXCommon::GetInstance();
    ID3D12Device* device = dxCommon->GetDevice();

    // TextureManagerの初期化 (起動時に最初のシーンとなるため必須)
    TextureManager::GetInstance()->Initialize(device, "Resources/");

    // 背景用ゲームプレイシーンの初期化
    gameplayScene_ = std::make_unique<GamePlayScene>();
    gameplayScene_->SetTitleMode(true);
    gameplayScene_->Initialize();

    camera_ = std::make_unique<Camera>(WinApp::kClientWidth, WinApp::kClientHeight);
    camera_->SetTranslate({ 0.0f, 0.0f, -40.0f });
    camera_->Update();

    // テクスチャロード
    TextureManager::GetInstance()->LoadTexture("Title/white1x1.png");
    TextureManager::GetInstance()->LoadTexture("circle2.png");
    TextureManager::GetInstance()->LoadTexture("test.dds"); // 環境マップ用

    // ── 3Dタイトルモデル（4層マルチレイヤー構造）の読み込み ──
    // 背景の中央〜下部に映るプレイヤー自機を一切隠さないよう、画面上部（Y=6.4f）に配置
    // 1. メイン前面文字（深みのあるダーククリムゾン＋控えめな金属光沢）
    titleModel_ = Model::LoadGLTF("Resources/Title/Title.obj", device);
    if (titleModel_) {
        titleModel_->SetColor({ 0.62f, 0.04f, 0.04f, 1.0f });
        titleModel_->SetLightingEnabled(true);
        titleModel_->SetEnvironmentCoefficient(0.12f);
        titleModel_->transform.scale = { 5.2f, 5.2f, 4.4f };
        titleModel_->transform.rotate = { 0.0f, 0.0f, 0.0f };
        titleModel_->transform.translate = { 0.0f, 6.4f, 20.0f };
    }

    // 2. 重厚ブロンズゴールド・ベベルリム層（明るすぎない落ち着いた金属縁取り）
    titleRimModel_ = Model::LoadGLTF("Resources/Title/Title.obj", device);
    if (titleRimModel_) {
        titleRimModel_->SetColor({ 0.65f, 0.38f, 0.10f, 1.0f });
        titleRimModel_->SetLightingEnabled(true);
        titleRimModel_->SetEnvironmentCoefficient(0.24f);
        titleRimModel_->transform.scale = { 5.36f, 5.42f, 4.50f };
        titleRimModel_->transform.rotate = { 0.0f, 0.0f, 0.0f };
        titleRimModel_->transform.translate = { 0.0f, 6.4f, 20.24f };
    }

    // 3. 漆黒＆ダーククリムゾン立体シャドウ層（文字を引き締めてくっきり浮かび上がらせる太縁取り）
    titleShadowModel_ = Model::LoadGLTF("Resources/Title/Title.obj", device);
    if (titleShadowModel_) {
        titleShadowModel_->SetColor({ 0.015f, 0.002f, 0.005f, 1.0f });
        titleShadowModel_->SetLightingEnabled(false);
        titleShadowModel_->SetEnvironmentCoefficient(0.0f);
        titleShadowModel_->transform.scale = { 5.65f, 5.88f, 4.65f };
        titleShadowModel_->transform.rotate = { 0.0f, 0.0f, 0.0f };
        titleShadowModel_->transform.translate = { 0.14f, 6.22f, 20.58f };
    }

    // 4. 背面へ周期的に拡大・拡散する2重の3Dダークオーラ残像層
    for (int i = 0; i < 2; ++i) {
        titleAuraModels_[i] = Model::LoadGLTF("Resources/Title/Title.obj", device);
        if (titleAuraModels_[i]) {
            titleAuraModels_[i]->SetColor({ 0.50f, 0.04f, 0.04f, 0.25f });
            titleAuraModels_[i]->SetLightingEnabled(false);
            titleAuraModels_[i]->SetEnvironmentCoefficient(0.0f);
        }
    }

    // ── 定数バッファの作成 ──
    auto initTransformCB = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& res, TransformationMatrix*& ptr) {
        res = CreateBufferResource(device, sizeof(TransformationMatrix));
        res->Map(0, nullptr, reinterpret_cast<void**>(&ptr));
        ptr->WVP = MakeIdentity4x4();
        ptr->World = MakeIdentity4x4();
        ptr->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    };

    initTransformCB(transformResource_, transformData_);
    initTransformCB(rimTransformResource_, rimTransformData_);
    initTransformCB(shadowTransformResource_, shadowTransformData_);
    for (int i = 0; i < 2; ++i) {
        initTransformCB(auraTransformResources_[i], auraTransformData_[i]);
    }

    directionalLightResource_ = CreateBufferResource(device, sizeof(DirectionalLight));
    directionalLightResource_->Map(0, nullptr, reinterpret_cast<void**>(&directionalLightData_));
    directionalLightData_->color = { 0.92f, 0.85f, 0.82f, 1.0f };
    directionalLightData_->direction = Normalize({ -0.22f, -0.65f, 0.72f });
    directionalLightData_->intensity = 0.85f; // 白飛びせず深みのある赤になるようライト強度を抑える

    // 文字表面を左から右へしっとりと走査する控えめなスポットライト光沢
    directionalLightData_->enableSpotLight = 1;
    directionalLightData_->spotLightPos = { 0.0f, 6.4f, 8.0f };
    directionalLightData_->spotLightDir = { 0.0f, 0.0f, 1.0f };
    directionalLightData_->spotLightRange = 38.0f;
    directionalLightData_->spotLightCosAngle = std::cos(20.0f * static_cast<float>(M_PI) / 180.0f);
    directionalLightData_->spotLightColor = { 0.85f, 0.42f, 0.18f };
    directionalLightData_->spotLightIntensity = 1.65f;

    cameraResource_ = CreateBufferResource(device, sizeof(CameraDataCB));
    cameraResource_->Map(0, nullptr, reinterpret_cast<void**>(&cameraDataCB_));
    cameraDataCB_->worldPosition = camera_->GetTransform().translate;

    // ── 2DコンパクトHUDフレーム＆サイバー演出スプライト群の生成 ──
    for (int i = 0; i < 2; ++i) {
        letterboxSprites_[i]     = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
        letterboxEdgeSprites_[i] = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
        backdropGlowSprites_[i]  = Sprite::Create("circle2.png", { 0.0f, 0.0f });
    }
    visorBandSprite_      = Sprite::Create("circle2.png", { 0.0f, 0.0f });
    visorInnerBandSprite_ = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    for (int i = 0; i < 4; ++i) {
        visorRailSprites_[i] = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
        diamondSprites_[i]   = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    }
    for (int i = 0; i < 6; ++i) {
        streakSprites_[i] = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
    }
    for (int i = 0; i < 12; ++i) {
        emberSprites_[i] = Sprite::Create("circle2.png", { 0.0f, 0.0f });
    }
    impactFlashSprite_ = Sprite::Create("Title/white1x1.png", { 0.0f, 0.0f });
}

void TitleScene::Finalize() {
}

void TitleScene::Update() {
    const float kDeltaTime = 1.0f / 60.0f;
    titleAnimTimer_ += kDeltaTime;

    // 背景用ゲームプレイシーンの更新
    if (gameplayScene_) {
        gameplayScene_->Update();
    }

    camera_->Update();
    Matrix4x4 viewMatrix = camera_->GetViewMatrix();
    Matrix4x4 projectionMatrix = camera_->GetProjectionMatrix();
    Matrix4x4 viewProj = Multiply(viewMatrix, projectionMatrix);

    // ── 1. タイトルロゴのモーション計算（画面上部に配置し、中央〜下部の自機を広く見せる） ──
    const Vector3 baseScale = { 5.2f, 5.2f, 4.4f };
    const Vector3 basePos   = { 0.0f, 6.4f, 20.0f };

    float introScaleX = 1.0f;
    float introScaleY = 1.0f;
    float introRotX   = 0.0f;
    float introRotY   = 0.0f;
    float introRotZ   = 0.0f;
    float introOffsetZ = 0.0f;

    if (titleAnimTimer_ < 0.42f) {
        // フェーズA (0.0s〜0.42s): 手前から回転しながら超高速ズームイン突入
        float u = titleAnimTimer_ / 0.42f;
        float easeOut = 1.0f - std::pow(1.0f - u, 4.0f);
        introScaleX  = std::lerp(2.15f, 1.0f, easeOut);
        introScaleY  = std::lerp(0.55f, 1.0f, easeOut);
        introRotX    = (1.0f - easeOut) * 0.35f;
        introRotY    = (1.0f - easeOut) * -0.52f;
        introRotZ    = (1.0f - easeOut) * -0.10f;
        introOffsetZ = (1.0f - easeOut) * -8.0f;
    } else if (titleAnimTimer_ < 0.72f) {
        // フェーズB (0.42s〜0.72s): 着弾インパクトの弾性バウンス
        float bu = 1.0f - (titleAnimTimer_ - 0.42f) / 0.30f;
        float bounce = 0.22f * bu * bu * std::cos((1.0f - bu) * 5.0f);
        introScaleX = 1.0f + bounce;
        introScaleY = 1.0f + bounce;
    }

    // 常時モーション：滑らかな8の字浮遊＆3D立体パースペクティブ揺動（ベベルと金属反射を美しく見せる）
    float floatX   = std::cos(titleAnimTimer_ * 1.15f) * 0.14f;
    float floatY   = std::sin(titleAnimTimer_ * 2.10f) * 0.24f;
    float swayRotX = std::sin(titleAnimTimer_ * 1.65f) * 0.085f;
    float swayRotY = std::sin(titleAnimTimer_ * 1.10f) * 0.135f;
    float swayRotZ = std::cos(titleAnimTimer_ * 1.40f) * 0.022f;

    // 周期的なハートビート（鼓動）パルス (1.8秒周期でドクンと力強く脈動)
    float beatCycle = std::fmod((std::max)(0.0f, titleAnimTimer_ - 0.72f), 1.8f);
    float beatPulse = 0.0f;
    if (titleAnimTimer_ >= 0.72f && beatCycle < 0.36f) {
        float bp = beatCycle / 0.36f;
        beatPulse = std::sin(bp * static_cast<float>(M_PI)) * (1.0f - bp * 0.4f);
    }

    Vector3 currentPos = {
        basePos.x + floatX,
        basePos.y + floatY,
        basePos.z + introOffsetZ
    };
    Vector3 currentRotate = {
        introRotX + swayRotX,
        introRotY + swayRotY,
        introRotZ + swayRotZ
    };
    Vector3 currentScale = {
        baseScale.x * introScaleX * (1.0f + beatPulse * 0.065f),
        baseScale.y * introScaleY * (1.0f + beatPulse * 0.065f),
        baseScale.z * (1.0f + beatPulse * 0.04f)
    };

    // ── 2. スポットライトによるリアルタイム光沢走査（文字表面を左→右へ走る落ち着いた輝き） ──
    if (directionalLightData_) {
        float sweepCycle = std::fmod(titleAnimTimer_ * 0.65f, 1.0f);
        // -20.0f から +20.0f へ滑らかに走査
        float sweepX = std::lerp(-20.0f, 20.0f, sweepCycle);
        directionalLightData_->spotLightPos = { sweepX, currentPos.y + 1.0f, currentPos.z - 11.0f };
        directionalLightData_->spotLightDir = Normalize({ -sweepX * 0.18f, -0.1f, 1.0f });
        directionalLightData_->spotLightIntensity = 1.65f + beatPulse * 1.1f;
    }

    // ── 3. 各3Dモデルレイヤーのトランスフォーム・カラー更新 ──
    // (1) メイン前面3D文字（深みのあるダーククリムゾン〜重厚な紅色の脈動）
    if (titleModel_ && transformData_) {
        titleModel_->transform.scale     = currentScale;
        titleModel_->transform.rotate    = currentRotate;
        titleModel_->transform.translate = currentPos;
        titleModel_->SetColor({
            0.62f + 0.14f * beatPulse,
            0.04f + 0.05f * beatPulse,
            0.04f + 0.03f * beatPulse,
            1.0f
        });

        Matrix4x4 worldMatrix = MakeAffineMatrix(titleModel_->transform.scale, titleModel_->transform.rotate, titleModel_->transform.translate);
        transformData_->World = worldMatrix;
        transformData_->WVP   = Multiply(worldMatrix, viewProj);
    }

    // (2) 重厚ブロンズゴールド・ベベルリム層（文字の周囲を落ち着いて縁取る金属エッジ）
    if (titleRimModel_ && rimTransformData_) {
        titleRimModel_->transform.scale = {
            currentScale.x * 1.030f,
            currentScale.y * 1.045f,
            currentScale.z * 1.020f
        };
        titleRimModel_->transform.rotate = currentRotate;
        titleRimModel_->transform.translate = {
            currentPos.x,
            currentPos.y,
            currentPos.z + 0.24f
        };
        titleRimModel_->SetColor({
            0.65f + 0.15f * beatPulse,
            0.38f + 0.12f * beatPulse,
            0.10f + 0.08f * beatPulse,
            1.0f
        });

        Matrix4x4 rimWorld = MakeAffineMatrix(titleRimModel_->transform.scale, titleRimModel_->transform.rotate, titleRimModel_->transform.translate);
        rimTransformData_->World = rimWorld;
        rimTransformData_->WVP   = Multiply(rimWorld, viewProj);
    }

    // (3) 漆黒＆ダーククリムゾン立体シャドウ層（文字単体で背景からくっきり分離させる太縁取り）
    if (titleShadowModel_ && shadowTransformData_) {
        titleShadowModel_->transform.scale = {
            currentScale.x * 1.085f,
            currentScale.y * 1.135f,
            currentScale.z * 1.055f
        };
        titleShadowModel_->transform.rotate = currentRotate;
        titleShadowModel_->transform.translate = {
            currentPos.x + 0.15f,
            currentPos.y - 0.18f,
            currentPos.z + 0.58f
        };

        Matrix4x4 shadowWorld = MakeAffineMatrix(titleShadowModel_->transform.scale, titleShadowModel_->transform.rotate, titleShadowModel_->transform.translate);
        shadowTransformData_->World = shadowWorld;
        shadowTransformData_->WVP   = Multiply(shadowWorld, viewProj);
    }

    // (4) 背面へ周期的に拡大・拡散する2重の3Dダークオーラ残像層
    for (int i = 0; i < 2; ++i) {
        if (titleAuraModels_[i] && auraTransformData_[i]) {
            float auraPhase = std::fmod(titleAnimTimer_ * 1.25f + i * 0.5f, 1.0f);
            float expand = 1.05f + 0.34f * auraPhase;
            float alpha  = (1.0f - auraPhase) * (1.0f - auraPhase) * (0.28f + 0.12f * beatPulse);

            titleAuraModels_[i]->transform.scale = {
                currentScale.x * expand,
                currentScale.y * (1.05f + 0.42f * auraPhase),
                currentScale.z
            };
            titleAuraModels_[i]->transform.rotate = currentRotate;
            titleAuraModels_[i]->transform.translate = {
                currentPos.x,
                currentPos.y,
                currentPos.z + 0.95f + i * 0.35f
            };

            if (i == 0) {
                // 深紅ダークオーラ
                titleAuraModels_[i]->SetColor({ 0.58f, 0.05f, 0.04f, alpha });
            } else {
                // 暗金色オーラ
                titleAuraModels_[i]->SetColor({ 0.62f, 0.35f, 0.08f, alpha * 0.75f });
            }

            Matrix4x4 auraWorld = MakeAffineMatrix(titleAuraModels_[i]->transform.scale, titleAuraModels_[i]->transform.rotate, titleAuraModels_[i]->transform.translate);
            auraTransformData_[i]->World = auraWorld;
            auraTransformData_[i]->WVP   = Multiply(auraWorld, viewProj);
            auraTransformData_[i]->color = { 1.0f, 1.0f, 1.0f, 1.0f };
        }
    }

    cameraDataCB_->worldPosition = camera_->GetTransform().translate;

    if (inputDelay_ > 0) {
        inputDelay_--;
    } else if (!isTransitioningToGame_) {
        // SPACEキーまたはENTERキー押下でブースト加速＆斜め斬撃画面遷移を開始
        if (input_->IsKeyTriggered(DIK_SPACE) || input_->IsKeyTriggered(DIK_RETURN)) {
            isTransitioningToGame_ = true;
            transitionTimer_ = 0.0f;
            if (gameplayScene_) {
                gameplayScene_->TriggerTitleTransitionBoost();
            }
            // 斜め斬撃＆上下分割による画面遷移を即座に開始
            SceneManager::GetInstance()->ChangeScene("GAMEPLAY");
        }
    }
}

void TitleScene::Draw() {
    // 1. 背景ゲームシーンの描画（画面中央〜下部に自機がダイナミックに映る）
    if (gameplayScene_) {
        gameplayScene_->Draw();
    }

    ID3D12GraphicsCommandList* commandList = DirectXCommon::GetInstance()->GetCommandList();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = DirectXCommon::GetInstance()->GetDsvHandle();

    // 2. 深度バッファのみをクリア（背景画像の上にタイトル演出と3Dモデルを重ねるため）
    DirectXCommon::GetInstance()->TransitionDepthStencilState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // デスクリプタヒープのセット
    ID3D12DescriptorHeap* modelHeaps[] = { TextureManager::GetInstance()->GetSrvHeap() };
    commandList->SetDescriptorHeaps(1, modelHeaps);

    const float clientW = static_cast<float>(WinApp::kClientWidth);
    const float clientH = static_cast<float>(WinApp::kClientHeight);
    const float halfClientW = clientW * 0.5f;
    const float halfClientH = clientH * 0.5f;
    Matrix4x4 projectionSprite = MakeOrthographicMatrix(-halfClientW, halfClientH, halfClientW, -halfClientH, 0.0f, 100.0f);
    Matrix4x4 viewProjSprite = Multiply(MakeIdentity4x4(), projectionSprite);

    // 中心基準＆任意のZ回転でスプライトを描画するヘルパーラムダ
    auto drawCenteredSprite = [&](Sprite* spr, float cx, float cy, float w, float h, float rotZ, const Vector4& col) {
        if (!spr || w <= 0.1f || h <= 0.1f || col.w <= 0.001f) return;
        float sx = w;
        float sy = -h; // 正射影(+Yが上)での上下反転補正
        float cosR = std::cos(rotZ);
        float sinR = std::sin(rotZ);
        spr->transform.scale = { sx, sy, 1.0f };
        spr->transform.rotate = { 0.0f, 0.0f, rotZ };
        spr->transform.translate = {
            cx - (0.5f * sx * cosR + 0.5f * sy * sinR),
            cy - (-0.5f * sx * sinR + 0.5f * sy * cosR),
            0.0f
        };
        spr->SetColor(col);
        spr->Draw(commandList, viewProjSprite);
    };

    float t = titleAnimTimer_;
    float barOpen = std::clamp(t / 0.32f, 0.0f, 1.0f);
    barOpen = 1.0f - (1.0f - barOpen) * (1.0f - barOpen);

    float beatCycle = std::fmod((std::max)(0.0f, t - 0.72f), 1.8f);
    float beatPulse = 0.0f;
    if (t >= 0.72f && beatCycle < 0.36f) {
        float bp = beatCycle / 0.36f;
        beatPulse = std::sin(bp * static_cast<float>(M_PI)) * (1.0f - bp * 0.4f);
    }

    // 3Dタイトルモデルの中心座標に対応する2Dスクリーン座標 (Y=6.4, Z=20, CameraZ=-40 -> 画面上部の +168px 付近)
    float titleScreenX = (titleModel_ ? titleModel_->transform.translate.x : 0.0f) * 26.216f;
    float titleScreenY = (titleModel_ ? titleModel_->transform.translate.y : 6.4f) * 26.216f;
    const float visorTilt = -0.018f; // 約 -1.0度のシャープな傾斜

    // ── 3. 背景のプレイヤーや街を隠さない透過型・局所ダークフレーム＆控えめな発光演出（画面上部の文字周辺のみ） ──
    if (graphicsPipeline_ && graphicsPipeline_->GetSpriteRootSignature() && graphicsPipeline_->GetSpritePipelineState()) {
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetSpriteRootSignature());
        commandList->SetPipelineState(graphicsPipeline_->GetSpritePipelineState());

        // (1) 文字の直後だけを暗く引き締める局所ダークシャドウ＆落ち着いた深紅グロー（circle2.pngで周囲は完全透過）
        float glowScale = 1.0f + 0.08f * std::sin(t * 3.2f) + beatPulse * 0.14f;
        drawCenteredSprite(visorBandSprite_.get(),        titleScreenX, titleScreenY, 640.0f * barOpen, 175.0f * barOpen, visorTilt, { 0.01f, 0.002f, 0.01f, 0.62f * barOpen });
        drawCenteredSprite(backdropGlowSprites_[0].get(), titleScreenX, titleScreenY, 540.0f * glowScale, 145.0f * glowScale, visorTilt, { 0.45f, 0.04f, 0.04f, (0.22f + 0.10f * beatPulse) * barOpen });
        drawCenteredSprite(backdropGlowSprites_[1].get(), titleScreenX, titleScreenY, 380.0f * glowScale, 100.0f * glowScale, visorTilt, { 0.55f, 0.28f, 0.06f, (0.14f + 0.10f * beatPulse) * barOpen });

        // (2) タイトル文字の上下だけをスタイリッシュに挟むスリム・レーザーフレーム（落ち着いた深紅＆ブロンズゴールド）
        float railSlide = (t < 0.38f) ? (1.0f - std::pow(1.0f - t / 0.38f, 3.0f)) : 1.0f;
        float railOffsetX = (1.0f - railSlide) * 420.0f;
        float halfFrameH = (66.0f + beatPulse * 6.0f) * barOpen;
        float frameW = 580.0f * barOpen;
        float topRailY = titleScreenY + halfFrameH;
        float botRailY = titleScreenY - halfFrameH;

        // 上スリムレーザーライン
        drawCenteredSprite(visorRailSprites_[0].get(), titleScreenX - railOffsetX, topRailY, frameW, 5.0f, visorTilt, { 0.55f, 0.06f, 0.05f, 0.42f * barOpen });
        drawCenteredSprite(visorRailSprites_[1].get(), titleScreenX - railOffsetX, topRailY, frameW * 0.92f, 2.0f, visorTilt, { 0.78f, 0.48f, 0.16f, 0.75f * barOpen });
        // 下スリムレーザーライン
        drawCenteredSprite(visorRailSprites_[2].get(), titleScreenX + railOffsetX, botRailY, frameW, 5.0f, visorTilt, { 0.55f, 0.06f, 0.05f, 0.42f * barOpen });
        drawCenteredSprite(visorRailSprites_[3].get(), titleScreenX + railOffsetX, botRailY, frameW * 0.92f, 2.0f, visorTilt, { 0.78f, 0.48f, 0.16f, 0.75f * barOpen });

        // (3) タイトル周辺（画面上部）だけを走る落ち着いたストリーク光線（6本）
        const float streakOffsetsY[6] = { 46.0f, -42.0f, 22.0f, -20.0f, 56.0f, -54.0f };
        const float streakWidths[6]   = { 240.0f, 280.0f, 190.0f, 260.0f, 160.0f, 220.0f };
        const float streakSpeeds[6]   = { 1400.0f, -1650.0f, 1850.0f, -1350.0f, 1150.0f, -1750.0f };
        for (int i = 0; i < 6; ++i) {
            float spanW = 760.0f;
            float travel = std::fmod(t * streakSpeeds[i] + i * 190.0f, spanW);
            float sx = titleScreenX + ((streakSpeeds[i] > 0.0f) ? (-spanW * 0.5f + travel) : (spanW * 0.5f - travel));
            float edgeFade = std::sin(std::clamp(std::abs(travel) / spanW, 0.0f, 1.0f) * static_cast<float>(M_PI));
            float sy = titleScreenY + streakOffsetsY[i] * barOpen + (sx - titleScreenX) * std::tan(-visorTilt);
            Vector4 sCol = (i % 2 == 0)
                ? Vector4{ 0.75f, 0.46f, 0.15f, 0.38f * edgeFade * barOpen }
                : Vector4{ 0.65f, 0.08f, 0.06f, 0.45f * edgeFade * barOpen };
            drawCenteredSprite(streakSprites_[i].get(), sx, sy, streakWidths[i] * edgeFade, 2.0f, visorTilt, sCol);
        }

        // (4) タイトル左右を彩る回転サイバーダイヤモンド・エンブレム（2重構造×左右）
        if (barOpen > 0.05f) {
            float diaRotOuter = static_cast<float>(M_PI) * 0.25f + t * 2.2f;
            float diaRotInner = static_cast<float>(M_PI) * 0.25f - t * 3.4f;
            float diaScaleOuter = (22.0f + beatPulse * 6.0f) * barOpen;
            float diaScaleInner = (12.0f + beatPulse * 3.5f) * barOpen;
            float diaOffsetX = 325.0f + (1.0f - barOpen) * 140.0f;
            float leftY  = titleScreenY - diaOffsetX * std::tan(-visorTilt);
            float rightY = titleScreenY + diaOffsetX * std::tan(-visorTilt);

            drawCenteredSprite(diamondSprites_[0].get(), titleScreenX - diaOffsetX, leftY,  diaScaleOuter, diaScaleOuter, diaRotOuter, { 0.62f, 0.08f, 0.06f, 0.75f * barOpen });
            drawCenteredSprite(diamondSprites_[1].get(), titleScreenX - diaOffsetX, leftY,  diaScaleInner, diaScaleInner, diaRotInner, { 0.78f, 0.48f, 0.16f, 0.82f * barOpen });
            drawCenteredSprite(diamondSprites_[2].get(), titleScreenX + diaOffsetX, rightY, diaScaleOuter, diaScaleOuter, -diaRotOuter, { 0.62f, 0.08f, 0.06f, 0.75f * barOpen });
            drawCenteredSprite(diamondSprites_[3].get(), titleScreenX + diaOffsetX, rightY, diaScaleInner, diaScaleInner, -diaRotInner, { 0.78f, 0.48f, 0.16f, 0.82f * barOpen });
        }

        // (5) タイトル周囲を舞い上がる深紅＆暗金色の火の粉（エンバースパーク粒子 12個）
        for (int i = 0; i < 12; ++i) {
            float seed = static_cast<float>(i) * 1.37f;
            float cycle = std::fmod(t * (0.45f + (i % 4) * 0.12f) + seed, 1.0f);
            float baseX = ((static_cast<float>(i) / 11.0f) * 2.0f - 1.0f) * 290.0f;
            float swayX = std::sin(t * 2.8f + seed * 3.0f) * 22.0f;
            float ex = titleScreenX + baseX + swayX;
            float ey = titleScreenY - 72.0f + cycle * 150.0f;
            float eAlpha = std::sin(cycle * static_cast<float>(M_PI)) * 0.65f * barOpen;
            float eSize = (5.0f + (i % 3) * 3.5f) * (0.7f + 0.3f * std::sin(t * 8.0f + seed));
            Vector4 eCol = (i % 3 == 0)
                ? Vector4{ 0.78f, 0.48f, 0.16f, eAlpha }
                : Vector4{ 0.68f, 0.08f, 0.06f, eAlpha };
            drawCenteredSprite(emberSprites_[i].get(), ex, ey, eSize, eSize, 0.0f, eCol);
        }
    }

    // ── 4. 4層マルチレイヤー3Dタイトルモデル「巣喰う街」の描画 ──
    if (graphicsPipeline_ && graphicsPipeline_->GetObject3dPipelineState() && graphicsPipeline_->GetObject3dRootSignature()) {
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetObject3dRootSignature());

        // ライト、カメラの定数バッファをセット
        if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
        if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());

        D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = TextureManager::GetInstance()->GetSrvHandleGPU("Title/white1x1.png");
        D3D12_GPU_DESCRIPTOR_HANDLE envSrv   = TextureManager::GetInstance()->GetSrvHandleGPU("test.dds");

        // 第1層（最背面）：周期的に拡大・拡散する2重の3Dオーラ残像（半透明アルファブレンド）
        if (graphicsPipeline_->GetObject3dBlendNormalPipelineState()) {
            commandList->SetPipelineState(graphicsPipeline_->GetObject3dBlendNormalPipelineState());
            for (int i = 1; i >= 0; --i) {
                if (titleAuraModels_[i] && auraTransformResources_[i]) {
                    commandList->SetGraphicsRootConstantBufferView(1, auraTransformResources_[i]->GetGPUVirtualAddress());
                    titleAuraModels_[i]->DrawModel(commandList, whiteSrv, envSrv);
                }
            }
        }

        // 不透明3D描画パイプラインに切り替え
        commandList->SetPipelineState(graphicsPipeline_->GetObject3dPipelineState());

        // 第2層：重厚ダークシルエット＆立体ドロップシャドウ層（黒紅の太縁取りで視認性を極限まで高める）
        if (titleShadowModel_ && shadowTransformResource_) {
            commandList->SetGraphicsRootConstantBufferView(1, shadowTransformResource_->GetGPUVirtualAddress());
            titleShadowModel_->DrawModel(commandList, whiteSrv, envSrv);
        }

        // 第3層：黄金メタリック・ベベルリム層（環境マップ反射を伴うゴールドエッジ）
        if (titleRimModel_ && rimTransformResource_) {
            commandList->SetGraphicsRootConstantBufferView(1, rimTransformResource_->GetGPUVirtualAddress());
            titleRimModel_->DrawModel(commandList, whiteSrv, envSrv);
        }

        // 第4層（最前面）：メイン3Dタイトル文字（紅蓮スカーレット＋リアルタイム走査スポットライト光沢）
        if (titleModel_ && transformResource_) {
            commandList->SetGraphicsRootConstantBufferView(1, transformResource_->GetGPUVirtualAddress());
            titleModel_->DrawModel(commandList, whiteSrv, envSrv);
        }
    }

    // ── 5. 登場着弾時の最前面スラッシュ閃光ビーム (0.38s〜0.68s) ──
    if (t >= 0.38f && t < 0.68f && impactFlashSprite_ && graphicsPipeline_ && graphicsPipeline_->GetSpriteRootSignature() && graphicsPipeline_->GetSpritePipelineState()) {
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetSpriteRootSignature());
        commandList->SetPipelineState(graphicsPipeline_->GetSpritePipelineState());
        float u = (t - 0.38f) / 0.30f;
        float beamH = 42.0f * (1.0f - u) * (1.0f - u);
        float beamAlpha = (1.0f - u);
        drawCenteredSprite(impactFlashSprite_.get(), 0.0f, titleScreenY, clientW + 200.0f, beamH, visorTilt, { 1.0f, 0.95f, 0.80f, beamAlpha });
    }
}