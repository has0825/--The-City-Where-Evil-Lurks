#pragma once
#include "MathTypes.h"

namespace Engine {

    /// <summary>
    /// ゲーム空間に存在するすべてのオブジェクトの共通抽象基底クラス
    /// [Template Method Pattern] と [Dirty Flag Pattern] を適用
    /// </summary>
    class GameObject {
    public:
        GameObject() = default;
        virtual ~GameObject() = default; // 必須の仮想デストラクタ (メモリリーク防止)

        /// <summary>
        /// [Template Method Pattern]
        /// 全ゲームオブジェクト共通の更新フロー骨組み
        /// 共通事前処理・共通事後処理を行い、具体的な処理は OnUpdate に委譲
        /// </summary>
        /// <param name="deltaTime">フレーム経過時間</param>
        void Update(float deltaTime) {
            if (!isAlive_) return;

            // 1. 各派生クラス固有の更新ロジックを呼び出し (Template Method)
            OnUpdate(deltaTime);

            // 2. [Dirty Flag Pattern] の更新処理
            if (isMatrixDirty_) {
                UpdateWorldMatrix();
                isMatrixDirty_ = false;
            }
        }

        // --- [Dirty Flag Pattern] 座標・回転・スケールの変更アクセサ ---
        const Vector3& GetPosition() const { return position_; }
        void SetPosition(const Vector3& pos) {
            position_ = pos;
            isMatrixDirty_ = true; // 行列再計算フラグをON
        }

        const Vector3& GetScale() const { return scale_; }
        void SetScale(const Vector3& scale) {
            scale_ = scale;
            isMatrixDirty_ = true;
        }

        const Vector3& GetRotate() const { return rotate_; }
        void SetRotate(const Vector3& rot) {
            rotate_ = rot;
            isMatrixDirty_ = true;
        }

        bool IsAlive() const { return isAlive_; }
        void SetIsAlive(bool alive) { isAlive_ = alive; }

        bool IsMatrixDirty() const { return isMatrixDirty_; }

    protected:
        /// <summary>
        /// 派生クラス固有の更新ステップ (Template Method Pattern)
        /// </summary>
        virtual void OnUpdate(float deltaTime) = 0;

        /// <summary>
        /// Dirty FlagがONの場合のみ呼び出される行列計算ステップ
        /// </summary>
        virtual void UpdateWorldMatrix() {}

        Vector3 position_ = { 0.0f, 0.0f, 0.0f };
        Vector3 scale_ = { 1.0f, 1.0f, 1.0f };
        Vector3 rotate_ = { 0.0f, 0.0f, 0.0f };
        bool isAlive_ = true;
        bool isMatrixDirty_ = true; // [Dirty Flag Pattern] 用フラグ
    };

}
