/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_TEST_CRASH_GUARD_H
#define KIS_AI_TEST_CRASH_GUARD_H

#include <QtGlobal>
#include <QTest>
#if defined(QT_WIDGETS_LIB)
#include <QtWidgets/QApplication>
#define AI_STROKE_TEST_APP QApplication
#elif defined(QT_GUI_LIB)
#include <QtGui/QGuiApplication>
#define AI_STROKE_TEST_APP QGuiApplication
#else
#include <QtCore/QCoreApplication>
#define AI_STROKE_TEST_APP QCoreApplication
#endif
#include <QProcessEnvironment>
#include <cstdio>
#include <cstdlib>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <windows.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#endif

namespace KisAiTestCrashGuard {

#if defined(Q_OS_WIN) || defined(_WIN32)

static inline int WINAPI MockMessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType)
{
    Q_UNUSED(hWnd);
    fprintf(stderr, "\n[CRASH GUARD] Suppressed Win32 MessageBoxW popup! Caption: '%ls', Text: '%ls', Type: 0x%X\n",
            lpCaption ? lpCaption : L"(null)", lpText ? lpText : L"(null)", uType);
    fflush(stderr);

    const UINT type = (uType & MB_TYPEMASK);
    if (type == MB_YESNO) return IDYES;
    if (type == MB_YESNOCANCEL) return IDYES;
    if (type == MB_ABORTRETRYIGNORE) return IDIGNORE;
    if (type == MB_CANCELTRYCONTINUE) return IDCONTINUE;
    if (type == MB_RETRYCANCEL) return IDCANCEL;
    return IDOK;
}

static inline int WINAPI MockMessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType)
{
    Q_UNUSED(hWnd);
    fprintf(stderr, "\n[CRASH GUARD] Suppressed Win32 MessageBoxA popup! Caption: '%s', Text: '%s', Type: 0x%X\n",
            lpCaption ? lpCaption : "(null)", lpText ? lpText : "(null)", uType);
    fflush(stderr);

    const UINT type = (uType & MB_TYPEMASK);
    if (type == MB_YESNO) return IDYES;
    if (type == MB_YESNOCANCEL) return IDYES;
    if (type == MB_ABORTRETRYIGNORE) return IDIGNORE;
    if (type == MB_CANCELTRYCONTINUE) return IDCONTINUE;
    if (type == MB_RETRYCANCEL) return IDCANCEL;
    return IDOK;
}

#if defined(__x86_64__) || defined(_M_X64)
inline bool patchFunctionJump64(void *targetFunc, void *hookFunc)
{
    if (!targetFunc || !hookFunc) return false;

    DWORD oldProtect = 0;
    // 12 bytes on x86_64:
    // 48 B8 [8-byte target address] : mov rax, <addr>
    // FF E0                         : jmp rax
    const SIZE_T patchSize = 12;
    if (!VirtualProtect(targetFunc, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    unsigned char patch[12];
    patch[0] = 0x48;
    patch[1] = 0xB8;
    uintptr_t hookAddr = reinterpret_cast<uintptr_t>(hookFunc);
    memcpy(&patch[2], &hookAddr, sizeof(hookAddr));
    patch[10] = 0xFF;
    patch[11] = 0xE0;

    memcpy(targetFunc, patch, patchSize);
    VirtualProtect(targetFunc, patchSize, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), targetFunc, patchSize);
    return true;
}
#endif

static inline LONG WINAPI TestUnhandledExceptionFilter(EXCEPTION_POINTERS *pExceptionInfo)
{
    DWORD code = 0;
    void *addr = nullptr;
    if (pExceptionInfo && pExceptionInfo->ExceptionRecord) {
        code = pExceptionInfo->ExceptionRecord->ExceptionCode;
        addr = pExceptionInfo->ExceptionRecord->ExceptionAddress;
    }
    fprintf(stderr, "\n[CRASH GUARD FATAL] Unhandled exception 0x%08lX at address %p. Terminating process immediately to prevent WerFault hang.\n",
            static_cast<unsigned long>(code), addr);
    fflush(stderr);

    // Immediately terminate process to avoid WerFault.exe dialog and hang
    TerminateProcess(GetCurrentProcess(), code ? code : 1);
    return EXCEPTION_EXECUTE_HANDLER;
}

#endif // defined(Q_OS_WIN) || defined(_WIN32)

/**
 * Configure comprehensive crash and message box suppression for unit test execution.
 * Prevents WerFault.exe, CRT assertion message boxes, and Win32 MessageBoxW popups
 * from freezing automated test runs.
 */
inline void setupWindowsTestCrashGuard()
{
#if defined(Q_OS_WIN) || defined(_WIN32)
    static bool s_guardInstalled = false;
    if (s_guardInstalled) return;
    s_guardInstalled = true;

    // 1. Disable Windows Error Mode GUI boxes (critical errors, GP faults, open file errors)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    // 2. SetProcessErrorMode (Windows 7/Server 2008 R2 and newer)
    typedef BOOL (WINAPI *SetProcessErrorModeFunc)(DWORD);
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (hKernel32) {
        SetProcessErrorModeFunc pSetProcessErrorMode =
            reinterpret_cast<SetProcessErrorModeFunc>(reinterpret_cast<void*>(GetProcAddress(hKernel32, "SetProcessErrorMode")));
        if (pSetProcessErrorMode) {
            pSetProcessErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        }
    }

    // 3. Suppress WerFault.exe GUI via WerSetFlags (Windows Vista / Windows Server 2008 and newer)
    typedef HRESULT (WINAPI *WerSetFlagsFunc)(DWORD);
    HMODULE hWer = LoadLibraryW(L"wer.dll");
    if (hWer) {
        WerSetFlagsFunc pWerSetFlags =
            reinterpret_cast<WerSetFlagsFunc>(reinterpret_cast<void*>(GetProcAddress(hWer, "WerSetFlags")));
        if (pWerSetFlags) {
            // WER_FAULT_REPORTING_NO_UI = 0x0020
            // WER_FAULT_REPORTING_NO_CONSOLE_LOG = 0x0040
            pWerSetFlags(0x0020);
        }
    }

    // 4. CRT assertion & abort behavior suppression
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
#elif defined(__MINGW32__)
    // Under MinGW targeting msvcrt.dll, _set_abort_behavior is not exported by libmsvcrt.a,
    // which results in an undefined reference to `__imp__set_abort_behavior` during linking.
    // Dynamically resolve CRT functions from the loaded CRT DLL if available.
    typedef unsigned int (__cdecl *SetAbortBehaviorFunc)(unsigned int, unsigned int);
    typedef int (__cdecl *SetErrorModeFunc)(int);
    HMODULE hCrt = GetModuleHandleW(L"ucrtbase.dll");
    if (!hCrt) {
        hCrt = GetModuleHandleW(L"msvcrt.dll");
    }
    if (hCrt) {
        SetAbortBehaviorFunc pSetAbortBehavior =
            reinterpret_cast<SetAbortBehaviorFunc>(reinterpret_cast<void*>(GetProcAddress(hCrt, "_set_abort_behavior")));
        if (pSetAbortBehavior) {
            pSetAbortBehavior(0, 0x0001 /* _WRITE_ABORT_MSG */ | 0x0002 /* _CALL_REPORTFAULT */);
        }
        SetErrorModeFunc pSetErrorMode =
            reinterpret_cast<SetErrorModeFunc>(reinterpret_cast<void*>(GetProcAddress(hCrt, "_set_error_mode")));
        if (pSetErrorMode) {
            pSetErrorMode(1 /* _OUT_TO_STDERR */);
        }
    }
#endif

    // 5. Unhandled exception filter to terminate without WerFault intervention
    SetUnhandledExceptionFilter(TestUnhandledExceptionFilter);

    // 6. Hook MessageBoxW and MessageBoxA in user32.dll
    HMODULE hUser32 = LoadLibraryW(L"user32.dll");
    if (hUser32) {
#if defined(__x86_64__) || defined(_M_X64)
        void *pMessageBoxW = reinterpret_cast<void*>(GetProcAddress(hUser32, "MessageBoxW"));
        if (pMessageBoxW) {
            patchFunctionJump64(pMessageBoxW, reinterpret_cast<void*>(MockMessageBoxW));
        }
        void *pMessageBoxA = reinterpret_cast<void*>(GetProcAddress(hUser32, "MessageBoxA"));
        if (pMessageBoxA) {
            patchFunctionJump64(pMessageBoxA, reinterpret_cast<void*>(MockMessageBoxA));
        }
#endif
    }
#endif // defined(Q_OS_WIN) || defined(_WIN32)

    // 7. Process environment variables
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    qputenv("QT_ASSUME_STDERR_HAS_CONSOLE", "1");
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
}

} // namespace KisAiTestCrashGuard

/**
 * Robust test main macro for AI Stroke Painter tests.
 * Ensures crash guard is invoked before any Qt initialization or test logic.
 */
#define AI_STROKE_TEST_MAIN(TestClass) \
int main(int argc, char *argv[]) \
{ \
    KisAiTestCrashGuard::setupWindowsTestCrashGuard(); \
    AI_STROKE_TEST_APP app(argc, argv); \
    TestClass tc; \
    return QTest::qExec(&tc, argc, argv); \
}

#endif // KIS_AI_TEST_CRASH_GUARD_H
