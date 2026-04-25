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

#include "plugin_worker.h"
#include "shared_memory.h"

#include <QThread>
#include <QStandardPaths>

#ifdef Q_OS_LINUX
#include <fstream>
#include <string>
#include <sstream>
#include <fcntl.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#include <elf.h>
#endif

#ifdef Q_OS_WINDOWS
#include <Windows.h>
#include <tlhelp32.h>
#endif

PluginWorker::PluginWorker(const QStringList &args, QObject *parent)
    : QObject(parent)
{
    m_processName = args.at(0);
    m_libraryPath = args.at(1);
}

void PluginWorker::stop()
{
    m_running = false;
}

void PluginWorker::run()
{
    const std::string SHM_NAME = "AurexTranslator_" + m_processName.toStdString();
    const size_t SHM_SIZE = sizeof(SharedData);

    m_processFound = false;
    emit workerMessage(tr("[Hook] Searching for process: %1").arg(m_processName));

    m_pid_t pid = 0;
    std::unique_ptr<SharedMemory> shm;

    while (m_running) {
        pid = get_pid(m_processName);

        handleProcessState(pid, shm, SHM_NAME, SHM_SIZE);
        handleSharedMemoryMessages(shm);

        QThread::msleep(100);
    }

    cleanupAndUnload();
}

void PluginWorker::handleProcessState(m_pid_t pid, std::unique_ptr<SharedMemory>& shm, const std::string& shmName, size_t shmSize)
{
    if (pid > 0 && (!m_processFound || pid != m_pid)) {
        onProcessFound(pid, shm, shmName, shmSize);
    }
    else if (pid <= 0 && m_processFound) {
        onProcessLost(shm);
    }
}

void PluginWorker::onProcessFound(m_pid_t pid, std::unique_ptr<SharedMemory>& shm, const std::string& shmName, size_t shmSize)
{
    m_processFound = true;
    m_pid = pid;

    uintptr_t module = get_module(pid, m_libraryPath);

    if (module == 0) {
        emit workerMessage(tr("[Hook] Process \"%1\" found (PID: %2). Injecting...").arg(m_processName).arg(pid));
        QThread::msleep(1500);
        startInjectorProcess("load", pid, m_libraryPath);
    } else {
        emit workerMessage(tr("[Hook] Injection skipped: library already loaded. Awaiting text from game..."));
    }

    shm = std::make_unique<SharedMemory>(shmName, shmSize, false);
}

void PluginWorker::onProcessLost(std::unique_ptr<SharedMemory>& shm)
{
    shm.reset();
    m_processFound = false;
    emit workerMessage(tr("[Hook] Searching for process: %1").arg(m_processName));
}

void PluginWorker::handleSharedMemoryMessages(std::unique_ptr<SharedMemory>& shm)
{
    if (!shm) return;

    auto msg = shm->receive();
    if (!msg) return;

    switch (msg->type) {
    case MsgType::Status:
        switch (msg->status_code) {
        case StatusCode::Success:
            emit workerMessage(tr("[Hook] Injection succeeded (PID: %1). Awaiting text from game...").arg(m_pid));
            break;
        case StatusCode::Failure:
            emit workerMessage(tr("[Hook] Error (PID: %1): %2.").arg(m_pid).arg(QString::fromStdString(msg->text)));
            stop();
            break;
        }
        break;
    case MsgType::Text:
        emit currentOutput("Hook", QString::fromStdString(msg->text));
        break;
    case MsgType::Info:
        emit workerMessage(tr("[Hook] %1").arg(QString::fromStdString(msg->text)));
        break;
    }
}

void PluginWorker::cleanupAndUnload()
{
    if (m_pid > 0 && m_libraryHandle) {
        startInjectorProcess("unload", m_pid, QString());
    }
}

m_pid_t PluginWorker::get_pid(const QString &process_name)
{
    m_pid_t pid = 0;
#if defined(Q_OS_WINDOWS)
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 process_entry;
        process_entry.dwSize = sizeof(process_entry);

        if (Process32First(hSnap, &process_entry)) {
            do {
                if (!std::wcscmp(process_entry.szExeFile, process_name.toStdWString().c_str())) {
                    pid = process_entry.th32ProcessID;
                    break;
                }
            } while (Process32Next(hSnap, &process_entry));
        }
    }
    CloseHandle(hSnap);
#else
    DIR* dir = opendir("/proc");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            int id = atoi(entry->d_name);
            if (id > 0) {
                std::string comm_path = "/proc/" + std::string(entry->d_name) + "/comm";
                std::ifstream comm_file(comm_path);
                if (!comm_file) {
                    continue;
                }

                std::string comm;
                std::getline(comm_file, comm);
                comm_file.close();

                if (comm == process_name.toStdString()) {
                    pid = id;
                    break;
                }
            }
        } closedir(dir);
    }
#endif
    return pid;
}

uintptr_t PluginWorker::get_module(const m_pid_t &pid, const QString &module_name)
{
    uintptr_t module_base = 0;
#if defined(Q_OS_WINDOWS)
    const auto moduleNameW = module_name.toStdWString();
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 module_entry;
        module_entry.dwSize = sizeof(module_entry);
        if (Module32First(hSnap, &module_entry)) {
            do {
                if (!wcscmp(module_entry.szModule, moduleNameW.c_str()) || !wcscmp(module_entry.szExePath, moduleNameW.c_str())) {
                    module_base = (uintptr_t)module_entry.modBaseAddr;
                    break;
                }
            } while (Module32Next(hSnap, &module_entry));
        }
    }
    CloseHandle(hSnap);
#else
    std::stringstream maps_file;
    maps_file << "/proc/" << pid << "/maps";
    std::ifstream maps_ifst(maps_file.str());

    if(!maps_ifst.is_open()) return module_base;

    maps_file.str(std::string());
    maps_file << maps_ifst.rdbuf();

    size_t module_base_path = maps_file.str().find(module_name.toStdString());
    size_t module_base_start = maps_file.str().rfind('\n', module_base_path);
    if (module_base_start == maps_file.str().npos) { module_base_start = 0; }

    size_t module_base_end = maps_file.str().find('-', module_base_path);
    if(module_base_end == maps_file.str().npos) return module_base;

    module_base = std::stoull(maps_file.str().substr(module_base_start, module_base_end - module_base_start), nullptr, 16);
    maps_ifst.close();
#endif
    return module_base;
}

PluginWorker::arch PluginWorker::get_process_arch(const m_pid_t &pid) {
#if defined(_WIN32)
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProcess) return PluginWorker::arch::Unknown;

    BOOL isWow64 = FALSE;
    PluginWorker::arch result = PluginWorker::arch::Unknown;

    if (IsWow64Process(hProcess, &isWow64)) {
#if defined(_WIN64)
        result = isWow64 ? PluginWorker::arch::X86 : PluginWorker::arch::X64;
#else
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        bool isSystem64 = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
                           si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64);

        if (isSystem64 && !isWow64) {
            result = PluginWorker::arch::X64;
        } else {
            result = PluginWorker::arch::X86;
        }
#endif
    }

    CloseHandle(hProcess);
    return result;
#else
    std::string exe_path = "/proc/" + std::to_string(pid) + "/exe";

    std::ifstream f(exe_path, std::ios::binary);
    if (!f) return PluginWorker::arch::Unknown;

    unsigned char e_ident[EI_NIDENT];
    f.read(reinterpret_cast<char*>(e_ident), EI_NIDENT);
    if (!f) return PluginWorker::arch::Unknown;

    if (e_ident[EI_MAG0] != ELFMAG0 || e_ident[EI_MAG1] != ELFMAG1 ||
        e_ident[EI_MAG2] != ELFMAG2 || e_ident[EI_MAG3] != ELFMAG3)
        return PluginWorker::arch::Unknown;

    switch (e_ident[EI_CLASS]) {
    case ELFCLASS32: return PluginWorker::arch::X86;
    case ELFCLASS64: return PluginWorker::arch::X64;
    default:         return PluginWorker::arch::Unknown;
    }
#endif
}

#ifdef Q_OS_LINUX
bool PluginWorker::hasCapability(cap_value_t cap, const QString &programPath)
{
    cap_t caps = cap_get_file(programPath.toStdString().c_str());
    if (caps == nullptr) {
        int err = errno;
        fprintf(stderr, "cap_get_file failed for %s: %s (%d)\n", programPath.toStdString().c_str(), strerror(err), err);
        return false;
    }

    cap_flag_value_t value;
    if (cap_get_flag(caps, cap, CAP_EFFECTIVE, &value) == -1) {
        cap_free(caps);
        return false;
    }

    cap_free(caps);

    return (value == CAP_SET);
}
#endif

void PluginWorker::startInjectorProcess(const QString &command, const m_pid_t &pid, const QString &libPath)
{
    arch current_arch = get_process_arch(pid);
    QString archSuffix;
    switch (current_arch) {
    case arch::X86:
        archSuffix = QStringLiteral("_x86");
        break;
    case arch::X64:
        archSuffix = QStringLiteral("_x64");
        break;
    default:
        emit workerMessage(tr("[Hook] Unknown architecture for process: %1").arg(m_processName));
        stop();
        return;
    }

    QString programPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                          + QStringLiteral("/plugins/bin/at-injector")
                          + archSuffix;

    QProcess *process = new QProcess();
    QObject::connect(process, &QProcess::finished, [=](int exitCode, QProcess::ExitStatus status) {
        if (exitCode == 0 && status == QProcess::NormalExit) {
            if (command == "load") {
                emit workerMessage(tr("[Hook] Waiting for plugin response..."));
                parseLibraryHandle(process);
            }
        } else {
            QString errMsg = QString("[Hook] Command \"%1\" failed (code %2, %3)")
            .arg(command)
                .arg(exitCode)
                .arg(status == QProcess::CrashExit ? QString("crashed")
                                                   : QString("normal exit"));

            QByteArray stdErr = process->readAllStandardError();
            if (!stdErr.isEmpty())
                errMsg += "\n" + QString::fromLocal8Bit(stdErr);

            emit workerMessage(errMsg);
            stop();
        }
        process->deleteLater();
    });

    QStringList args;
#if defined(Q_OS_LINUX)
    bool hasPtrace = hasCapability(CAP_SYS_PTRACE, programPath);

    if (hasPtrace) {
        process->setProgram(programPath);
        args = { command, "--pid", QString::number(pid) };
    } else {
        process->setProgram("pkexec");
        args = { programPath, command, "--pid", QString::number(pid) };
    }
#elif defined(Q_OS_WINDOWS)
    process->setProgram(programPath);
    args = { command, "--pid", QString::number(pid) };
#endif

    if (command == "load") {
        args << "--library"
             << libPath;
    } else {
        QString handleStr = QString::asprintf("%p", m_libraryHandle);
#if defined(Q_OS_WINDOWS)
        args << "--module-base" << handleStr;
#else
        args << "--handle" << handleStr;
#endif
    }

    process->setArguments(args);
    process->start();
    process->waitForFinished();
}

void PluginWorker::parseLibraryHandle(QProcess* process)
{
    QString output = process->readAllStandardOutput();
#if defined(Q_OS_WINDOWS)
    QString marker = "module base = ";
#else
    QString marker = "handle = ";
#endif
    int idx = output.indexOf(marker);

    if (idx == -1) {
        emit workerMessage(tr("[Hook] Failed to find handle marker in output"));
        return;
    }

    int start = idx + marker.length();
    int end = output.indexOf(QChar::Space, start);
    if (end == -1) end = output.indexOf('\n', start);

    QString hexStr = output.mid(start, end - start).trimmed();
    bool ok = false;
    qulonglong addr = hexStr.toULongLong(&ok, 16);

    if (!ok) {
        emit workerMessage(tr("[Hook] Failed to parse address: %1").arg(hexStr));
        return;
    }

    m_libraryHandle = reinterpret_cast<void*>(addr);
}
