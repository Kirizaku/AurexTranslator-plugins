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

#include "plugininterface.h"
#include "plugin_worker.h"

class At_injector : public PluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PluginInterface_iid FILE "../resources/metadata.json")
    Q_INTERFACES(PluginInterface)

public:
    At_injector();

    void setLanguage(const QString &languageCode) override;
    QString execute(const QString &command, const QStringList &args = QStringList()) override;

private:
    PluginWorker *m_worker = nullptr;
    QThread *m_thread = nullptr;

    QTranslator *m_translator = nullptr;
    QString m_currentLanguage;

    void cleanupWorker();
};

#endif //AT_INJECTOR_H
