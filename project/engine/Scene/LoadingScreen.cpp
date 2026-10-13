#include "LoadingScreen.h"
#include "DirectXCommon.h"
#include "WinApp.h"
#include "TextureManager.h"
#include "SrvManager.h"
#include "GraphicsPipeline.h"
#include "SceneManager.h"
#include "D3D12Util.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace {
    constexpr float kSlashTitleDuration   = 0.44f; // タイトル画面を左斜め上→右斜め下へ斬る時間
    constexpr float kSplitTitleDuration   = 0.68f; // 切れたタイトル画面が上下へ移動する時間
    constexpr float kSlashLoadingDuration = 0.44f; // [パターン0] ロード画面を右斜め下→左斜め上へ斬る時間
    constexpr float kSplitLoadingDuration = 0.70f; // [パターン0] 切れたロード画面が左右へ移動する時間

    // [パターン1] 多方向連続斬撃 (5回×0.19秒 = 0.95秒) ＋ 正面突入チャージ (0.38秒) ＝ 1.33秒
    constexpr int   kNumMultiSlashes         = 5;
    constexpr float kSingleSlashTime         = 0.19f;
    constexpr float kMultiSlashPhaseTime     = kSingleSlashTime * (float)kNumMultiSlashes; // 0.95f
    constexpr float kFrontDiveTime           = 0.38f;
    constexpr float kMultiSlashTotalDuration = kMultiSlashPhaseTime + kFrontDiveTime;      // 1.33f
    constexpr float kGlassShatterDuration    = 0.92f; // ガラス破片が飛び散りプレイヤー機と重なる時間

    struct SlashDef {
        float x0, y0;
        float x1, y1;
    };

    // 画面を様々な角度から横断する5本の斬撃軌道（すべて中央付近 (640, 400) を取り囲むように交差）
    constexpr SlashDef kMultiSlashDefs[kNumMultiSlashes] = {
        { 1380.0f,  740.0f, -100.0f,  -20.0f }, // 1. 右斜め下 -> 左斜め上
        { -100.0f,  680.0f, 1380.0f,   60.0f }, // 2. 左斜め下 -> 右斜め上
        {  220.0f,  -90.0f, 1060.0f,  810.0f }, // 3. 左上中央 -> 右下中央
        { 1400.0f,  320.0f, -120.0f,  470.0f }, // 4. 右中段   -> 左中段
        { 1240.0f,  -90.0f,  140.0f,  810.0f }  // 5. 右上     -> 左下
    };

    // 2D線分を頂点配列へ追加するヘルパー（先端・終端で太さを変えられるテーパー対応）
    void AppendTaperedLineVertices(
        std::vector<VertexData>& outVerts,
        float x0, float y0, float x1, float y1,
        float thicknessStart, float thicknessEnd,
        uint32_t maxVerts) {
        if (outVerts.size() + 6 > maxVerts) return;
        float dx = x1 - x0;
        float dy = y1 - y0;
        float len = std::hypot(dx, dy);
        if (len < 0.5f) return;

        float nx = -dy / len;
        float ny =  dx / len;
        float h0x = nx * (thicknessStart * 0.5f);
        float h0y = ny * (thicknessStart * 0.5f);
        float h1x = nx * (thicknessEnd * 0.5f);
        float h1y = ny * (thicknessEnd * 0.5f);

        VertexData v0{ { x0 - h0x, y0 - h0y, 0.0f, 1.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f } };
        VertexData v1{ { x0 + h0x, y0 + h0y, 0.0f, 1.0f }, { 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f } };
        VertexData v2{ { x1 + h1x, y1 + h1y, 0.0f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f, -1.0f } };
        VertexData v3{ { x1 - h1x, y1 - h1y, 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f } };

        outVerts.push_back(v0);
        outVerts.push_back(v1);
        outVerts.push_back(v2);
        outVerts.push_back(v0);
        outVerts.push_back(v2);
        outVerts.push_back(v3);
    }

    // 斬撃ラインから枝分かれするガラスのひび割れを追加するヘルパー
    void AppendSlashWithBranchCracks(
        std::vector<VertexData>& glowVerts,
        std::vector<VertexData>& coreVerts,
        float x0, float y0, float x1, float y1,
        int slashIdx,
        uint32_t maxVerts) {
        // メインの斬撃ライン
        AppendTaperedLineVertices(glowVerts, x0, y0, x1, y1, 22.0f, 14.0f, maxVerts);
        AppendTaperedLineVertices(coreVerts, x0, y0, x1, y1,  5.5f,  3.0f, maxVerts);

        // 斬撃ライン上の複数箇所から枝分かれするガラスの亀裂
        float dx = x1 - x0;
        float dy = y1 - y0;
        float len = std::hypot(dx, dy);
        if (len < 80.0f) return;

        float baseAngle = std::atan2(dy, dx);
        const int numBranches = 5;
        for (int b = 0; b < numBranches; ++b) {
            float frac = 0.18f + 0.16f * (float)b;
            if (frac > 0.92f) break;
            float bx = x0 + dx * frac;
            float by = y0 + dy * frac;

            float side = ((slashIdx + b) % 2 == 0) ? 1.0f : -1.0f;
            float branchAngle = baseAngle + side * (0.48f + 0.15f * (float)((slashIdx * 3 + b) % 3));
            float branchLen = (42.0f + 22.0f * (float)((slashIdx + b * 2) % 4)) * (len / 1400.0f);

            float mx = bx + std::cos(branchAngle) * branchLen;
            float my = by + std::sin(branchAngle) * branchLen;

            AppendTaperedLineVertices(glowVerts, bx, by, mx, my, 8.0f, 2.0f, maxVerts);
            AppendTaperedLineVertices(coreVerts, bx, by, mx, my, 2.4f, 0.6f, maxVerts);

            // 亀裂の先端がさらに折れ曲がる二次亀裂
            float subAngle = branchAngle - side * 0.35f;
            float subLen = branchLen * 0.65f;
            float ex = mx + std::cos(subAngle) * subLen;
            float ey = my + std::sin(subAngle) * subLen;
            AppendTaperedLineVertices(glowVerts, mx, my, ex, ey, 4.5f, 1.0f, maxVerts);
            AppendTaperedLineVertices(coreVerts, mx, my, ex, ey, 1.5f, 0.4f, maxVerts);
        }
    }
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

    // ダイナミックバッチ用バッファ初期化ラムダ
    auto initDynamicBatch = [&](
        uint32_t maxVerts,
        Microsoft::WRL::ComPtr<ID3D12Resource>& vbRes,
        VertexData*& mappedPtr,
        D3D12_VERTEX_BUFFER_VIEW& vbView,
        Microsoft::WRL::ComPtr<ID3D12Resource>& matRes,
        Material*& matData,
        Microsoft::WRL::ComPtr<ID3D12Resource>& wvpRes,
        TransformationMatrix*& wvpData) {
        vbRes = CreateBufferResource(device, sizeof(VertexData) * maxVerts);
        vbRes->Map(0, nullptr, reinterpret_cast<void**>(&mappedPtr));
        vbView.BufferLocation = vbRes->GetGPUVirtualAddress();
        vbView.SizeInBytes = sizeof(VertexData) * maxVerts;
        vbView.StrideInBytes = sizeof(VertexData);

        matRes = CreateBufferResource(device, sizeof(Material));
        matRes->Map(0, nullptr, reinterpret_cast<void**>(&matData));
        matData->color = { 1.0f, 1.0f, 1.0f, 1.0f };
        matData->enableLighting = 0;
        matData->uvTransform = MakeIdentity4x4();

        wvpRes = CreateBufferResource(device, sizeof(TransformationMatrix));
        wvpRes->Map(0, nullptr, reinterpret_cast<void**>(&wvpData));
        wvpData->WVP = MakeIdentity4x4();
        wvpData->World = MakeIdentity4x4();
        wvpData->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    };

    initDynamicBatch(kMaxBatchVertices, slashGlowBatchVbRes_, slashGlowBatchMapped_, slashGlowBatchVbView_, slashGlowBatchMatRes_, slashGlowBatchMatData_, slashGlowBatchWvpRes_, slashGlowBatchWvpData_);
    initDynamicBatch(kMaxBatchVertices, slashCoreBatchVbRes_, slashCoreBatchMapped_, slashCoreBatchVbView_, slashCoreBatchMatRes_, slashCoreBatchMatData_, slashCoreBatchWvpRes_, slashCoreBatchWvpData_);
    initDynamicBatch(kMaxShardVertices, shatterTexVbRes_,     shatterTexMapped_,     shatterTexVbView_,     shatterTexMatRes_,     shatterTexMatData_,     shatterTexWvpRes_,     shatterTexWvpData_);
    initDynamicBatch(kMaxShardVertices, shatterGlintVbRes_,   shatterGlintMapped_,   shatterGlintVbView_,   shatterGlintMatRes_,   shatterGlintMatData_,   shatterGlintWvpRes_,   shatterGlintWvpData_);

    // 112枚のガラス破片メッシュを構築
    BuildGlassShards();

    // 画面を横断・突入して斬るプレイヤー機モデル (Player2/Player.obj)
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

void LoadingScreen::BuildGlassShards() {
    glassShards_.clear();

    const float W = (float)WinApp::kClientWidth;
    const float H = (float)WinApp::kClientHeight;
    // 突入するプレイヤー機が衝突する画面中央やや下のインパクト中心
    const float cx = 640.0f;
    const float cy = 400.0f;

    constexpr int kSectors = 16;
    constexpr int kRings   = 4;

    // 4隅 (0,0), (W,0), (W,H), (0,H) を確実に通る16方向の基準角度配列
    float cornerAngles[4] = {
        std::atan2(0.0f - cy, 0.0f - cx), // 左上 (-2.58 rad)
        std::atan2(0.0f - cy, W    - cx), // 右上 (-0.56 rad)
        std::atan2(H    - cy, W    - cx), // 右下 (+0.46 rad)
        std::atan2(H    - cy, 0.0f - cx)  // 左下 (+2.68 rad)
    };

    float baseAngles[kSectors]{};
    for (int c = 0; c < 4; ++c) {
        float a0 = cornerAngles[c];
        float a1 = cornerAngles[(c + 1) % 4];
        if (a1 < a0) a1 += 2.0f * std::numbers::pi_v<float>;
        for (int sub = 0; sub < 4; ++sub) {
            float t = (float)sub / 4.0f;
            baseAngles[c * 4 + sub] = a0 + (a1 - a0) * t;
        }
    }

    const float ringRadii[kRings + 1] = { 0.0f, 92.0f, 220.0f, 395.0f, 1000.0f };
    Vector2 ringPts[kRings + 1][kSectors]{};

    for (int s = 0; s < kSectors; ++s) {
        ringPts[0][s] = { cx, cy };
    }

    for (int r = 1; r <= kRings; ++r) {
        for (int s = 0; s < kSectors; ++s) {
            if (r == kRings) {
                // 最外周リングは画面境界 [0, W] x [0, H] に隙間なくぴったり一致させる
                float ang = baseAngles[s];
                float dx = std::cos(ang);
                float dy = std::sin(ang);
                float tx = 1e9f;
                float ty = 1e9f;
                if (dx > 0.0001f)       tx = (W - cx) / dx;
                else if (dx < -0.0001f) tx = (0.0f - cx) / dx;
                if (dy > 0.0001f)       ty = (H - cy) / dy;
                else if (dy < -0.0001f) ty = (0.0f - cy) / dy;
                float tBorder = (std::min)(tx, ty);
                float px = std::clamp(cx + dx * tBorder, 0.0f, W);
                float py = std::clamp(cy + dy * tBorder, 0.0f, H);
                ringPts[r][s] = { px, py };
            } else {
                // 内側リングは自然なガラス破片になるよう角度・半径に決定論的ジッターを付与
                float jitterAng = std::sin((float)(r * 13 + s * 7) * 1.9f) * 0.09f;
                float jitterRad = std::cos((float)(r * 17 + s * 11) * 2.3f) * (ringRadii[r] * 0.16f);
                float ang = baseAngles[s] + jitterAng;
                float rad = ringRadii[r] + jitterRad;
                float px = std::clamp(cx + std::cos(ang) * rad, 8.0f, W - 8.0f);
                float py = std::clamp(cy + std::sin(ang) * rad, 8.0f, H - 8.0f);
                ringPts[r][s] = { px, py };
            }
        }
    }

    auto addShardTriangle = [&](const Vector2& p0, const Vector2& p1, const Vector2& p2, int ringIdx, int secIdx) {
        GlassShard shard{};
        shard.basePos[0] = p0;
        shard.basePos[1] = p1;
        shard.basePos[2] = p2;
        for (int k = 0; k < 3; ++k) {
            shard.uv[k] = {
                std::clamp(shard.basePos[k].x / W, 0.0f, 1.0f),
                std::clamp(shard.basePos[k].y / H, 0.0f, 1.0f)
            };
        }
        shard.center = {
            (p0.x + p1.x + p2.x) / 3.0f,
            (p0.y + p1.y + p2.y) / 3.0f
        };

        float dx = shard.center.x - cx;
        float dy = shard.center.y - cy;
        float dist = std::hypot(dx, dy);
        float dirX = (dist > 0.1f) ? (dx / dist) : std::cos((float)secIdx * 0.4f);
        float dirY = (dist > 0.1f) ? (dy / dist) : std::sin((float)secIdx * 0.4f);

        // ランダム性のある飛散速度（全破片が0.85秒以内に確実に画面外へ飛び散る初速）
        float hash = std::sin((float)(ringIdx * 31 + secIdx * 17) * 3.17f);
        float speed = 980.0f + (float)(4 - ringIdx) * 140.0f + std::abs(hash) * 360.0f;
        shard.vel = {
            dirX * speed + hash * 120.0f,
            dirY * speed - 180.0f // 衝突の衝撃で一度少し跳ね上がってから重力で落下
        };

        // 3D空間でクルクル回転しながら飛び散る角速度
        shard.rotXSpeed = (6.5f + (float)((ringIdx * 7 + secIdx * 3) % 7) * 1.4f) * ((secIdx % 2 == 0) ? 1.0f : -1.0f);
        shard.rotYSpeed = (7.2f + (float)((ringIdx * 5 + secIdx * 9) % 6) * 1.5f) * ((ringIdx % 2 == 0) ? 1.0f : -1.0f);
        shard.rotZSpeed = (3.8f + (float)((ringIdx * 3 + secIdx * 5) % 5) * 1.1f) * (((ringIdx + secIdx) % 2 == 0) ? 1.0f : -1.0f);
        shard.scaleSpeed = (ringIdx <= 2) ? 0.35f : 0.12f;
        shard.delay = (float)(ringIdx - 1) * 0.010f;

        glassShards_.push_back(shard);
    };

    for (int s = 0; s < kSectors; ++s) {
        int sNext = (s + 1) % kSectors;
        // 中心から第1リングへの16枚の三角形
        addShardTriangle({ cx, cy }, ringPts[1][s], ringPts[1][sNext], 1, s);

        // 第1〜第4リング間のクアッドをそれぞれ2枚の三角形に分割（計96枚、合計112枚）
        for (int r = 1; r < kRings; ++r) {
            addShardTriangle(ringPts[r][s], ringPts[r + 1][s], ringPts[r + 1][sNext], r + 1, s * 2);
            addShardTriangle(ringPts[r][s], ringPts[r + 1][sNext], ringPts[r][sNext], r + 1, s * 2 + 1);
        }
    }
}

void LoadingScreen::Reset() {
    phase_ = TransitionPhase::kSlashTitle;
    phaseTimer_ = 0.0f;
    time_ = 0.0f;
    currentProgress_ = 0.0f;
    targetProgress_ = 0.0f;
    hasCapturedTitle_ = false;
    hasCapturedLoading_ = false;
    hasCapturedCrackedLoading_ = false;
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
        // 1. 左斜め上から右斜め下へプレイヤー機が横断してタイトル画面を切る
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
        if (targetProgress_ >= 0.99f && currentProgress_ >= 0.99f && phaseTimer_ >= 0.35f && hasCapturedLoading_) {
            if (exitPattern_ == ExitPattern::kMultiSlashGlassShatter) {
                phase_ = TransitionPhase::kMultiSlashLoading;
            } else {
                phase_ = TransitionPhase::kSlashLoadingReverse;
            }
            phaseTimer_ = 0.0f;
        }
        break;

    case TransitionPhase::kSlashLoadingReverse:
        // [パターン0] 右斜め下から左斜め上へプレイヤー機が逆横断してロード画面を切る
        currentProgress_ = 1.0f;
        if (phaseTimer_ >= kSlashLoadingDuration) {
            phase_ = TransitionPhase::kSplitLoadingLeftRight;
            phaseTimer_ = 0.0f;
        }
        break;

    case TransitionPhase::kSplitLoadingLeftRight:
        // [パターン0] 切れたロード画面が左右へ移動し、完全に画面外へ出たら遷移完了
        currentProgress_ = 1.0f;
        if (phaseTimer_ >= kSplitLoadingDuration) {
            phase_ = TransitionPhase::kCompleted;
            phaseTimer_ = 0.0f;
            isTransitionCompleted_ = true;
        }
        break;

    case TransitionPhase::kMultiSlashLoading:
        // [パターン1] 色んな方向から5回連続で斬りつけて切れ目を入れ、正面から突っ込む
        currentProgress_ = 1.0f;
        if (phaseTimer_ >= kMultiSlashTotalDuration) {
            phase_ = TransitionPhase::kGlassShatterAndOverlap;
            phaseTimer_ = 0.0f;
        }
        break;

    case TransitionPhase::kGlassShatterAndOverlap:
        // [パターン1] ガラスが割れて飛び散り、突っ込んだプレイヤー機がゲーム内プレイヤーモデルと重なって始まる
        currentProgress_ = 1.0f;
        if (phaseTimer_ >= kGlassShatterDuration) {
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

void LoadingScreen::DrawLoadingBase(ID3D12GraphicsCommandList* commandList, const Matrix4x4& orthoProjection, Vector2 shakeOffset) {
    if (background_) {
        background_->transform.translate = { 0.0f, 0.0f, 0.0f };
        background_->Draw(commandList, orthoProjection);
    }
    if (textBg_) {
        textBg_->transform.translate = { 980.0f + shakeOffset.x, 655.0f + shakeOffset.y, 0.0f };
        textBg_->Draw(commandList, orthoProjection);
    }
    if (textFill_ && currentProgress_ > 0.001f) {
        textFill_->transform.translate = { 980.0f + shakeOffset.x, 655.0f + shakeOffset.y, 0.0f };
        textFill_->Draw(commandList, orthoProjection);
    }

    for (int i = (int)dots_.size() - 1; i >= 0; --i) {
        if (dots_[i]) {
            dots_[i]->transform.translate.x += shakeOffset.x;
            dots_[i]->transform.translate.y += shakeOffset.y;
            dots_[i]->Draw(commandList, orthoProjection);
            dots_[i]->transform.translate.x -= shakeOffset.x;
            dots_[i]->transform.translate.y -= shakeOffset.y;
        }
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

void LoadingScreen::DrawDynamicBatch(
    ID3D12GraphicsCommandList* commandList,
    const Matrix4x4& orthoProjection,
    const std::vector<VertexData>& vertices,
    VertexData* mappedVertices,
    const D3D12_VERTEX_BUFFER_VIEW& vbView,
    ID3D12Resource* matRes,
    Material* matData,
    ID3D12Resource* wvpRes,
    TransformationMatrix* wvpData,
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle,
    const Vector4& color) {
    if (vertices.empty() || !mappedVertices || !matRes || !matData || !wvpRes || !wvpData) return;

    std::memcpy(mappedVertices, vertices.data(), sizeof(VertexData) * vertices.size());
    matData->color = color;
    matData->enableLighting = 0;
    matData->uvTransform = MakeIdentity4x4();

    wvpData->World = MakeIdentity4x4();
    wvpData->WVP = orthoProjection;
    wvpData->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &vbView);
    commandList->SetGraphicsRootConstantBufferView(0, matRes->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(1, wvpRes->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(2, srvHandle);
    commandList->DrawInstanced(static_cast<UINT>(vertices.size()), 1, 0, 0);
}

void LoadingScreen::DrawSlashFighter3D(
    ID3D12GraphicsCommandList* commandList,
    float screenX, float screenY,
    float dirScreenX, float dirScreenY,
    float progress01) {
    if (!slashFighterModel_ || !slashFighterTransformRes_) return;

    DirectXCommon* dxCommon = DirectXCommon::GetInstance();
    GraphicsPipeline* pipeline = GraphicsPipeline::GetInstance();
    if (!pipeline || !pipeline->GetObject3dPipelineState() || !pipeline->GetObject3dRootSignature()) return;

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

    // 任意の2D画面進行ベクトル (dirScreenX, dirScreenY) を3Dワールド空間の飛行ベクトルへ変換
    Vector3 flightDir = Normalize({ dirScreenX, -dirScreenY, 0.0f });
    if (std::abs(flightDir.x) < 0.0001f && std::abs(flightDir.y) < 0.0001f) {
        flightDir = { 1.0f, 0.0f, 0.0f };
    }

    Vector3 basisUp = { 0.0f, 0.0f, -1.0f };
    Vector3 basisRight = Normalize(Cross(basisUp, flightDir));
    basisUp = Normalize(Cross(flightDir, basisRight));

    Matrix4x4 R_basis = MakeIdentity4x4();
    R_basis.m[0][0] = basisRight.x; R_basis.m[0][1] = basisRight.y; R_basis.m[0][2] = basisRight.z;
    R_basis.m[1][0] = basisUp.x;    R_basis.m[1][1] = basisUp.y;    R_basis.m[1][2] = basisUp.z;
    R_basis.m[2][0] = flightDir.x;  R_basis.m[2][1] = flightDir.y;  R_basis.m[2][2] = flightDir.z;

    float rollSign = (dirScreenX >= 0.0f) ? 1.0f : -1.0f;
    float rollAngle = rollSign * 0.55f + progress01 * 1.15f * rollSign;
    Matrix4x4 rotY180  = MakeRotateYMatrix(std::numbers::pi_v<float>);
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
    if (slashFighterCameraData_) {
        slashFighterCameraData_->worldPosition = { 0.0f, 0.0f, camZ };
    }

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

void LoadingScreen::DrawDiveAndOverlapFighter3D(
    ID3D12GraphicsCommandList* commandList,
    float diveProgress01,
    bool isShatterPhase,
    float shatterProgress01) {
    if (!slashFighterModel_ || !slashFighterTransformRes_) return;

    // すでにゲーム内プレイヤーモデルと完全に重なり終えた後（shatterProgress01 >= 0.72f）はゲーム内モデル単体に任せてZファイティングを防止
    if (isShatterPhase && shatterProgress01 >= 0.72f) {
        return;
    }

    DirectXCommon* dxCommon = DirectXCommon::GetInstance();
    GraphicsPipeline* pipeline = GraphicsPipeline::GetInstance();
    if (!pipeline || !pipeline->GetObject3dPipelineState() || !pipeline->GetObject3dRootSignature()) return;

    Matrix4x4 targetFighterWorld = MakeIdentity4x4();
    Matrix4x4 targetViewProj     = MakeIdentity4x4();
    Vector3   camPos             = { 0.0f, 6.0f, -65.0f };
    Vector3   camForward         = { 0.0f, 0.0f, 1.0f };
    Vector3   camUp              = { 0.0f, 1.0f, 0.0f };
    Vector3   targetFighterPos   = { 0.0f, -3.0f, 0.0f };

    BaseScene* nextScene = SceneManager::GetInstance()->GetNextScene();
    bool hasTarget = nextScene && nextScene->GetTransitionPlayerTarget(
        targetFighterWorld, targetViewProj, camPos, camForward, camUp, targetFighterPos);

    if (!hasTarget) {
        const float fovY = 0.45f;
        const float aspect = (float)WinApp::kClientWidth / (float)WinApp::kClientHeight;
        Matrix4x4 viewMat = Inverse(MakeTranslateMatrix(camPos));
        Matrix4x4 projMat = MakePerspectiveFovMatrix(fovY, aspect, 0.1f, 1000.0f);
        targetViewProj = Multiply(viewMat, projMat);
        targetFighterWorld = Multiply(
            Multiply(Matrix4x4MakeScaleMatrix({ 10.0f, 10.0f, 10.0f }), MakeRotateYMatrix(std::numbers::pi_v<float>)),
            MakeTranslateMatrix(targetFighterPos)
        );
    }

    // カメラ手前（正面ドアップ）の突入開始位置と、ガラス面を突き破るインパクト位置
    Vector3 startDivePos = {
        camPos.x + camForward.x * 16.0f + camUp.x * 1.2f,
        camPos.y + camForward.y * 16.0f + camUp.y * 1.2f,
        camPos.z + camForward.z * 16.0f + camUp.z * 1.2f
    };
    Vector3 impactPos = {
        std::lerp(startDivePos.x, targetFighterPos.x, 0.44f),
        std::lerp(startDivePos.y, targetFighterPos.y, 0.44f),
        std::lerp(startDivePos.z, targetFighterPos.z, 0.44f)
    };

    Vector3 currentPos{};
    float rollAngle = 0.0f;
    float pitchOffset = 0.0f;

    if (!isShatterPhase) {
        // フェーズ4B後半: カメラ正面手前からひび割れたガラス中心へ向かってバレルロールしながら急突入
        float t = std::clamp(diveProgress01, 0.0f, 1.0f);
        float accelT = t * t * (0.35f + 0.65f * t);
        currentPos = {
            std::lerp(startDivePos.x, impactPos.x, accelT),
            std::lerp(startDivePos.y, impactPos.y, accelT),
            std::lerp(startDivePos.z, impactPos.z, accelT)
        };
        rollAngle = (1.0f - t) * 2.0f * std::numbers::pi_v<float> + 0.65f;
        pitchOffset = (1.0f - t) * 0.14f;
    } else {
        // フェーズ5B: ガラスを突き破った瞬間から、ゲーム内のプレイヤーモデルの位置・姿勢へピタッと重なる
        float overlapT = std::clamp(shatterProgress01 / 0.72f, 0.0f, 1.0f);
        float easeOut = 1.0f - std::pow(1.0f - overlapT, 3.0f);
        currentPos = {
            std::lerp(impactPos.x, targetFighterPos.x, easeOut),
            std::lerp(impactPos.y, targetFighterPos.y, easeOut),
            std::lerp(impactPos.z, targetFighterPos.z, easeOut)
        };
        rollAngle = (1.0f - easeOut) * 0.65f;
        pitchOffset = 0.0f;
    }

    // targetFighterWorld の回転・スケールをベースにしつつ、突入ロールと位置を適用（overlap完了時にtargetFighterWorldと100%一致）
    Matrix4x4 extraRot = Multiply(MakeRotateZMatrix(rollAngle), MakeRotateXMatrix(pitchOffset));
    Matrix4x4 baseNoTrans = targetFighterWorld;
    baseNoTrans.m[3][0] = 0.0f;
    baseNoTrans.m[3][1] = 0.0f;
    baseNoTrans.m[3][2] = 0.0f;

    Matrix4x4 worldMat = Multiply(Multiply(extraRot, baseNoTrans), MakeTranslateMatrix(currentPos));

    dxCommon->TransitionDepthStencilState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = dxCommon->GetCurrentBackBufferRtvHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dxCommon->GetDsvHandle();
    commandList->OMSetRenderTargets(1, &backBufferRtv, false, &dsvHandle);
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    slashFighterTransformData_->World = worldMat;
    slashFighterTransformData_->WVP   = Multiply(worldMat, targetViewProj);
    slashFighterTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (slashFighterCameraData_) {
        slashFighterCameraData_->worldPosition = camPos;
    }

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
    D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = TextureManager::GetInstance()->GetSrvHandleGPU("Title/white1x1.png");

    auto bindSpritePipeline = [&]() {
        ID3D12DescriptorHeap* heaps[] = { TextureManager::GetInstance()->GetSrvHeap() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetPipelineState(pipeline->GetSpritePipelineState());
        commandList->SetGraphicsRootSignature(pipeline->GetSpriteRootSignature());
    };

    bindSpritePipeline();

    if (phase_ == TransitionPhase::kSlashTitle) {
        if (!hasCapturedTitle_) {
            CaptureCurrentBackBuffer(commandList);
            hasCapturedTitle_ = true;
        } else if (capturedFullSprite_) {
            capturedFullSprite_->transform.translate = { 0.0f, 0.0f, 0.0f };
            capturedFullSprite_->transform.scale = { W, H, 1.0f };
            capturedFullSprite_->DrawCustomSrv(commandList, orthoProjection, capturedSrvHandleGPU_, 6, 0);
        }

        float t = std::clamp(phaseTimer_ / kSlashTitleDuration, 0.0f, 1.0f);
        float easedT = 0.25f * t + 0.75f * t * t;
        float fx = std::lerp(-160.0f, W + 160.0f, easedT);
        float fy = std::lerp(-90.0f,  H + 90.0f,  easedT);

        float lineEndX = std::clamp(fx, 0.0f, W);
        float lineEndY = std::clamp(fy, 0.0f, H);
        if (fx > 0.0f) {
            DrawLineSprite(slashGlowOuterSprite_.get(), commandList, orthoProjection, 0.0f, 0.0f, lineEndX, lineEndY, 26.0f, { 0.10f, 0.75f, 1.0f, 0.45f });
            DrawLineSprite(slashGlowMidSprite_.get(),   commandList, orthoProjection, 0.0f, 0.0f, lineEndX, lineEndY, 11.0f, { 0.45f, 0.95f, 1.0f, 0.85f });
            DrawLineSprite(slashCoreSprite_.get(),      commandList, orthoProjection, 0.0f, 0.0f, lineEndX, lineEndY, 4.0f,  { 1.0f,  1.0f,  1.0f, 1.0f  });
        }

        if (t > 0.82f && slashFlashSprite_) {
            float flashAlpha = std::sin(((t - 0.82f) / 0.18f) * std::numbers::pi_v<float>) * 0.45f;
            slashFlashSprite_->SetColor({ 0.85f, 0.98f, 1.0f, flashAlpha });
            slashFlashSprite_->Draw(commandList, orthoProjection);
        }

        DrawSlashFighter3D(commandList, fx, fy, W, H, t);
    }
    else if (phase_ == TransitionPhase::kSplitTitleUpDown) {
        DrawLoadingBase(commandList, orthoProjection);

        float t = std::clamp(phaseTimer_ / kSplitTitleDuration, 0.0f, 1.0f);
        float eased = t * t * (2.2f - 1.2f * t);
        float splitOffsetY = eased * (H + 40.0f);

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

        float edgeAlpha = std::clamp(1.0f - t * 0.7f, 0.0f, 1.0f);
        DrawLineSprite(slashEdgeSpriteA_.get(), commandList, orthoProjection, 0.0f, -splitOffsetY, W, H - splitOffsetY, 10.0f, { 0.2f, 0.85f, 1.0f, 0.75f * edgeAlpha });
        DrawLineSprite(slashEdgeCoreA_.get(),   commandList, orthoProjection, 0.0f, -splitOffsetY, W, H - splitOffsetY, 3.5f,  { 1.0f, 1.0f,  1.0f, 1.0f  * edgeAlpha });
        DrawLineSprite(slashEdgeSpriteB_.get(), commandList, orthoProjection, 0.0f, +splitOffsetY, W, H + splitOffsetY, 10.0f, { 0.2f, 0.85f, 1.0f, 0.75f * edgeAlpha });
        DrawLineSprite(slashEdgeCoreB_.get(),   commandList, orthoProjection, 0.0f, +splitOffsetY, W, H + splitOffsetY, 3.5f,  { 1.0f, 1.0f,  1.0f, 1.0f  * edgeAlpha });
    }
    else if (phase_ == TransitionPhase::kLoadingProgress) {
        DrawLoadingBase(commandList, orthoProjection);

        if (targetProgress_ >= 0.99f && currentProgress_ >= 0.99f && phaseTimer_ >= 0.30f && !hasCapturedLoading_) {
            CaptureCurrentBackBuffer(commandList);
            hasCapturedLoading_ = true;
        }
    }
    else if (phase_ == TransitionPhase::kSlashLoadingReverse) {
        DrawLoadingBase(commandList, orthoProjection);
        if (!hasCapturedLoading_) {
            CaptureCurrentBackBuffer(commandList);
            hasCapturedLoading_ = true;
        }

        float t = std::clamp(phaseTimer_ / kSlashLoadingDuration, 0.0f, 1.0f);
        float easedT = 0.25f * t + 0.75f * t * t;
        float fx = std::lerp(W + 160.0f, -160.0f, easedT);
        float fy = std::lerp(H + 90.0f,  -90.0f,  easedT);

        float lineEndX = std::clamp(fx, 0.0f, W);
        float lineEndY = std::clamp(fy, 0.0f, H);
        if (fx < W) {
            DrawLineSprite(slashGlowOuterSprite_.get(), commandList, orthoProjection, W, H, lineEndX, lineEndY, 26.0f, { 0.10f, 0.75f, 1.0f, 0.45f });
            DrawLineSprite(slashGlowMidSprite_.get(),   commandList, orthoProjection, W, H, lineEndX, lineEndY, 11.0f, { 0.45f, 0.95f, 1.0f, 0.85f });
            DrawLineSprite(slashCoreSprite_.get(),      commandList, orthoProjection, W, H, lineEndX, lineEndY, 4.0f,  { 1.0f,  1.0f,  1.0f, 1.0f  });
        }

        if (t > 0.82f && slashFlashSprite_) {
            float flashAlpha = std::sin(((t - 0.82f) / 0.18f) * std::numbers::pi_v<float>) * 0.45f;
            slashFlashSprite_->SetColor({ 0.85f, 0.98f, 1.0f, flashAlpha });
            slashFlashSprite_->Draw(commandList, orthoProjection);
        }

        DrawSlashFighter3D(commandList, fx, fy, -W, -H, t);
    }
    else if (phase_ == TransitionPhase::kSplitLoadingLeftRight) {
        float t = std::clamp(phaseTimer_ / kSplitLoadingDuration, 0.0f, 1.0f);
        float eased = t * t * (2.2f - 1.2f * t);
        float splitOffsetX = eased * (W + 40.0f);

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

        float edgeAlpha = std::clamp(1.0f - t * 0.7f, 0.0f, 1.0f);
        DrawLineSprite(slashEdgeSpriteA_.get(), commandList, orthoProjection, -splitOffsetX, 0.0f, W - splitOffsetX, H, 10.0f, { 0.2f, 0.85f, 1.0f, 0.75f * edgeAlpha });
        DrawLineSprite(slashEdgeCoreA_.get(),   commandList, orthoProjection, -splitOffsetX, 0.0f, W - splitOffsetX, H, 3.5f,  { 1.0f, 1.0f,  1.0f, 1.0f  * edgeAlpha });
        DrawLineSprite(slashEdgeSpriteB_.get(), commandList, orthoProjection, +splitOffsetX, 0.0f, W + splitOffsetX, H, 10.0f, { 0.2f, 0.85f, 1.0f, 0.75f * edgeAlpha });
        DrawLineSprite(slashEdgeCoreB_.get(),   commandList, orthoProjection, +splitOffsetX, 0.0f, W + splitOffsetX, H, 3.5f,  { 1.0f, 1.0f,  1.0f, 1.0f  * edgeAlpha });
    }
    else if (phase_ == TransitionPhase::kMultiSlashLoading) {
        // [パターン1] 色んな方向から5回連続で斬りつけて切れ目を刻み、最後に正面から突っ込む
        bool isDiveSubPhase = (phaseTimer_ >= kMultiSlashPhaseTime);
        int activeSlashIdx = std::clamp(static_cast<int>(phaseTimer_ / kSingleSlashTime), 0, kNumMultiSlashes - 1);
        float subSlashT = std::clamp((phaseTimer_ - (float)activeSlashIdx * kSingleSlashTime) / kSingleSlashTime, 0.0f, 1.0f);

        // 斬撃ごとの衝撃揺れ
        Vector2 shake = { 0.0f, 0.0f };
        if (!isDiveSubPhase && subSlashT > 0.2f && subSlashT < 0.85f) {
            float shakeAmp = 8.0f * (1.0f - subSlashT);
            shake.x = std::sin(phaseTimer_ * 140.0f) * shakeAmp;
            shake.y = std::cos(phaseTimer_ * 165.0f) * shakeAmp;
        }

        // 1. ロード画面ベースの描画
        DrawLoadingBase(commandList, orthoProjection, shake);

        // 2. これまでに刻まれた切れ目＋ガラスのひび割れをバッチ構築
        std::vector<VertexData> glowVerts;
        std::vector<VertexData> coreVerts;
        glowVerts.reserve(768);
        coreVerts.reserve(768);

        int completedCount = isDiveSubPhase ? kNumMultiSlashes : activeSlashIdx;
        for (int i = 0; i < completedCount; ++i) {
            const auto& sd = kMultiSlashDefs[i];
            AppendSlashWithBranchCracks(glowVerts, coreVerts, sd.x0 + shake.x, sd.y0 + shake.y, sd.x1 + shake.x, sd.y1 + shake.y, i, kMaxBatchVertices);
        }

        // 現在進行中の斬撃ライン
        float curFx = 0.0f;
        float curFy = 0.0f;
        float curDirX = 1.0f;
        float curDirY = 0.0f;
        if (!isDiveSubPhase) {
            const auto& sd = kMultiSlashDefs[activeSlashIdx];
            float eased = 0.2f * subSlashT + 0.8f * subSlashT * subSlashT;
            curFx = std::lerp(sd.x0, sd.x1, eased);
            curFy = std::lerp(sd.y0, sd.y1, eased);
            curDirX = sd.x1 - sd.x0;
            curDirY = sd.y1 - sd.y0;
            AppendSlashWithBranchCracks(glowVerts, coreVerts, sd.x0 + shake.x, sd.y0 + shake.y, curFx + shake.x, curFy + shake.y, activeSlashIdx, kMaxBatchVertices);
        } else {
            // 正面突入チャージ中：中央 (640, 400) の衝突点周辺にも蜘蛛の巣状のガラス亀裂が浮き出る
            float diveT = std::clamp((phaseTimer_ - kMultiSlashPhaseTime) / kFrontDiveTime, 0.0f, 1.0f);
            const float cx = 640.0f;
            const float cy = 400.0f;
            int numWebRays = 10;
            for (int r = 0; r < numWebRays; ++r) {
                float ang = (float)r * (2.0f * std::numbers::pi_v<float> / (float)numWebRays) + 0.25f;
                float rayLen = (65.0f + (float)((r * 3) % 4) * 35.0f) * (std::min)(1.0f, diveT * 1.6f);
                float rx = cx + std::cos(ang) * rayLen;
                float ry = cy + std::sin(ang) * rayLen;
                AppendTaperedLineVertices(glowVerts, cx, cy, rx, ry, 10.0f, 2.0f, kMaxBatchVertices);
                AppendTaperedLineVertices(coreVerts, cx, cy, rx, ry,  3.0f, 0.8f, kMaxBatchVertices);
            }
        }

        // 切れ目・ガラス亀裂の一括描画
        DrawDynamicBatch(commandList, orthoProjection, glowVerts, slashGlowBatchMapped_, slashGlowBatchVbView_, slashGlowBatchMatRes_.Get(), slashGlowBatchMatData_, slashGlowBatchWvpRes_.Get(), slashGlowBatchWvpData_, whiteSrv, { 0.15f, 0.85f, 1.0f, 0.72f });
        DrawDynamicBatch(commandList, orthoProjection, coreVerts, slashCoreBatchMapped_, slashCoreBatchVbView_, slashCoreBatchMatRes_.Get(), slashCoreBatchMatData_, slashCoreBatchWvpRes_.Get(), slashCoreBatchWvpData_, whiteSrv, { 1.0f,  1.0f,  1.0f, 0.98f });

        if (!isDiveSubPhase) {
            // 5回の多方向横断中：各斬撃の先端を3Dプレイヤー機が高速飛行
            DrawSlashFighter3D(commandList, curFx, curFy, curDirX, curDirY, subSlashT);
        } else {
            // 5本の切れ目が入った直後、3D機体を描く前に「切れ目入りのロード画面」をガラス破片テクスチャとしてキャプチャ！
            float diveT = std::clamp((phaseTimer_ - kMultiSlashPhaseTime) / kFrontDiveTime, 0.0f, 1.0f);
            if (diveT >= 0.55f && !hasCapturedCrackedLoading_) {
                CaptureCurrentBackBuffer(commandList);
                hasCapturedCrackedLoading_ = true;
            }

            // インパクト直前のホワイトフラッシュ予兆
            if (diveT > 0.78f && slashFlashSprite_) {
                float flashAlpha = ((diveT - 0.78f) / 0.22f) * 0.55f;
                slashFlashSprite_->SetColor({ 0.88f, 0.98f, 1.0f, flashAlpha });
                slashFlashSprite_->Draw(commandList, orthoProjection);
            }

            // 正面（カメラ手前）からひび割れたロード画面の中央へ向かって3Dプレイヤー機が突っ込む！
            DrawDiveAndOverlapFighter3D(commandList, diveT, false, 0.0f);
        }
    }
    else if (phase_ == TransitionPhase::kGlassShatterAndOverlap) {
        // ※下層には SceneManager::Draw() で既に nextScene_ (GamePlayScene) が描画されている
        float shatterT = std::clamp(phaseTimer_ / kGlassShatterDuration, 0.0f, 1.0f);

        // 1. ガラスを突き破って突入したプレイヤー機が、奥のゲーム内プレイヤーモデルにピタッと重なる3D描画
        DrawDiveAndOverlapFighter3D(commandList, 1.0f, true, shatterT);

        // 2. スプライトパイプラインに戻して、112枚のガラス破片が3D回転しながら四方八方へ飛び散る演出を描画
        bindSpritePipeline();

        std::vector<VertexData> texShardVerts;
        std::vector<VertexData> glintShardVerts;
        texShardVerts.reserve(glassShards_.size() * 3);
        glintShardVerts.reserve(glassShards_.size() * 3);

        for (size_t i = 0; i < glassShards_.size(); ++i) {
            const auto& shard = glassShards_[i];
            float st = (std::max)(0.0f, phaseTimer_ - shard.delay);

            // 破片の現在重心位置（放射状爆発 ＋ 重力落下）
            float curCx = shard.center.x + shard.vel.x * st;
            float curCy = shard.center.y + shard.vel.y * st + 0.5f * 920.0f * st * st;

            // 画面外へ十分出た破片はスキップ
            if (curCx < -260.0f || curCx > W + 260.0f || curCy < -260.0f || curCy > H + 320.0f) {
                continue;
            }

            float rx = shard.rotXSpeed * st;
            float ry = shard.rotYSpeed * st;
            float rz = shard.rotZSpeed * st;
            float cosZ = std::cos(rz);
            float sinZ = std::sin(rz);
            float cosX = std::cos(rx);
            float cosY = std::cos(ry);
            float scale = 1.0f + shard.scaleSpeed * st;

            VertexData triVerts[3]{};
            for (int k = 0; k < 3; ++k) {
                float lx = (shard.basePos[k].x - shard.center.x) * scale;
                float ly = (shard.basePos[k].y - shard.center.y) * scale;

                float rx2 = lx * cosZ - ly * sinZ;
                float ry2 = lx * sinZ + ly * cosZ;

                // 3Dピッチ・ヨー回転による投影伸縮で立体的にヒラヒラ舞うガラス破片を表現
                float px = curCx + rx2 * cosY;
                float py = curCy + ry2 * cosX;

                triVerts[k].position = { px, py, 0.0f, 1.0f };
                triVerts[k].texcoord = shard.uv[k];
                triVerts[k].normal   = { 0.0f, 0.0f, -1.0f };
                texShardVerts.push_back(triVerts[k]);
            }

            // 破片が3D回転して光を反射した瞬間のガラスのキラッとしたハイライト
            float glintVal = std::abs(std::sin(rx * 1.5f + ry * 1.2f));
            if (glintVal > 0.72f) {
                for (int k = 0; k < 3; ++k) {
                    glintShardVerts.push_back(triVerts[k]);
                }
            }
        }

        float shardAlpha = std::clamp(1.0f - (shatterT - 0.75f) / 0.25f, 0.0f, 1.0f);
        DrawDynamicBatch(commandList, orthoProjection, texShardVerts,   shatterTexMapped_,   shatterTexVbView_,   shatterTexMatRes_.Get(),   shatterTexMatData_,   shatterTexWvpRes_.Get(),   shatterTexWvpData_,   capturedSrvHandleGPU_, { 1.0f, 1.0f, 1.0f, shardAlpha });
        DrawDynamicBatch(commandList, orthoProjection, glintShardVerts, shatterGlintMapped_, shatterGlintVbView_, shatterGlintMatRes_.Get(), shatterGlintMatData_, shatterGlintWvpRes_.Get(), shatterGlintWvpData_, whiteSrv,              { 0.70f, 0.96f, 1.0f, 0.42f * shardAlpha });

        // ガラスが割れた瞬間のインパクトフラッシュ
        if (shatterT < 0.18f && slashFlashSprite_) {
            float flashAlpha = (1.0f - (shatterT / 0.18f)) * 0.65f;
            slashFlashSprite_->SetColor({ 0.90f, 0.98f, 1.0f, flashAlpha });
            slashFlashSprite_->Draw(commandList, orthoProjection);
        }
    }
}


