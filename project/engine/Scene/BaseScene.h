#pragma once
#include "MathTypes.h"

// シーンの基底クラス
class BaseScene {
public:
    virtual ~BaseScene() = default;

    // 初期化
    virtual void Initialize() = 0;
    // 終了処理
    virtual void Finalize() = 0;
    // 更新
    virtual void Update() = 0;
    // 描画
    virtual void Draw() = 0;

    // 画面遷移演出でゲーム内プレイヤーモデルと重なるためのターゲット変換情報取得
    virtual bool GetTransitionPlayerTarget(
        Matrix4x4& outFighterWorld,
        Matrix4x4& outViewProj,
        Vector3& outCamPos,
        Vector3& outCamForward,
        Vector3& outCamUp,
        Vector3& outFighterPos) const {
        (void)outFighterWorld;
        (void)outViewProj;
        (void)outCamPos;
        (void)outCamForward;
        (void)outCamUp;
        (void)outFighterPos;
        return false;
    }
};