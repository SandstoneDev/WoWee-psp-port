#pragma once
constexpr int PSP_O_RDONLY=1;
int sceIoOpen(const char*,int,int);
int sceIoClose(int);
