#pragma once
#include "GameObject.h"

namespace Engine {

    /// <summary>
    /// 弾オブジェクトクラス (GameObjectを継承したポリモーフィズム構造)
    /// </summary>
    class Bullet : public GameObject {
    public:
        Bullet() = default;
        ~Bullet() override = default;

        void Initialize(const Vector3& position, const Vector3& velocity, float radius = 0.5f, int type = 0);
        
        /// <summary>
        /// 毎フレームの更新ロジック (Template Method Pattern)
        /// </summary>
        void OnUpdate(float deltaTime) override;

        // --- カプセル化された Getter / Setter ---
        const Vector3& GetVelocity() const { return velocity_; }
        void SetVelocity(const Vector3& vel) { velocity_ = vel; }

        float GetRadius() const { return radius_; }
        void SetRadius(float r) { radius_ = r; }

        float GetCurrentTime() const { return currentTime_; }
        void SetCurrentTime(float t) { currentTime_ = t; }

        int GetType() const { return type_; }
        void SetType(int t) { type_ = t; }

    private:
        Vector3 velocity_ = { 0.0f, 0.0f, 0.0f };
        float radius_ = 0.5f;
        float currentTime_ = 0.0f;
        int type_ = 0; // 0: Normal, 1: SubWeapon, etc.
    };

}
