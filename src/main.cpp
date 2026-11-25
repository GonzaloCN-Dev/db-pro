#include <windows.h>

#include <cstdlib>

#include "core/brand.h"
#include "diagnostics.h"
#include "platform.h"

int run_app();

int main()
{
    dbp::diagnostics::install_crash_handler();
    // One window per user session: a second launch just brings the first one forward.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\DbPro.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(nullptr, L"Db Pro")) {
            if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
        }
        return 0;
    }

    // The GL renderer shows a blank window on some drivers; software rendering is reliable everywhere.
    if (!std::getenv("SLINT_BACKEND")) _putenv_s("SLINT_BACKEND", "winit-software");

    dbp::diagnostics::begin_session(dbp::platform::data_dir() + "\\logs", dbp::brand::kVersion);
    int rc = run_app();
    dbp::diagnostics::end_session_clean();
    return rc;
}
