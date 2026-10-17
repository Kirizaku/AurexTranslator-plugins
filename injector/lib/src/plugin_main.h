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

#ifndef AT_INJECTOR_H
#define AT_INJECTOR_H

#include <QObject>
#include <QThread>
#include <QTranslator>

#include "host/plugininterface.h"
#include "plugin_worker.h"

class At_injector : public PluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PluginInterface_iid FILE "../resources/metadata.json")
    Q_INTERFACES(PluginInterface)

public:
    At_injector();
    ~At_injector() override;

    void setLanguage(const QString &languageCode) override;
    QString execute(const QString &command, const QStringList &args = QStringList()) override;

private:
    struct Session {
        PluginWorker *worker = nullptr;
        QThread *thread = nullptr;
    };
    QHash<QString, Session> m_sessions;
    QHash<QString, Session> m_stopping;
    QHash<QString, QStringList> m_pendingStarts;
    QHash<QString, QString> m_pendingConfigs;

    QTranslator *m_translator = nullptr;
    QString m_currentLanguage;

    static QString sessionKey(const QString &pluginName, const QString &processName);
    void beginStop(const QString &key);
    void finishStop(const QString &key);
    void waitStopped();
};

#endif //AT_INJECTOR_H
