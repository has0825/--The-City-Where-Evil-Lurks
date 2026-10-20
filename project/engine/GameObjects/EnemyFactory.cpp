#include "EnemyFactory.h"

namespace Engine {

    Enemy EnemyFactory::CreateEnemy(const Vector3& position, float hp, float radius, int groupIdx, int memberIdx) {
        Enemy enemy;
        enemy.Initialize(position, hp, radius);
        enemy.SetGroupIndex(groupIdx);
        enemy.SetMemberIndex(memberIdx);
        return enemy;
    }

}
