#include "rendering/psp/psp_collision_math.hpp"
#include <cassert>
#include <cmath>
int main(){using namespace wowee::rendering::psp;const glm::vec3 d(0,0,-1);
 const glm::vec3 a(0,0,10),b(10,0,10),c(0,10,10);
 assert(collisionRayTri({2,2,11},d,a,b,c)==1);
 assert(collisionRayTri({-.03f,2,11},d,a,b,c)<0); // narrow baked seam
 assert(std::fabs(collisionFloorRayTri({-.03f,2,11},d,a,b,c,.12f)-1)<.001f);
 assert(collisionFloorRayTri({-.2f,2,11},d,a,b,c,.12f)<0); // real ledge still falls
 assert(collisionFloorRayTri({-.03f,2,9},d,a,b,c,.12f)<0); // no snapping up through a floor
 const glm::vec3 slopeB(10,0,15),slopeC(0,10,10);
 const float hit=collisionFloorRayTri({-.03f,2,11},d,a,slopeB,slopeC,.12f);
 assert(hit>=0 && hit<=1.001f); // ramp edge support stays on the ramp
 assert(collisionFloorRayTri({-.03f,2,11},d,a,b,c,0)<0);
}
