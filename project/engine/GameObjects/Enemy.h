#pragma once
#include "GameObject.h"
#include "EnemyState.h"
#include <memory>

namespace Engine {

    struct BoxCollider {
        bool hasCollider = false;
        Vector3 center = { 0.0f, 0.0f, 0.0f }; // ローカル座標での中心オフセット
        Vector3 size = { 2.0f, 2.0f, 2.0f };   // コライダーサイズ
    };

    /// <summary>
    /// 雑魚エネミークラス (GameObjectを継承したポリモーフィズム構造 & State Pattern適用)
    /// </summary>
    class Enemy : public GameObject {
    public:
        enum class StateType {
            kSideWait,
            kAppear,
            kWander,
            kDive
        };

        Enemy() = default;
        ~Enemy() override = default;

        // std::vector等のコンテナ用コピー・ムーブコンストラクタ
        Enemy(const Enemy& other);
        Enemy& operator=(const Enemy& other);
        Enemy(Enemy&& other) noexcept = default;
        Enemy& operator=(Enemy&& other) noexcept = default;

        void Initialize(const Vector3& position, float hp = 10.0f, float radius = 1.5f);
        
        /// <summary>
        /// 毎フレームの更新ロジック (Template Method Pattern)
        /// </summary>
        void OnUpdate(float deltaTime) override;

        /// <summary>
        /// 状態変更ロジック (State Pattern)
        /// </summary>
        void ChangeState(std::unique_ptr<IEnemyState> newState);

        void TakeDamage(float damage);

        // --- カプセル化された Getter / Setter ---
        float GetRadius() const { return radius_; }
        void SetRadius(float r) { radius_ = r; }

        float GetHP() const { return hp_; }
        void SetHP(float hp) { hp_ = hp; }

        float GetMaxHP() const { return maxHP_; }
        void SetMaxHP(float maxHp) { maxHP_ = maxHp; }

        const BoxCollider& GetCollider() const { return collider_; }
        void SetCollider(const BoxCollider& collider) { collider_ = collider; }

        int GetGroupIndex() const { return groupIndex_; }
        void SetGroupIndex(int idx) { groupIndex_ = idx; }

        int GetMemberIndex() const { return memberIndex_; }
        void SetMemberIndex(int idx) { memberIndex_ = idx; }

        const Vector3& GetLocalOffset() const { return localOffset_; }
        void SetLocalOffset(const Vector3& offset) { localOffset_ = offset; }

        StateType GetStateType() const { return currentStateType_; }
        void SetStateType(StateType st) { currentStateType_ = st; }

        float GetStateTimer() const { return stateTimer_; }
        void SetStateTimer(float timer) { stateTimer_ = timer; }
        void AddStateTimer(float delta) { stateTimer_ += delta; }

        const Vector3& GetWanderAnchor() const { return wanderAnchor_; }
        void SetWanderAnchor(const Vector3& anchor) { wanderAnchor_ = anchor; }

        const Vector3& GetDiveDirection() const { return diveDirection_; }
        void SetDiveDirection(const Vector3& dir) { diveDirection_ = dir; }

        float GetWanderPhase() const { return wanderPhase_; }
        void SetWanderPhase(float phase) { wanderPhase_ = phase; }
        void AddWanderPhase(float delta) { wanderPhase_ += delta; }

        float GetSpeed() const { return speed_; }
        void SetSpeed(float s) { speed_ = s; }

        float GetRelativeZ() const { return relativeZ_; }
        void SetRelativeZ(float rz) { relativeZ_ = rz; }

        const Vector3& GetAppearStartPos() const { return appearStartPos_; }
        void SetAppearStartPos(const Vector3& pos) { appearStartPos_ = pos; }

        float GetRailProgress() const { return railProgress_; }
        void SetRailProgress(float rp) { railProgress_ = rp; }

        float GetWanderAnchorRailProgress() const { return wanderAnchorRailProgress_; }
        void SetWanderAnchorRailProgress(float rp) { wanderAnchorRailProgress_ = rp; }

        float GetWanderAnchorRelX() const { return wanderAnchorRelX_; }
        void SetWanderAnchorRelX(float rx) { wanderAnchorRelX_ = rx; }

        float GetWanderAnchorRelY() const { return wanderAnchorRelY_; }
        void SetWanderAnchorRelY(float ry) { wanderAnchorRelY_ = ry; }

    private:
        float radius_ = 0.0f;
        float hp_ = 0.0f;
        float maxHP_ = 0.0f;
        BoxCollider collider_;

        // State Pattern用メンバ
        std::unique_ptr<IEnemyState> currentState_;
        StateType currentStateType_ = StateType::kSideWait;

        // フォーメーション管理用
        int groupIndex_ = 0;
        int memberIndex_ = 0;
        Vector3 localOffset_ = { 0.0f, 0.0f, 0.0f };

        // 自律移動・特攻用パラメータ
        float stateTimer_ = 0.0f;
        Vector3 wanderAnchor_ = { 0.0f, 0.0f, 0.0f };
        Vector3 diveDirection_ = { 0.0f, 0.0f, 0.0f };
        float wanderPhase_ = 0.0f;
        float speed_ = 0.0f;
        float relativeZ_ = 120.0f;
        Vector3 appearStartPos_ = { 0.0f, 0.0f, 0.0f };

        float railProgress_ = 0.0f;
        float wanderAnchorRailProgress_ = 0.0f;
        float wanderAnchorRelX_ = 0.0f;
        float wanderAnchorRelY_ = 0.0f;
    };
}
