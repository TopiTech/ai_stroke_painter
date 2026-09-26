/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiTestCrashGuard.h"
#include <QTest>
#include <QProcessEnvironment>

class KisAiCrashGuardTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testEnvironmentVariablesConfigured();
    void testWin32MessageBoxWInterception();
    void testWin32MessageBoxAInterception();
    void testErrorModeFlags();
};

void KisAiCrashGuardTest::initTestCase()
{
    KisAiTestCrashGuard::setupWindowsTestCrashGuard();
}

void KisAiCrashGuardTest::testEnvironmentVariablesConfigured()
{
    const QString noAssertMsg = qEnvironmentVariable("KRITA_NO_ASSERT_MSG");
    QCOMPARE(noAssertMsg, QStringLiteral("1"));

    const QString qpaPlatform = qEnvironmentVariable("QT_QPA_PLATFORM");
    QVERIFY(!qpaPlatform.isEmpty());

    const QString consoleStderr = qEnvironmentVariable("QT_ASSUME_STDERR_HAS_CONSOLE");
    QCOMPARE(consoleStderr, QStringLiteral("1"));
}

void KisAiCrashGuardTest::testWin32MessageBoxWInterception()
{
#if defined(Q_OS_WIN) || defined(_WIN32)
    // Calling Win32 MessageBoxW must NOT popup a modal dialog or block.
    // It should be intercepted by our inline hook and return immediately with IDOK or default.
    int ret1 = MessageBoxW(nullptr, L"Unit test non-interactive check", L"TestCaption", MB_OK);
    QCOMPARE(ret1, IDOK);

    int ret2 = MessageBoxW(nullptr, L"Yes/No check", L"TestCaption", MB_YESNO);
    QCOMPARE(ret2, IDYES);

    int ret3 = MessageBoxW(nullptr, L"Abort/Retry/Ignore check", L"TestCaption", MB_ABORTRETRYIGNORE);
    QCOMPARE(ret3, IDIGNORE);
#else
    QSKIP("Win32 MessageBoxW interception test is Windows-specific");
#endif
}

void KisAiCrashGuardTest::testWin32MessageBoxAInterception()
{
#if defined(Q_OS_WIN) || defined(_WIN32)
    int ret1 = MessageBoxA(nullptr, "Unit test non-interactive check", "TestCaption", MB_OK);
    QCOMPARE(ret1, IDOK);

    int ret2 = MessageBoxA(nullptr, "Yes/No check", "TestCaption", MB_YESNO);
    QCOMPARE(ret2, IDYES);
#else
    QSKIP("Win32 MessageBoxA interception test is Windows-specific");
#endif
}

void KisAiCrashGuardTest::testErrorModeFlags()
{
#if defined(Q_OS_WIN) || defined(_WIN32)
    // SetErrorMode(0) returns previous error mode
    UINT prevMode = SetErrorMode(0);
    // Restore error mode
    SetErrorMode(prevMode);

    QVERIFY((prevMode & SEM_FAILCRITICALERRORS) != 0);
    QVERIFY((prevMode & SEM_NOGPFAULTERRORBOX) != 0);
    QVERIFY((prevMode & SEM_NOOPENFILEERRORBOX) != 0);
#else
    QSKIP("Win32 SetErrorMode test is Windows-specific");
#endif
}

AI_STROKE_TEST_MAIN(KisAiCrashGuardTest)
#include "KisAiCrashGuardTest.moc"
