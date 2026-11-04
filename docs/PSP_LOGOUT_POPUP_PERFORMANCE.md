# Logout popup texture lifetime

The game menu closes after requesting logout. beginPopups did not count logoutPending as a popup owner, so it released all six shared dialog textures; drawLogoutCountdown then loaded them again from ui.pak on that same frame. This repeated throughout the countdown, introducing synchronous memory-card reads, texture allocation and cache churn every frame.

The retention check now includes pending logout. Textures load once for the countdown and become eligible for normal cleanup after cancellation or completion. The server-authoritative timer and Cancel controls are unchanged. Hardware frame-rate recovery still needs validation.

Undercity: existing collision_wait diagnostics now also report terrain/doodad readiness to distinguish the remaining gates. These diagnostic additions do not claim to resolve a new hardware reproduction.
