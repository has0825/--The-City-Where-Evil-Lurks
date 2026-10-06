#include "Bullet.h"

namespace Engine {

    void Bullet::Initialize(const Vector3& position, const Vector3& velocity, float radius, int type) {
        position_ = position;
        velocity_ = velocity;
        radius_ = radius;
        type_ = type;
        currentTime_ = 0.0f;
        isAlive_ = true;
    }

    void Bullet::OnUpdate(float deltaTime) {
        position_.x += velocity_.x * deltaTime;
        position_.y += velocity_.y * deltaTime;
        position_.z += velocity_.z * deltaTime;

        currentTime_ += deltaTime;

        // 5秒経過したら消滅
        if (currentTime_ >= 5.0f) {
            isAlive_ = false;
        }
    }

}
