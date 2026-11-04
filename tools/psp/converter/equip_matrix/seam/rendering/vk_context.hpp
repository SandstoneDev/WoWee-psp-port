#pragma once
// PC seam - no Vulkan; upstream's upload batching is a no-op here.
namespace wowee {
namespace rendering {

class VkContext {
public:
    void beginUploadBatch() {}
    void endUploadBatch() {}
    void endUploadBatchSync() {}
};

}  // namespace rendering
}  // namespace wowee
