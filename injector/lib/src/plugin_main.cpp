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

#include "plugin_main.h"
#include <QCoreApplication>
#include <QApplication>
#include <QMessageBox>

At_injector::At_injector() : m_translator(new QTranslator(this)) {}

At_injector::~At_injector()
{
    const QStringList keys = m_sessions.keys();
    for (const QString &key : keys)
        beginStop(key);
    
    waitStopped();
}

void At_injector::setLanguage(const QString &languageCode)
{
    const QString translationPath = ":/i18n/libat-injector_" + languageCode;

    if (m_translator->load(translationPath)) {
        QCoreApplication::installTranslator(m_translator);
    }
}

QString At_injector::sessionKey(const QString &pluginName, const QString &processName)
{
    return pluginName + QStringLiteral("|") + processName;
}

QString At_injector::execute(const QString &command, const QStringList &args)
{
    if (command == "start") {
        if (args.size() < 2) return "Unknown command";
        const QString processName = args.at(0);
        const QString pluginName = args.at(1);
        const QString key = sessionKey(pluginName, processName);

        if (m_sessions.contains(key))
            return QString();

        if (m_stopping.contains(key)) {
            m_pendingStarts.insert(key, args);
            return QString();
        }

        Session session;
        session.worker = new PluginWorker(args);
        session.thread = new QThread;
        session.worker->moveToThread(session.thread);

        connect(session.worker, &PluginWorker::workerMessage, this, &PluginInterface::pluginMessage);
        connect(session.worker, &PluginWorker::currentOutput, this, &PluginInterface::currentOutput);
        connect(session.worker, &PluginWorker::processFound, this, &PluginInterface::processFound);
        connect(session.worker, &PluginWorker::processLost, this, &PluginInterface::processLost);
        connect(session.worker, &PluginWorker::confirmationRequired, this, [worker = QPointer<PluginWorker>(session.worker)](const QString& message) {
            QMessageBox msgBox(nullptr);
            msgBox.setWindowTitle(tr("Injection confirmation"));
            msgBox.setText(message);
            msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
            msgBox.setWindowFlags(msgBox.windowFlags()
                | Qt::WindowStaysOnTopHint
                | Qt::Dialog);
            msgBox.raise();
            msgBox.activateWindow();

            const bool confirmed = msgBox.exec() == QMessageBox::Yes;
            if (worker)
                worker->onConfirmationResult(confirmed);
            }, Qt::QueuedConnection);

        connect(session.thread, &QThread::started, session.worker, &PluginWorker::run);

        m_sessions.insert(key, session);
        session.thread->start();

        return QString();
    }
    else if (command == "stop") {
        if (args.isEmpty()) return "Unknown command";
        const QString pluginName = args.at(0);

        if (args.size() >= 2) {
            beginStop(sessionKey(pluginName, args.at(1)));
            return QString();
        }

        const QString prefix = pluginName + QStringLiteral("|");
        const QStringList keys = m_sessions.keys() + m_pendingStarts.keys();
        for (const QString &key : keys) {
            if (key.startsWith(prefix))
                beginStop(key);
        }
        return QString();
    }
    else if (command == "wait-stopped") {
        waitStopped();
        return QString();
    }
#ifdef Q_OS_LINUX
    else if (command == "ptrace-status") {
        switch (PluginWorker::ptraceStatus()) {
        case PluginWorker::PtraceStatus::Granted: return QStringLiteral("granted");
        case PluginWorker::PtraceStatus::Missing: return QStringLiteral("missing");
        default:                                  return QStringLiteral("unavailable");
        }
    }
    else if (command == "ptrace-grant") {
        const QString error = PluginWorker::grantPtrace();
        return error.isEmpty() ? QStringLiteral("ok") : error;
    }
#endif
    else if (command == "config") {
        if (args.size() >= 2) {
            const QString prefix = args.at(0) + QStringLiteral("|");
            const QStringList keys = m_sessions.keys();
            for (const QString &key : keys) {
                if (key.startsWith(prefix))
                    m_sessions.value(key).worker->setConfig(args.at(1));
            }
            const QStringList pendingKeys = m_pendingStarts.keys();
            for (const QString &key : pendingKeys) {
                if (key.startsWith(prefix))
                    m_pendingConfigs.insert(key, args.at(1));
            }
        }
        return QString();
    }

    return "Unknown command";
}

void At_injector::beginStop(const QString &key)
{
    m_pendingStarts.remove(key);
    m_pendingConfigs.remove(key);

    auto it = m_sessions.find(key);
    if (it == m_sessions.end()) return;

    Session session = it.value();
    m_sessions.erase(it);
    m_stopping.insert(key, session);

    session.worker->stop();
    connect(session.thread, &QThread::finished, this, [this, key, thread = session.thread] {
        const auto stopping = m_stopping.constFind(key);
        if (stopping != m_stopping.constEnd() && stopping->thread == thread)
            finishStop(key);
    });
    session.thread->quit();
}

void At_injector::finishStop(const QString &key)
{
    auto it = m_stopping.find(key);
    if (it == m_stopping.end()) return;

    Session session = it.value();
    m_stopping.erase(it);

    session.thread->wait();
    delete session.worker;
    delete session.thread;

    const auto pending = m_pendingStarts.find(key);
    if (pending == m_pendingStarts.end()) return;

    const QStringList args = pending.value();
    m_pendingStarts.erase(pending);
    const QString config = m_pendingConfigs.take(key);

    execute(QStringLiteral("start"), args);
    if (!config.isEmpty()) {
        const auto started = m_sessions.constFind(key);
        if (started != m_sessions.constEnd())
            started->worker->setConfig(config);
    }
}

void At_injector::waitStopped()
{
    m_pendingStarts.clear();
    m_pendingConfigs.clear();

    const QStringList keys = m_stopping.keys();
    for (const QString &key : keys)
        finishStop(key);
}
