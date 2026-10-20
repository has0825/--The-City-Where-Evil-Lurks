#include "Building.h"

namespace Engine {

    void Building::Initialize(const Vector3& position, const Vector3& scale, int floors) {
        position_ = position;
        scale_ = scale;
        floors_ = floors;
        rotate_ = { 0.0f, 0.0f, 0.0f };
        isDestroyed_ = false;
        velocity_ = { 0.0f, 0.0f, 0.0f };
        rotationSpeed_ = { 0.0f, 0.0f, 0.0f };
        destroyTimer_ = 0.0f;
        isAlive_ = true;
        isWorldMatrixCached_ = false;

        // コライダーの設定
        collider_.hasCollider = true;
        collider_.center = { 0.0f, (float)floors * 2.0f, 0.0f };
        collider_.size = { scale.x * 2.0f, (float)floors * 4.0f, scale.z * 2.0f };
    }

    void Building::OnUpdate(float deltaTime) {
        // 破壊状態の場合の挙動更新
        if (isDestroyed_) {
            destroyTimer_ += deltaTime;
            position_.x += velocity_.x * deltaTime;
            position_.y += velocity_.y * deltaTime;
            position_.z += velocity_.z * deltaTime;

            rotate_.x += rotationSpeed_.x * deltaTime;
            rotate_.y += rotationSpeed_.y * deltaTime;
            rotate_.z += rotationSpeed_.z * deltaTime;

            // 1.5秒経過したら消滅
            if (destroyTimer_ >= 1.5f) {
                isAlive_ = false;
            }
        }
    }

    void Building::DestroyBuilding(const Vector3& initialVelocity, const Vector3& rotSpeed) {
        if (isDestroyed_) return;

        isDestroyed_ = true;
        velocity_ = initialVelocity;
        rotationSpeed_ = rotSpeed;
        destroyTimer_ = 0.0f;
    }

}
