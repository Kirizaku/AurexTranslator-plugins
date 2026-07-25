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

#include <QThread>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>
#include <QJsonObject>

#ifdef Q_OS_LINUX
#include <fstream>
#include <string>
#include <sstream>

#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#include <elf.h>
#endif

#ifdef Q_OS_WINDOWS
#include <Windows.h>
#include <tlhelp32.h>
#endif

static QString sanitizeSource(QString name)
{
    name = name.trimmed();
    name.remove(QLatin1Char(':'));
    name.truncate(64);
    if (name.size() >= 2
        && (name.at(0) == QLatin1Char('v') || name.at(0) == QLatin1Char('g'))
        && name.at(1).isDigit())
        name.clear();
    return name;
}

PluginWorker::PluginWorker(const QStringList &args, QObject *parent)
    : QObject(parent)
{
    m_processName = args.at(0);
    m_pluginName = args.at(1);

    const QStringList entries = args.at(2).split(";", Qt::SkipEmptyParts);
    for (const QString& entry : entries) {
        const int sep = entry.indexOf(':');
        if (sep == -1) continue;
        m_archPaths[entry.left(sep)] = entry.mid(sep + 1);
    }

    // text_mode
    m_streamsCharacters = (args.size() > 3 && args.at(3) == QLatin1String("per_char"));
}

void PluginWorker::setConfig(const QString &json)
{
    QMutexLocker lk(&m_pipeMutex);
    applyConfigJson(json);
}

void PluginWorker::stop()
{
    m_running = false;

    QMutexLocker lk(&m_confirmMutex);
    m_confirmed = false;
    m_waitingConfirmation = false;
    m_confirmCondition.wakeAll();
}

void PluginWorker::run()
{
    // Only per_char plugins accumulate text that needs a silence-based flush;
    // whole-text plugins forward each block immediately, so don't spin an idle
    // poller for them
    if (m_streamsCharacters) {
        m_debounceRunning.store(true, std::memory_order_relaxed);
        m_debounceThread = std::thread([this] { debounceLoop(); });
    }

    m_processFound = false;
    emit workerMessage(tr("[Hook] Searching for process: %1").arg(m_processName));

    m_pid_t pid = 0;

    while (m_running) {
        pid = get_pid(m_processName);
        handleProcessState(pid);
        QThread::msleep(100);
    }

    {
        QMutexLocker lk(&m_pipeMutex);
        if (m_pipe) m_pipe->close();
    }

    if (m_pipeThread.joinable()) m_pipeThread.join();
    {
        QMutexLocker lk(&m_pipeMutex);
        m_pipe.reset();
    }

    m_debounceRunning.store(false, std::memory_order_relaxed);
    if (m_debounceThread.joinable()) m_debounceThread.join();
    QMap<QString, PendingText> remaining;
    {
        QMutexLocker lk(&m_pendingTextMutex);
        remaining.swap(m_pendingText);
    }
    for (auto it = remaining.begin(); it != remaining.end(); ++it)
        emit currentOutput(it.key(), it->text);

    cleanupAndUnload();
}

void PluginWorker::onConfirmationResult(bool confirmed)
{
    m_confirmMutex.lock();
    m_confirmed = confirmed;
    m_waitingConfirmation = false;
    m_confirmCondition.wakeAll();
    m_confirmMutex.unlock();
}

void PluginWorker::handleProcessState(m_pid_t pid)
{
    if (pid > 0 && (!m_processFound || pid != m_pid)) {
        onProcessFound(pid);
    }
    else if (pid <= 0 && m_processFound) {
        onProcessLost();
    }
}

void PluginWorker::onProcessFound(m_pid_t pid)
{
    m_processFound = true;
    m_pid = pid;

    m_processArch = get_process_arch(pid);
    if (m_processArch == arch::Unknown) {
        emit workerMessage(tr("[Hook] Unknown architecture for process: %1").arg(m_processName));
        stop();
        return;
    }

    const QString archStr = (m_processArch == arch::X86)
                                ? QStringLiteral("x86")
                                : QStringLiteral("x64");

    const QString libPath = m_archPaths.value(archStr);
    if (libPath.isEmpty()) {
        emit workerMessage(tr("[Hook] No library found for %1").arg(m_pluginName));
        stop();
        return;
    }
    const QString libFileName = QFileInfo(libPath).fileName();
    uintptr_t module = get_module(pid, libFileName);

    if (module != 0) {
        void* handle = nullptr;
#if defined(Q_OS_WINDOWS)
        handle = reinterpret_cast<void*>(module);
#else
        handle = persistedHandleFor(pid);
        if (!handle) {
            emit workerMessage(tr("[Hook] Library already loaded in \"%1\" but its handle is "
                                  "unknown; cannot unload cleanly. Please close the game and "
                                  "start the hook again").arg(m_processName));
            stop();
            return;
        }
#endif

        emit workerMessage(tr("[Hook] Stale library found in \"%1\". Reinjecting...").arg(m_processName));
        m_libraryHandle = handle;
        startInjectorProcess(QStringLiteral("unload"), pid, QString(), m_processArch);
        m_libraryHandle = nullptr;

        if (!m_running) return; // unload failed and triggered stop()

#ifdef Q_OS_LINUX
        clearPersistedHandle();
#endif
        module = 0;
    }

    if (module == 0) {
        emit workerMessage(tr("[Hook] Process \"%1\" found (PID: %2). Injecting...").arg(m_processName).arg(pid));

        constexpr int STABLE_CHECKS = 10;
        constexpr int CHECK_INTERVAL = 50;
        int stableCount = 0;
        m_pid_t lastPid = pid;

        while (m_running && stableCount < STABLE_CHECKS) {
            QThread::msleep(CHECK_INTERVAL);
            const m_pid_t currentPid = get_pid(m_processName);

            if (currentPid <= 0) {
                emit workerMessage(tr("[Hook] Process disappeared, waiting..."));
                stableCount = 0;
                lastPid = 0;
                continue;
            }

            if (currentPid != lastPid) {
                emit workerMessage(tr("[Hook] Process restarted (PID: %1 → %2), waiting...")
                    .arg(lastPid).arg(currentPid));
                lastPid = currentPid;
                stableCount = 0;
                continue;
            }

            ++stableCount;
        }

        if (!m_running) return;

        if (lastPid != pid) {
            m_pid = lastPid;
            emit workerMessage(tr("[Hook] Process stable (PID: %1)").arg(m_pid));
        }

        m_waitingConfirmation = true;
        m_confirmed = false;
        emit confirmationRequired(tr("Inject into \"%1\" (PID: %2)?").arg(m_processName).arg(m_pid));

        m_confirmMutex.lock();
        m_confirmCondition.wait(&m_confirmMutex);
        m_confirmMutex.unlock();

        if (!m_running) return;

        if (!m_confirmed) {
            emit workerMessage(tr("[Hook] Injection cancelled by user"));
            stop();
            return;
        }

        QThread::msleep(1000);
        emit workerMessage(tr("[Hook] Injecting into \"%1\" (PID: %2)...").arg(m_processName).arg(m_pid));
        const std::string pipeName = "AurexTranslator_" + m_pluginName.toStdString();
        startPipeServer(pipeName);

        startInjectorProcess("load", m_pid, libPath, m_processArch);
    }
}

void PluginWorker::onProcessLost()
{
    {
        QMutexLocker lk(&m_pipeMutex);
        if (m_pipe) m_pipe->close();
    }

    if (m_pipeThread.joinable()) m_pipeThread.join();
    {
        QMutexLocker lk(&m_pipeMutex);
        m_pipe.reset();
    }

    m_processFound = false;
    m_libraryHandle = nullptr;
    m_pid = 0;

    emit processLost();
    emit workerMessage(tr("[Hook] Searching for process: %1").arg(m_processName));
}

void PluginWorker::startPipeServer(const std::string& pipeName)
{
    {
        QMutexLocker lk(&m_pipeMutex);
        m_pipe = std::make_unique<IpcPipe>(pipeName, true);
    }
    m_pipeThread = std::thread([this] { pipeReaderLoop(); });

    QMutexLocker lk(&m_pendingTextMutex);
    m_pendingText.clear();
}

void PluginWorker::pipeReaderLoop()
{
    if (!m_pipe || !m_pipe->waitForConnection()) return;
    while (true) {
        auto msg = m_pipe->receive();
        if (!msg) break;
        handlePipeMessage(*msg);
    }
}

void PluginWorker::handlePipeMessage(const IpcPipe::Message& msg)
{
    switch (msg.type) {
    case MsgType::Status:
        switch (msg.status_code) {
        case StatusCode::Success:
            emit workerMessage(tr("[Hook] Injection succeeded (PID: %1). Awaiting text from game...").arg(m_pid));
            break;
        case StatusCode::Failure:
            emit workerMessage(tr("[Hook] Error (PID: %1): %2").arg(m_pid).arg(QString::fromStdString(msg.text)));
            stop();
            break;
        }
        break;
    case MsgType::Text: {
        //   "Hook"
        //   "Hook:v<N>"
        //   "Hook:<label>"
        //   "Hook:<label>:v<N>"
        const QString label = sanitizeSource(QString::fromStdString(msg.source));

        QString source = QStringLiteral("Hook");
        if (!label.isEmpty())
            source += QStringLiteral(":") + label;
        if (msg.variant != 0)
            source += QStringLiteral(":v%1").arg(msg.variant);
        const QString text = QString::fromStdString(msg.text);

        if (m_streamsCharacters) {
            // Per-character mode: accumulate and let debounceLoop flush the block
            // after flush_interval_ms of silence, keyed by source
            QMutexLocker lk(&m_pendingTextMutex);
            PendingText& pending = m_pendingText[source];
            pending.text += text;
            pending.lastCharTime = std::chrono::steady_clock::now();
        } else {
            // Whole-text mode: each message is already a finished block
            emit currentOutput(source, text);
        }
        break;
    }
    case MsgType::Info:
        emit workerMessage(QString("[Hook] %1").arg(QString::fromStdString(msg.text)));
        break;
    }
}

void PluginWorker::cleanupAndUnload()
{
    if (m_pid > 0 && m_libraryHandle) {
        startInjectorProcess("unload", m_pid, QString(), m_processArch);
    }
#ifdef Q_OS_LINUX
    clearPersistedHandle();
#endif
}

QString PluginWorker::pluginsDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
    + QStringLiteral("/plugins");
}

#ifdef Q_OS_LINUX
void PluginWorker::persistLibraryHandle()
{
    const QString path = pluginsDir() + "/runtime.json";

    QJsonObject root;
    QFile in(path);
    if (in.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(in.readAll()).object();
        in.close();
    }

    QJsonObject entry;
    entry[QStringLiteral("pid")] = static_cast<qint64>(m_pid);
    entry[QStringLiteral("handle")] = QString::asprintf("%p", m_libraryHandle);
    root[m_pluginName] = entry;

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        out.close();
    }
}

void PluginWorker::clearPersistedHandle()
{
    const QString path = pluginsDir() + "/runtime.json";

    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) return;
    QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
    in.close();

    if (!root.contains(m_pluginName)) return;
    root.remove(m_pluginName);

    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        out.close();
    }
}

void* PluginWorker::persistedHandleFor(m_pid_t pid) const
{
    QFile in(pluginsDir() + "/runtime.json");
    if (!in.open(QIODevice::ReadOnly)) return nullptr;
    const QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
    in.close();

    const QJsonObject entry = root.value(m_pluginName).toObject();
    if (entry.isEmpty()) return nullptr;

    if (static_cast<m_pid_t>(entry.value(QStringLiteral("pid")).toInteger()) != pid)
        return nullptr;

    QString hex = entry.value(QStringLiteral("handle")).toString();
    if (hex.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        hex = hex.mid(2);

    bool ok = false;
    const qulonglong addr = hex.toULongLong(&ok, 16);
    return ok ? reinterpret_cast<void*>(addr) : nullptr;
}
#endif // Q_OS_LINUX

void PluginWorker::applyConfigJson(const QString &json)
{
    // Live-tunable settings from the host: currently just flush_interval_ms
    // (the per_char silence window). text_mode is NOT here -- it's a fixed fact
    // passed once as a constructor arg. All aggregation lives here in the host;
    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();

    if (root.contains(QStringLiteral("flush_interval_ms"))) {
        const int val = root.value(QStringLiteral("flush_interval_ms")).toInt(350);
        m_flushIntervalMs.store(qBound(10, val, 60000), std::memory_order_relaxed);
    }
}

void PluginWorker::debounceLoop()
{
    while (m_debounceRunning.load(std::memory_order_relaxed)) {
        QThread::msleep(50);

        const auto now = std::chrono::steady_clock::now();
        const int interval = m_flushIntervalMs.load(std::memory_order_relaxed);

        QMap<QString, QString> ready;
        {
            QMutexLocker lk(&m_pendingTextMutex);
            for (auto it = m_pendingText.begin(); it != m_pendingText.end(); ) {
                const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->lastCharTime).count();
                if (idleMs >= interval) {
                    ready.insert(it.key(), it->text);
                    it = m_pendingText.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto it = ready.begin(); it != ready.end(); ++it)
            emit currentOutput(it.key(), it.value());
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

void PluginWorker::startInjectorProcess(const QString &command, const m_pid_t &pid, const QString &libPath, arch processArch)
{
    const QString archSuffix = (processArch == arch::X86)
    ? QStringLiteral("_x86")
    : QStringLiteral("_x64");

    QString programPath = pluginsDir() + "/bin/at-injector" + archSuffix;

    QProcess *process = new QProcess();
    QObject::connect(process, &QProcess::finished, this, [=](int exitCode, QProcess::ExitStatus status) {
        if (exitCode == 0 && status == QProcess::NormalExit) {
            if (command == "load") {
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
#ifdef Q_OS_LINUX
    persistLibraryHandle();
#endif
}