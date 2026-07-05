/******************************************************************************
    Copyright (C) 2026 by Daniil Nabiulin

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#ifndef PLUGIN_WORKER_H
#define PLUGIN_WORKER_H

#include <QObject>
#include <QProcess>
#include <QMap>
#include <QMutex>
#include <QWaitCondition>
#include <thread>

#include "ipc_pipe.h"

#if defined(Q_OS_WINDOWS)
#include <Windows.h>
typedef DWORD m_pid_t;
#else
typedef pid_t m_pid_t;
#endif

#ifdef Q_OS_LINUX
#include <sys/capability.h>
#endif

class PluginWorker : public QObject
{
    Q_OBJECT
public:
    explicit PluginWorker(const QStringList &args, QObject *parent = nullptr);
    bool isProcessFound() const { return m_processFound; }
    void setConfig(const QString &json);
    void stop();

signals:
    void workerMessage(const QString &msg);
    void currentOutput(const QString &source, const QString &out);
    void confirmationRequired(const QString& message);

public slots:
    void run();
    void onConfirmationResult(bool confirmed);

private:
    void handleProcessState(m_pid_t pid);
    void onProcessFound(m_pid_t pid);
    void onProcessLost();
    void startPipeServer(const std::string& pipeName);
    void pipeReaderLoop();
    void handlePipeMessage(const IpcPipe::Message& msg);
    void cleanupAndUnload();

    void applyConfigJson(const QString &json);
    void debounceLoop();

    m_pid_t get_pid(const QString &process_name);
    uintptr_t get_module(const m_pid_t &pid, const QString &module_name);

    enum class arch {
        Unknown = 0,
        X86,
        X64
    };

    arch get_process_arch(const m_pid_t &pid);

#ifdef Q_OS_LINUX
    bool hasCapability(cap_value_t cap, const QString &programPath);
#endif
    void startInjectorProcess(const QString &command, const m_pid_t &pid, const QString &libPath, arch processArch);
    void parseLibraryHandle(QProcess* process);

    m_pid_t m_pid = 0;
    QString m_processName;
    QString m_pluginName;
    QMap<QString, QString> m_archPaths;
    arch m_processArch = arch::Unknown;
    void* m_libraryHandle = nullptr;
    bool m_running = true;
    bool m_processFound = false;

    std::unique_ptr<IpcPipe> m_pipe;
    std::thread              m_pipeThread;
    QMutex                   m_pipeMutex;

    bool m_confirmed = false;
    bool m_waitingConfirmation = false;
    QMutex m_confirmMutex;
    QWaitCondition m_confirmCondition;

    struct PendingText {
        QString                              text;
        std::chrono::steady_clock::time_point lastCharTime;
    };
    QMap<QString, PendingText> m_pendingText;
    QMutex                     m_pendingTextMutex;
    std::thread                m_debounceThread;
    std::atomic<bool>          m_debounceRunning { false };
    std::atomic<int>           m_flushIntervalMs { 350 };
    bool                       m_streamsCharacters = false;
};

#endif // PLUGIN_WORKER_H
