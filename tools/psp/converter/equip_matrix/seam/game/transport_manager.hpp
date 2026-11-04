#pragma once
// PC seam - upstream's TransportManager is never reached by
// the equipment code (GameHandler::getTransportManager answers nullptr here).
namespace wowee {
namespace game {

class TransportManager {
public:
    template <typename... A> bool hasPathForEntry(A&&...) const { return false; }
    template <typename... A> bool hasUsableMovingPathForEntry(A&&...) const { return false; }
    template <typename... A> bool hasTaxiPathForMap(A&&...) const { return false; }
    template <typename... A> bool isClientSideAnimation(A&&...) const { return false; }
    template <typename... A> void* getTransport(A&&...) const { return nullptr; }
    template <typename... A> void registerTransport(A&&...) {}
    template <typename... A> void updateServerTransport(A&&...) {}
    template <typename... A> void rebindTransportInstance(A&&...) {}
    template <typename... A> void loadPathFromNodes(A&&...) {}
    template <typename... A> bool assignTaxiPathToTransport(A&&...) { return false; }
    template <typename... A> unsigned inferDbcPathForSpawn(A&&...) const { return 0; }
    template <typename... A> unsigned pickFallbackMovingPath(A&&...) const { return 0; }
};

}  // namespace game
}  // namespace wowee
