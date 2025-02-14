/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "vcsplugin.h"

#include "vcsbaseconstants.h"
#include "vcsbasesubmiteditor.h"

#include "commonvcssettings.h"
#include "nicknamedialog.h"
#include "vcsoutputwindow.h"
#include "wizard/vcscommandpage.h"
#include "wizard/vcsconfigurationpage.h"
#include "wizard/vcsjsextension.h"

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/iversioncontrol.h>
#include <coreplugin/jsexpander.h>
#include <coreplugin/vcsmanager.h>

#include <projectexplorer/jsonwizard/jsonwizardfactory.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projecttree.h>

#include <utils/futuresynchronizer.h>
#include <utils/macroexpander.h>

#include <QDebug>

using namespace Core;
using namespace ProjectExplorer;
using namespace Utils;

namespace VcsBase {
namespace Internal {

class VcsPluginPrivate
{
public:
    CommonOptionsPage m_settingsPage;
    QStandardItemModel *m_nickNameModel = nullptr;
    Utils::FutureSynchronizer m_synchronizer;
};

static VcsPlugin *m_instance = nullptr;

VcsPlugin::VcsPlugin()
{
    m_instance = this;
}

VcsPlugin::~VcsPlugin()
{
    d->m_synchronizer.waitForFinished();
    VcsOutputWindow::destroy();
    m_instance = nullptr;
    delete d;
}

bool VcsPlugin::initialize(const QStringList &arguments, QString *errorMessage)
{
    Q_UNUSED(arguments)
    Q_UNUSED(errorMessage)

    d = new VcsPluginPrivate;
    d->m_synchronizer.setCancelOnWait(true);

    EditorManager::addCloseEditorListener([this](IEditor *editor) -> bool {
        bool result = true;
        if (auto se = qobject_cast<VcsBaseSubmitEditor *>(editor))
            emit submitEditorAboutToClose(se, &result);
        return result;
    });

    connect(&d->m_settingsPage, &CommonOptionsPage::settingsChanged,
            this, &VcsPlugin::settingsChanged);
    connect(&d->m_settingsPage, &CommonOptionsPage::settingsChanged,
            this, &VcsPlugin::slotSettingsChanged);
    slotSettingsChanged();

    JsonWizardFactory::registerPageFactory(new Internal::VcsConfigurationPageFactory);
    JsonWizardFactory::registerPageFactory(new Internal::VcsCommandPageFactory);

    JsExpander::registerGlobalObject<VcsJsExtension>("Vcs");

    Utils::MacroExpander *expander = Utils::globalMacroExpander();
    expander->registerVariable(Constants::VAR_VCS_NAME,
        tr("Name of the version control system in use by the current project."),
        []() -> QString {
            IVersionControl *vc = nullptr;
            if (Project *project = ProjectTree::currentProject())
                vc = VcsManager::findVersionControlForDirectory(project->projectDirectory().toString());
            return vc ? vc->displayName() : QString();
        });

    expander->registerVariable(Constants::VAR_VCS_TOPIC,
        tr("The current version control topic (branch or tag) identification of the current project."),
        []() -> QString {
            IVersionControl *vc = nullptr;
            QString topLevel;
            if (Project *project = ProjectTree::currentProject())
                vc = VcsManager::findVersionControlForDirectory(project->projectDirectory().toString(), &topLevel);
            return vc ? vc->vcsTopic(FilePath::fromString(topLevel)) : QString();
        });

    expander->registerVariable(Constants::VAR_VCS_TOPLEVELPATH,
        tr("The top level path to the repository the current project is in."),
        []() -> QString {
            if (Project *project = ProjectTree::currentProject())
                return VcsManager::findTopLevelForDirectory(project->projectDirectory().toString());
            return QString();
        });

    // Just touch VCS Output Pane before initialization
    VcsOutputWindow::instance();

    return true;
}

VcsPlugin *VcsPlugin::instance()
{
    return m_instance;
}

void VcsPlugin::addFuture(const QFuture<void> &future)
{
    m_instance->d->m_synchronizer.addFuture(future);
}

CommonVcsSettings &VcsPlugin::settings() const
{
    return d->m_settingsPage.settings();
}

/* Delayed creation/update of the nick name model. */
QStandardItemModel *VcsPlugin::nickNameModel()
{
    if (!d->m_nickNameModel) {
        d->m_nickNameModel = NickNameDialog::createModel(this);
        populateNickNameModel();
    }
    return d->m_nickNameModel;
}

void VcsPlugin::populateNickNameModel()
{
    QString errorMessage;
    if (!NickNameDialog::populateModelFromMailCapFile(settings().nickNameMailMap.value(),
                                                      d->m_nickNameModel,
                                                      &errorMessage)) {
        qWarning("%s", qPrintable(errorMessage));
    }
}

void VcsPlugin::slotSettingsChanged()
{
    if (d->m_nickNameModel)
        populateNickNameModel();
}

} // namespace Internal
} // namespace VcsBase
