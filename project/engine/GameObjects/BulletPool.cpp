#include "BulletPool.h"

namespace Engine {

    void BulletPool::Initialize(size_t capacity) {
        capacity_ = capacity;
        pool_.clear();
        pool_.resize(capacity_);
        for (auto& bullet : pool_) {
            bullet.SetIsAlive(false);
        }
    }

    Bullet* BulletPool::Acquire(const Vector3& position, const Vector3& velocity, float radius, int type) {
        // プール内から非アクティブ(isAlive == false)な弾を探して再利用 (Object Pool Pattern)
        for (auto& bullet : pool_) {
            if (!bullet.IsAlive()) {
                bullet.Initialize(position, velocity, radius, type);
                return &bullet;
            }
        }

        // 非アクティブなものが無い場合、プールを自動拡張
        pool_.emplace_back();
        auto& newBullet = pool_.back();
        newBullet.Initialize(position, velocity, radius, type);
        return &newBullet;
    }

    void BulletPool::Update(float deltaTime) {
        for (auto& bullet : pool_) {
            if (bullet.IsAlive()) {
                bullet.Update(deltaTime);
            }
        }
    }

}
