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

#include "shared_memory.h"

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
    void stop();

signals:
    void workerMessage(const QString &msg);
    void currentOutput(const QString &source, const QString &out);

public slots:
    void run();

private:
    void handleProcessState(m_pid_t pid, std::unique_ptr<SharedMemory>& shm, const std::string& shmName, size_t shmSize);
    void onProcessFound(m_pid_t pid, std::unique_ptr<SharedMemory>& shm, const std::string& shmName, size_t shmSize);
    void onProcessLost(std::unique_ptr<SharedMemory>& shm);
    void handleSharedMemoryMessages(std::unique_ptr<SharedMemory>& shm);
    void cleanupAndUnload();

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
    void startInjectorProcess(const QString &command, const m_pid_t &pid, const QString &libPath);
    void parseLibraryHandle(QProcess* process);

    m_pid_t m_pid = 0;
    QString m_processName;
    QString m_libraryPath;
    void* m_libraryHandle = nullptr;
    bool m_running = true;
    bool m_processFound = false;
};

#endif // PLUGIN_WORKER_H
