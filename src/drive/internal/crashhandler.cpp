/*
 * SPDX-License-Identifier: GPL-3.0-only
 * VibeScore - MuseScore Studio for mobile devices
 *
 * Copyright (C) 2026 VibeScore contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "crashhandler.h"

#include <QDateTime>
#include <QDir>

#if defined(Q_OS_ANDROID) || defined(Q_OS_LINUX)
#define VS_CRASH_HANDLER
#endif

#ifdef VS_CRASH_HANDLER
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <unwind.h>
#endif

using namespace mu::drive;

#ifdef VS_CRASH_HANDLER
namespace {
// Everything the handler needs is prepared beforehand: in a signal handler only async-signal-safe calls are
// allowed (open, write, close; dladdr and the unwinder are not strictly safe, but are what crash reporters use)
char s_reportPath[1024];
char s_header[256];
constexpr int SIGNALS[] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTRAP };
struct sigaction s_previous[32];
char s_altStack[64 * 1024];

void writeStr(int fd, const char* s)
{
    if (s) {
        ssize_t r = ::write(fd, s, strlen(s));
        (void)r;
    }
}

void writeHex(int fd, uintptr_t value)
{
    char buf[2 + sizeof(uintptr_t) * 2 + 1];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < int(sizeof(uintptr_t) * 2); ++i) {
        const int nibble = (value >> ((sizeof(uintptr_t) * 2 - 1 - i) * 4)) & 0xF;
        buf[2 + i] = char(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    buf[sizeof(buf) - 1] = 0;
    writeStr(fd, buf);
}

void writeInt(int fd, long value)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", value);
    writeStr(fd, buf);
}

struct Trace {
    uintptr_t pcs[64];
    int count = 0;
};

_Unwind_Reason_Code collect(struct _Unwind_Context* context, void* arg)
{
    Trace* trace = static_cast<Trace*>(arg);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc && trace->count < 64) {
        trace->pcs[trace->count++] = pc;
    }
    return trace->count < 64 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void onCrash(int sig, siginfo_t* info, void* context)
{
    const int fd = ::open(s_reportPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        writeStr(fd, s_header);
        writeStr(fd, "signal ");
        writeInt(fd, sig);
        writeStr(fd, " (");
        writeStr(fd, strsignal(sig));
        writeStr(fd, "), code ");
        writeInt(fd, info ? info->si_code : 0);
        writeStr(fd, ", fault address ");
        writeHex(fd, info ? uintptr_t(info->si_addr) : 0);
        writeStr(fd, "\nthread ");
        writeInt(fd, long(gettid()));
        writeStr(fd, "\n\nstack (library + offset, symbol if exported):\n");

        Trace trace;
        _Unwind_Backtrace(collect, &trace);
        for (int i = 0; i < trace.count; ++i) {
            Dl_info dl;
            writeStr(fd, "#");
            writeInt(fd, i);
            writeStr(fd, " pc ");
            if (dladdr(reinterpret_cast<void*>(trace.pcs[i]), &dl) && dl.dli_fname) {
                writeHex(fd, trace.pcs[i] - uintptr_t(dl.dli_fbase));
                writeStr(fd, "  ");
                const char* name = strrchr(dl.dli_fname, '/');
                writeStr(fd, name ? name + 1 : dl.dli_fname);
                if (dl.dli_sname) {
                    writeStr(fd, " (");
                    writeStr(fd, dl.dli_sname);
                    writeStr(fd, "+");
                    writeHex(fd, trace.pcs[i] - uintptr_t(dl.dli_saddr));
                    writeStr(fd, ")");
                }
            } else {
                writeHex(fd, trace.pcs[i]);
            }
            writeStr(fd, "\n");
        }
        writeStr(fd, "\n");
        ::close(fd);
    }

    // Then as if we were not here: the system's handler (Android: the tombstone and the "app has stopped" dialog)
    for (size_t i = 0; i < sizeof(SIGNALS) / sizeof(SIGNALS[0]); ++i) {
        if (SIGNALS[i] == sig) {
            sigaction(sig, &s_previous[i], nullptr);
            break;
        }
    }
    raise(sig);
}
}
#endif

void CrashHandler::install(const QString& logsDir)
{
#ifdef VS_CRASH_HANDLER
    QDir().mkpath(logsDir);
    const QByteArray path = (logsDir + "/crash_" + QDateTime::currentDateTime().toString("yyMMdd_HHmmss") + ".txt").toUtf8();
    strncpy(s_reportPath, path.constData(), sizeof(s_reportPath) - 1);
    const QByteArray header = ("Crash report. App started " + QDateTime::currentDateTime().toString(Qt::ISODate)
                               + ", build " + QStringLiteral(VS_BUILD_ID) + "\n").toUtf8();
    strncpy(s_header, header.constData(), sizeof(s_header) - 1);

    // The main thread's stack may be the one that overflowed
    stack_t altStack {};
    altStack.ss_sp = s_altStack;
    altStack.ss_size = sizeof(s_altStack);
    sigaltstack(&altStack, nullptr);

    struct sigaction action {};
    action.sa_sigaction = onCrash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(SIGNALS) / sizeof(SIGNALS[0]); ++i) {
        sigaction(SIGNALS[i], &action, &s_previous[i]);
    }
#else
    Q_UNUSED(logsDir)
#endif
}
