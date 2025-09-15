//
// Created by User on 08/09/2025.
//

#ifndef ANEAWESOMEUTILSWINDOWS_WINDOWSFILTERINPUTS_H
#define ANEAWESOMEUTILSWINDOWS_WINDOWSFILTERINPUTS_H
void StartHooksIfNeeded(bool filterAll, const std::vector<DWORD> &filteredKeys);
void StopHooks();
void SubclassMainWindow();
void UnsubclassMainWindow();
#endif //ANEAWESOMEUTILSWINDOWS_WINDOWSFILTERINPUTS_H