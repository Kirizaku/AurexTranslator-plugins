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

void At_injector::setLanguage(const QString &languageCode)
{
    const QString translationPath = ":/i18n/libat-injector_" + languageCode;

    if (m_translator->load(translationPath)) {
        QCoreApplication::installTranslator(m_translator);
    }
}

QString At_injector::execute(const QString &command, const QStringList &args)
{
    if (command == "start") {
        if (m_worker && m_thread)
            return QString();

        m_worker = new PluginWorker(args);
        m_thread = new QThread;
        m_worker->moveToThread(m_thread);

        connect(m_worker, &PluginWorker::workerMessage, this, &PluginInterface::pluginMessage);
        connect(m_worker, &PluginWorker::currentOutput, this, &PluginInterface::currentOutput);
        connect(m_worker, &PluginWorker::confirmationRequired, this, [=](const QString& message) {
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
            m_worker->onConfirmationResult(confirmed);
            }, Qt::QueuedConnection);

        connect(m_thread, &QThread::started, m_worker, &PluginWorker::run);

        m_thread->start();

        return QString();
    }
    else if (command == "stop") {
        if (m_worker) {
            m_worker->stop();

            cleanupWorker();
            return QString();
        }
    }

    return "Unknown command";
}

void At_injector::cleanupWorker()
{
    m_thread->quit();
    m_thread->wait();

    delete m_worker; m_worker = nullptr;
    delete m_thread; m_thread = nullptr;
}
