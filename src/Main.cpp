// Main.cpp -- ASI entry point.

#include <windows.h>

#include "Overlay.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        Overlay::Install();  // spawns a worker; no work under the loader lock
        break;
    case DLL_PROCESS_DETACH:
        Overlay::Remove();
        break;
    default:
        break;
    }
    return TRUE;
}
