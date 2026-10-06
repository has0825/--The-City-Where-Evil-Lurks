#include "BulletFactory.h"

namespace Engine {

    Bullet BulletFactory::CreateBullet(const Vector3& position, const Vector3& velocity, float radius, int type) {
        Bullet bullet;
        bullet.Initialize(position, velocity, radius, type);
        return bullet;
    }

}
