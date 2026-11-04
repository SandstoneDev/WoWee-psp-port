#include "ui/psp/ui_action_assignment.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
using wowee::ui::psp::ActionAssignment;
int main() {
    using wowee::ui::psp::actionBarBase;
    for (int page=0;page<6;++page) {
        assert(actionBarBase(page,0)==page*12);
        assert(actionBarBase(page,72)==(page ? page*12 : 72));
        assert(actionBarBase(page,120)==120);
    }
    ActionAssignment a;assert(!a.active());
    a.begin(133,72,3,123);assert(a.active() && a.base==72 && a.slot==3 && a.returnFocus==123);
    assert(a.activate(true)==ActionAssignment::ConfirmReplacement);
    a.select(4);assert(!a.confirming); // moving cancels the old confirmation
    assert(a.activate(true)==ActionAssignment::ConfirmReplacement);
    assert(a.activate(true)==ActionAssignment::Commit);
    a.cancel();assert(!a.active());
    a.begin(116,0,0,9);assert(a.activate(false)==ActionAssignment::Commit); // empty destination
    a.cancel();assert(!a.active() && a.returnFocus==9);
}
