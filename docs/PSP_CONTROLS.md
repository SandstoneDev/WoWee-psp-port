# Follow camera

With controls=2, camfollow=1 and camsmartfollow=1, movement is calculated first using the current camera yaw. After movement sets this frame's character facing, the camera turns toward that facing using the old camera's exponential smoothing rate (1.6 per second). Follow runs on every moving frame, including sideways/backward input, autorun and taxis. There is no thumb-direction gating, stored steering angle, gesture lock, or steering rebase.

R + stick remains manual camera orbit. Idle preserves the view. Pitch, zoom and collision are unchanged. Old mode retains its previous pre-movement follow placement; Smart mode uses the same gentle follow motion after movement and also includes autorun.

Settings: Camera follow On and Camera system Smart. Regression tests cover the exact old smoothing rate, frame-rate independence, shortest-arc wrapping, current-view steering and continuous following across changing input directions. PSP/emulator feel testing remains required.
