//
// Created by User on 08/09/2025.
//

#ifndef ANEAWESOMEUTILSWINDOWS_WINDOWSFILTERINPUTS_H
#define ANEAWESOMEUTILSWINDOWS_WINDOWSFILTERINPUTS_H
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>

extern "C" {
    void StartHooksIfNeeded(bool filterAll, const std::vector<DWORD>& filteredKeys);
    void StopHooks();
    void SubclassMainWindow();
    void UnsubclassMainWindow();
    void SetAneModuleHandle(HMODULE hModule);
}

#endif //ANEAWESOMEUTILSWINDOWS_WINDOWSFILTERINPUTS_H