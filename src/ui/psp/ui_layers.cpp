// Layer manager (include/ui/psp/ui_layers.hpp).
#include "ui/psp/ui_layers.hpp"

#include <algorithm>

namespace wowee::ui::psp {

int UiLayers::add(const Client& c) {
    Client n = c;
    n.order = next_++;
    // Reuse a removed slot so handles stay small.
    for (size_t i = 0; i < alive_.size(); ++i) {
        if (!alive_[i]) {
            clients_[i] = n;
            alive_[i] = true;
            dirty_ = true;
            return static_cast<int>(i);
        }
    }
    clients_.push_back(n);
    alive_.push_back(true);
    dirty_ = true;
    return static_cast<int>(clients_.size() - 1);
}

void UiLayers::remove(int h) {
    if (h < 0 || static_cast<size_t>(h) >= alive_.size()) return;
    alive_[static_cast<size_t>(h)] = false;
    dirty_ = true;
}

void UiLayers::setVisible(int h, bool v) {
    if (h < 0 || static_cast<size_t>(h) >= clients_.size()) return;
    clients_[static_cast<size_t>(h)].visible = v;
}

void UiLayers::setLevel(int h, int16_t level) {
    if (h < 0 || static_cast<size_t>(h) >= clients_.size()) return;
    if (clients_[static_cast<size_t>(h)].level == level) return;
    clients_[static_cast<size_t>(h)].level = level;
    dirty_ = true;
}

void UiLayers::sort() {
    order_.clear();
    for (size_t i = 0; i < clients_.size(); ++i)
        if (alive_[i]) order_.push_back(static_cast<int>(i));
    std::sort(order_.begin(), order_.end(), [&](int a, int b) {
        const Client& A = clients_[static_cast<size_t>(a)];
        const Client& B = clients_[static_cast<size_t>(b)];
        if (A.strata != B.strata) return A.strata < B.strata;
        if (A.level != B.level) return A.level < B.level;
        return A.order < B.order;
    });
    dirty_ = false;
}

void UiLayers::draw(UiRenderer& ui) {
    if (dirty_) sort();
    for (int i : order_) {
        const Client& c = clients_[static_cast<size_t>(i)];
        if (c.visible && c.draw) c.draw(c.ctx, ui, c.strata);
    }
}

int UiLayers::hitTest(float x, float y) const {
    // Called after draw() in the frame, so order_ is current.
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        const Client& c = clients_[static_cast<size_t>(*it)];
        if (c.visible && c.hit && c.hit(c.ctx, x, y, c.strata)) return *it;
    }
    return -1;
}

uint32_t UiLayers::clientCount() const {
    uint32_t n = 0;
    for (bool a : alive_) n += a ? 1u : 0u;
    return n;
}

}  // namespace wowee::ui::psp
