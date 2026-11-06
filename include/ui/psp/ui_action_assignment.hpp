#pragma once
#include <cstdint>
namespace wowee::ui::psp {
// Manual bars can be selected while a stance has a bonus bar; vehicle overrides stay authoritative.
inline int actionBarBase(int page,int bonusBase) {
    if (bonusBase>=120) return bonusBase;
    page=page<0 ? 0 : page>5 ? 5 : page;
    return page==0 && bonusBase>=0 ? bonusBase : page*12;
}
// Selection state only. Nothing is removed or written until the caller receives Commit.
struct ActionAssignment {
    enum Result { ConfirmReplacement, Commit };
    uint32_t spell = 0, returnFocus = 0;
    int base = 0, slot = 0;
    bool confirming = false;
    bool active() const { return spell != 0; }
    void begin(uint32_t id, int barBase, int selected, uint32_t origin) {
        spell=id; base=barBase; slot=selected; returnFocus=origin; confirming=false;
    }
    void select(int selected) { if (slot!=selected) { slot=selected; confirming=false; } }
    Result activate(bool replacing) {
        if (replacing && !confirming) { confirming=true; return ConfirmReplacement; }
        return Commit;
    }
    void cancel() { spell=0; confirming=false; }
};
}
