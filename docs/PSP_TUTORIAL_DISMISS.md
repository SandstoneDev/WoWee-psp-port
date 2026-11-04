# Closing automatic tutorial windows

DamagedItems is tutorial 36. Tutorials could auto-open while gameplay owned
the pad, or over another panel's modal focus. Closing one could also immediately
open the next queued tutorial, making the close action appear ineffective.

Circle now dismisses a visible topmost tutorial before gameplay/window input,
without requiring SELECT or cursor placement. The press is consumed and held
Circle is suppressed until release, preventing an unintended action afterward.
Foreground menus/settings/logout still own their input. Other queued tips stay
available through the alert instead of appearing immediately after closing.
The explicit close button and generic stack-close path use the same deferred
queue behavior. Auto-open only occurs during clear gameplay outside combat;
tips arriving over panels/settings/logout remain alerts.

Host tests passed for tutorial opening/dismissal, deferred queue handling,
foreground ownership, combat/settings/menu interruption, and existing game-menu
and window-drag behavior. Tutorial source syntax passed with PSP SDK headers.
Full EBOOT compilation and device testing remain to be done. Rebuild and copy
EBOOT.PBP only; no asset pack rebuild is needed.
