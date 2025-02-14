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

#include "vcsbaseplugin.h"
#include "vcsbasesubmiteditor.h"
#include "vcsplugin.h"
#include "commonvcssettings.h"
#include "vcsoutputwindow.h"
#include "vcscommand.h"

#include <coreplugin/documentmanager.h>
#include <coreplugin/icore.h>
#include <coreplugin/idocument.h>
#include <coreplugin/editormanager/editormanager.h>
#include <projectexplorer/projecttree.h>
#include <projectexplorer/project.h>
#include <projectexplorer/session.h>
#include <utils/qtcassert.h>
#include <utils/qtcprocess.h>

#include <QDebug>
#include <QDir>
#include <QLoggingCategory>
#include <QSharedData>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QProcessEnvironment>
#include <QTextCodec>

#include <QAction>
#include <QMessageBox>
#include <QFileDialog>

using namespace Core;
using namespace Utils;
using namespace ProjectExplorer;

namespace {
static Q_LOGGING_CATEGORY(baseLog, "qtc.vcs.base", QtWarningMsg)
static Q_LOGGING_CATEGORY(findRepoLog, "qtc.vcs.find-repo", QtWarningMsg)
static Q_LOGGING_CATEGORY(stateLog, "qtc.vcs.state", QtWarningMsg)
}

/*!
    \namespace VcsBase
    \brief The VcsBase namespace contains classes for the VcsBase plugin.
*/

/*!
    \namespace VcsBase::Internal
    \brief The Internal namespace contains internal classes for the VcsBase
    plugin.
    \internal
*/

namespace VcsBase {
namespace Internal {

/*!
    \class VcsBase::Internal::State

    \brief The State class provides the internal state created by the state
    listener and VcsBasePluginState.

    Aggregated in the QSharedData of VcsBase::VcsBasePluginState.
*/

class State
{
public:
    void clearFile();
    void clearPatchFile();
    void clearProject();
    inline void clear();

    bool equals(const State &rhs) const;

    inline bool hasFile() const     { return !currentFileTopLevel.isEmpty(); }
    inline bool hasProject() const  { return !currentProjectTopLevel.isEmpty(); }
    inline bool isEmpty() const     { return !hasFile() && !hasProject(); }

    QString currentFile;
    QString currentFileName;
    QString currentPatchFile;
    QString currentPatchFileDisplayName;

    QString currentFileDirectory;
    QString currentFileTopLevel;

    QString currentProjectPath;
    QString currentProjectName;
    QString currentProjectTopLevel;
};

void State::clearFile()
{
    currentFile.clear();
    currentFileName.clear();
    currentFileDirectory.clear();
    currentFileTopLevel.clear();
}

void State::clearPatchFile()
{
    currentPatchFile.clear();
    currentPatchFileDisplayName.clear();
}

void State::clearProject()
{
    currentProjectPath.clear();
    currentProjectName.clear();
    currentProjectTopLevel.clear();
}

void State::clear()
{
    clearFile();
    clearPatchFile();
    clearProject();
}

bool State::equals(const State &rhs) const
{
    return currentFile == rhs.currentFile
            && currentFileName == rhs.currentFileName
            && currentPatchFile == rhs.currentPatchFile
            && currentPatchFileDisplayName == rhs.currentPatchFileDisplayName
            && currentFileTopLevel == rhs.currentFileTopLevel
            && currentProjectPath == rhs.currentProjectPath
            && currentProjectName == rhs.currentProjectName
            && currentProjectTopLevel == rhs.currentProjectTopLevel;
}

QDebug operator<<(QDebug in, const State &state)
{
    QDebug nospace = in.nospace();
    nospace << "State: ";
    if (state.isEmpty()) {
        nospace << "<empty>";
    } else {
        if (state.hasFile()) {
            nospace << "File=" << state.currentFile
                    << ',' << state.currentFileTopLevel;
        } else {
            nospace << "<no file>";
        }
        nospace << '\n';
        if (state.hasProject()) {
            nospace << "       Project=" << state.currentProjectName
            << ',' << state.currentProjectPath
            << ',' << state.currentProjectTopLevel;

        } else {
            nospace << "<no project>";
        }
        nospace << '\n';
    }
    return in;
}

/*!
    \class VcsBase::Internal::StateListener

    \brief The StateListener class connects to the relevant signals of \QC,
    tries to find version
    controls and emits signals to the plugin instances.

    Singleton (as not to do checks multiple times).
*/

class StateListener : public QObject
{
    Q_OBJECT

public:
    explicit StateListener(QObject *parent);

    static QString windowTitleVcsTopic(const QString &filePath);

signals:
    void stateChanged(const VcsBase::Internal::State &s, IVersionControl *vc);

public slots:
    void slotStateChanged();
};

StateListener::StateListener(QObject *parent) : QObject(parent)
{
    connect(EditorManager::instance(), &EditorManager::currentEditorChanged,
            this, &StateListener::slotStateChanged);
    connect(EditorManager::instance(), &EditorManager::currentDocumentStateChanged,
            this, &StateListener::slotStateChanged);
    connect(VcsManager::instance(), &VcsManager::repositoryChanged,
            this, &StateListener::slotStateChanged);

    connect(ProjectTree::instance(), &ProjectTree::currentProjectChanged,
            this, &StateListener::slotStateChanged);
    connect(SessionManager::instance(), &SessionManager::startupProjectChanged,
            this, &StateListener::slotStateChanged);


    EditorManager::setWindowTitleVcsTopicHandler(&StateListener::windowTitleVcsTopic);
}

QString StateListener::windowTitleVcsTopic(const QString &filePath)
{
    QString searchPath;
    if (!filePath.isEmpty()) {
        searchPath = QFileInfo(filePath).absolutePath();
    } else {
        // use single project's information if there is only one loaded.
        const QList<Project *> projects = SessionManager::projects();
        if (projects.size() == 1)
            searchPath = projects.first()->projectDirectory().toString();
    }
    if (searchPath.isEmpty())
        return QString();
    QString topLevelPath;
    IVersionControl *vc = VcsManager::findVersionControlForDirectory(
                searchPath, &topLevelPath);
    return (vc && !topLevelPath.isEmpty()) ? vc->vcsTopic(FilePath::fromString(topLevelPath)) : QString();
}

static inline QString displayNameOfEditor(const QString &fileName)
{
    IDocument *document = DocumentModel::documentForFilePath(FilePath::fromString(fileName));
    if (document)
        return document->displayName();
    return QString();
}

void StateListener::slotStateChanged()
{
    // Get the current file. Are we on a temporary submit editor indicated by
    // temporary path prefix or does the file contains a hash, indicating a project
    // folder?
    State state;
    IDocument *currentDocument = EditorManager::currentDocument();
    if (currentDocument) {
        state.currentFile = currentDocument->filePath().toString();
        if (state.currentFile.isEmpty() || currentDocument->isTemporary())
            state.currentFile = VcsBase::source(currentDocument);
    }

    // Get the file and its control. Do not use the file unless we find one
    IVersionControl *fileControl = nullptr;

    if (!state.currentFile.isEmpty()) {
        QFileInfo currentFi(state.currentFile);

        if (currentFi.exists()) {
            // Quick check: Does it look like a patch?
            const bool isPatch = state.currentFile.endsWith(".patch")
                    || state.currentFile.endsWith(".diff");
            if (isPatch) {
                // Patch: Figure out a name to display. If it is a temp file, it could be
                // Codepaster. Use the display name of the editor.
                state.currentPatchFile = state.currentFile;
                state.currentPatchFileDisplayName = displayNameOfEditor(state.currentPatchFile);
                if (state.currentPatchFileDisplayName.isEmpty())
                    state.currentPatchFileDisplayName = currentFi.fileName();
            }

            if (currentFi.isDir()) {
                state.currentFile.clear();
                state.currentFileDirectory = currentFi.absoluteFilePath();
            } else {
                state.currentFileDirectory = currentFi.absolutePath();
                state.currentFileName = currentFi.fileName();
            }
            fileControl = VcsManager::findVersionControlForDirectory(state.currentFileDirectory,
                                                                     &state.currentFileTopLevel);
        }

        if (!fileControl)
            state.clearFile();
    }

    // Check for project, find the control
    IVersionControl *projectControl = nullptr;
    Project *currentProject = ProjectTree::currentProject();
    if (!currentProject)
        currentProject = SessionManager::startupProject();

    if (currentProject) {
        state.currentProjectPath = currentProject->projectDirectory().toString();
        state.currentProjectName = currentProject->displayName();
        projectControl = VcsManager::findVersionControlForDirectory(state.currentProjectPath,
                                                                    &state.currentProjectTopLevel);
        if (projectControl) {
            // If we have both, let the file's one take preference
            if (fileControl && projectControl != fileControl)
                state.clearProject();
        } else {
            state.clearProject(); // No control found
        }
    }

    // Assemble state and emit signal.
    IVersionControl *vc = fileControl;
    if (!vc)
        vc = projectControl;
    if (!vc)
        state.clearPatchFile(); // Need a repository to patch

    qCDebug(stateLog).noquote() << "VC:" << (vc ? vc->displayName() : QString("None")) << state;
    EditorManager::updateWindowTitles();
    emit stateChanged(state, vc);
}

} // namespace Internal

class VcsBasePluginStateData : public QSharedData
{
public:
    Internal::State m_state;
};

/*!
    \class  VcsBase::VcsBasePluginState

    \brief The VcsBasePluginState class provides relevant state information
    about the VCS plugins.

    Qt Creator's state relevant to VCS plugins is a tuple of

    \list
    \li Current file and it's version system control/top level
    \li Current project and it's version system control/top level
    \endlist

    \sa VcsBase::VcsBasePlugin
*/

VcsBasePluginState::VcsBasePluginState() : data(new VcsBasePluginStateData)
{ }


VcsBasePluginState::VcsBasePluginState(const VcsBasePluginState &rhs) : data(rhs.data)
{ }

VcsBasePluginState::~VcsBasePluginState() = default;

VcsBasePluginState &VcsBasePluginState::operator=(const VcsBasePluginState &rhs)
{
    if (this != &rhs)
        data.operator=(rhs.data);
    return *this;
}

QString VcsBasePluginState::currentFile() const
{
    return data->m_state.currentFile;
}

QString VcsBasePluginState::currentFileName() const
{
    return data->m_state.currentFileName;
}

QString VcsBasePluginState::currentFileTopLevel() const
{
    return data->m_state.currentFileTopLevel;
}

QString VcsBasePluginState::currentFileDirectory() const
{
    return data->m_state.currentFileDirectory;
}

QString VcsBasePluginState::relativeCurrentFile() const
{
    QTC_ASSERT(hasFile(), return QString());
    return QDir(data->m_state.currentFileTopLevel).relativeFilePath(data->m_state.currentFile);
}

QString VcsBasePluginState::currentPatchFile() const
{
    return data->m_state.currentPatchFile;
}

QString VcsBasePluginState::currentPatchFileDisplayName() const
{
    return data->m_state.currentPatchFileDisplayName;
}

QString VcsBasePluginState::currentProjectPath() const
{
    return data->m_state.currentProjectPath;
}

QString VcsBasePluginState::currentProjectName() const
{
    return data->m_state.currentProjectName;
}

QString VcsBasePluginState::currentProjectTopLevel() const
{
    return data->m_state.currentProjectTopLevel;
}

QString VcsBasePluginState::relativeCurrentProject() const
{
    QTC_ASSERT(hasProject(), return QString());
    if (data->m_state.currentProjectTopLevel != data->m_state.currentProjectPath)
        return QDir(data->m_state.currentProjectTopLevel).relativeFilePath(data->m_state.currentProjectPath);
    return QString();
}

bool VcsBasePluginState::hasTopLevel() const
{
    return data->m_state.hasFile() || data->m_state.hasProject();
}

QString VcsBasePluginState::topLevel() const
{
    return hasFile() ? data->m_state.currentFileTopLevel : data->m_state.currentProjectTopLevel;
}

bool VcsBasePluginState::equals(const Internal::State &rhs) const
{
    return data->m_state.equals(rhs);
}

bool VcsBasePluginState::equals(const VcsBasePluginState &rhs) const
{
    return equals(rhs.data->m_state);
}

void VcsBasePluginState::clear()
{
    data->m_state.clear();
}

void VcsBasePluginState::setState(const Internal::State &s)
{
    data->m_state = s;
}

bool VcsBasePluginState::isEmpty() const
{
    return data->m_state.isEmpty();
}

bool VcsBasePluginState::hasFile() const
{
    return data->m_state.hasFile();
}

bool VcsBasePluginState::hasPatchFile() const
{
    return !data->m_state.currentPatchFile.isEmpty();
}

bool VcsBasePluginState::hasProject() const
{
    return data->m_state.hasProject();
}

VCSBASE_EXPORT QDebug operator<<(QDebug in, const VcsBasePluginState &state)
{
    in << state.data->m_state;
    return in;
}

/*!
    \class VcsBase::VcsBasePlugin

    \brief The VcsBasePlugin class is the base class for all version control
    plugins.

    The plugin connects to the
    relevant change signals in Qt Creator and calls the virtual
    updateActions() for the plugins to update their menu actions
    according to the new state. This is done centrally to avoid
    single plugins repeatedly invoking searches/QFileInfo on files,
    etc.

    Independently, there are accessors for current patch files, which return
    a file name if the current file could be a patch file which could be applied
    and a repository exists.

    If current file/project are managed
    by different version controls, the project is discarded and only
    the current file is taken into account, allowing to do a diff
    also when the project of a file is not opened.

    When triggering an action, a copy of the state should be made to
    keep it, as it may rapidly change due to context changes, etc.

    The class also detects the VCS plugin submit editor closing and calls
    the virtual submitEditorAboutToClose() to trigger the submit process.
*/

bool VcsBasePluginPrivate::supportsRepositoryCreation() const
{
    return supportsOperation(IVersionControl::CreateRepositoryOperation);
}

static Internal::StateListener *m_listener = nullptr;

VcsBasePluginPrivate::VcsBasePluginPrivate(const Context &context)
    : m_context(context)
{
    Internal::VcsPlugin *plugin = Internal::VcsPlugin::instance();
    connect(plugin, &Internal::VcsPlugin::submitEditorAboutToClose,
            this, &VcsBasePluginPrivate::slotSubmitEditorAboutToClose);
    // First time: create new listener
    if (!m_listener)
        m_listener = new Internal::StateListener(plugin);
    connect(m_listener, &Internal::StateListener::stateChanged,
            this, &VcsBasePluginPrivate::slotStateChanged);
    // VCSes might have become (un-)available, so clear the VCS directory cache
    connect(this, &IVersionControl::configurationChanged,
            VcsManager::instance(), &VcsManager::clearVersionControlCache);
    connect(this, &IVersionControl::configurationChanged,
            m_listener, &Internal::StateListener::slotStateChanged);
}

void VcsBasePluginPrivate::extensionsInitialized()
{
    // Initialize enable menus.
    m_listener->slotStateChanged();
}

void VcsBasePluginPrivate::slotSubmitEditorAboutToClose(VcsBaseSubmitEditor *submitEditor, bool *result)
{
    qCDebug(baseLog) << this << "plugin's submit editor" << m_submitEditor
                     << (m_submitEditor ? m_submitEditor->document()->id().name() : QByteArray())
                     << "closing submit editor" << submitEditor
                     << (submitEditor ? submitEditor->document()->id().name() : QByteArray());
    if (submitEditor == m_submitEditor)
        *result = submitEditorAboutToClose();
}

void VcsBasePluginPrivate::slotStateChanged(const Internal::State &newInternalState, Core::IVersionControl *vc)
{
    if (vc == this) {
        // We are directly affected: Change state
        if (!m_state.equals(newInternalState)) {
            m_state.setState(newInternalState);
            updateActions(VcsEnabled);
            ICore::addAdditionalContext(m_context);
        }
    } else {
        // Some other VCS plugin or state changed: Reset us to empty state.
        const ActionState newActionState = vc ? OtherVcsEnabled : NoVcsEnabled;
        if (m_actionState != newActionState || !m_state.isEmpty()) {
            m_actionState = newActionState;
            const VcsBasePluginState emptyState;
            m_state = emptyState;
            updateActions(newActionState);
        }
        ICore::removeAdditionalContext(m_context);
    }
}

const VcsBasePluginState &VcsBasePluginPrivate::currentState() const
{
    return m_state;
}

bool VcsBasePluginPrivate::enableMenuAction(ActionState as, QAction *menuAction) const
{
    qCDebug(baseLog) << "enableMenuAction" << menuAction->text() << as;
    switch (as) {
    case NoVcsEnabled: {
        const bool supportsCreation = supportsRepositoryCreation();
        menuAction->setVisible(supportsCreation);
        menuAction->setEnabled(supportsCreation);
        return supportsCreation;
    }
    case OtherVcsEnabled:
        menuAction->setVisible(false);
        return false;
    case VcsEnabled:
        menuAction->setVisible(true);
        menuAction->setEnabled(true);
        break;
    }
    return true;
}

QString VcsBasePluginPrivate::commitDisplayName() const
{
    return tr("Commit", "name of \"commit\" action of the VCS.");
}

bool VcsBasePluginPrivate::promptBeforeCommit()
{
    return DocumentManager::saveAllModifiedDocuments(tr("Save before %1?")
                                                     .arg(commitDisplayName().toLower()));
}

void VcsBasePluginPrivate::promptToDeleteCurrentFile()
{
    const VcsBasePluginState state = currentState();
    QTC_ASSERT(state.hasFile(), return);
    const bool rc = VcsManager::promptToDelete(this, state.currentFile());
    if (!rc)
        QMessageBox::warning(ICore::dialogParent(), tr("Version Control"),
                             tr("The file \"%1\" could not be deleted.").
                             arg(QDir::toNativeSeparators(state.currentFile())),
                             QMessageBox::Ok);
}

static inline bool ask(QWidget *parent, const QString &title, const QString &question, bool defaultValue = true)

{
    const QMessageBox::StandardButton defaultButton = defaultValue ? QMessageBox::Yes : QMessageBox::No;
    return QMessageBox::question(parent, title, question, QMessageBox::Yes|QMessageBox::No, defaultButton) == QMessageBox::Yes;
}

void VcsBasePluginPrivate::createRepository()
{
    QTC_ASSERT(supportsOperation(IVersionControl::CreateRepositoryOperation), return);
    // Find current starting directory
    QString directory;
    if (const Project *currentProject = ProjectTree::currentProject())
        directory = currentProject->projectFilePath().absolutePath().toString();
    // Prompt for a directory that is not under version control yet
    QWidget *mw = ICore::dialogParent();
    do {
        directory = QFileDialog::getExistingDirectory(mw, tr("Choose Repository Directory"), directory);
        if (directory.isEmpty())
            return;
        const IVersionControl *managingControl = VcsManager::findVersionControlForDirectory(directory);
        if (managingControl == nullptr)
            break;
        const QString question = tr("The directory \"%1\" is already managed by a version control system (%2)."
                                    " Would you like to specify another directory?").arg(directory, managingControl->displayName());

        if (!ask(mw, tr("Repository already under version control"), question))
            return;
    } while (true);
    // Create
    const bool rc = vcsCreateRepository(FilePath::fromString(directory));
    const QString nativeDir = QDir::toNativeSeparators(directory);
    if (rc) {
        QMessageBox::information(mw, tr("Repository Created"),
                                 tr("A version control repository has been created in %1.").
                                 arg(nativeDir));
    } else {
        QMessageBox::warning(mw, tr("Repository Creation Failed"),
                                 tr("A version control repository could not be created in %1.").
                                 arg(nativeDir));
    }
}

void VcsBasePluginPrivate::setSubmitEditor(VcsBaseSubmitEditor *submitEditor)
{
    m_submitEditor = submitEditor;
}

VcsBaseSubmitEditor *VcsBasePluginPrivate::submitEditor() const
{
    return m_submitEditor;
}

bool VcsBasePluginPrivate::raiseSubmitEditor() const
{
    if (!m_submitEditor)
        return false;
    EditorManager::activateEditor(m_submitEditor, EditorManager::IgnoreNavigationHistory);
    return true;
}

// Find top level for version controls like git/Mercurial that have
// a directory at the top of the repository.
// Note that checking for the existence of files is preferred over directories
// since checking for directories can cause them to be created when
// AutoFS is used (due its automatically creating mountpoints when querying
// a directory). In addition, bail out when reaching the home directory
// of the user or root (generally avoid '/', where mountpoints are created).
FilePath findRepositoryForFile(const FilePath &fileOrDir, const QString &checkFile)
{
    const FilePath dirS = fileOrDir.isDir() ? fileOrDir : fileOrDir.parentDir();
    qCDebug(findRepoLog) << ">" << dirS << checkFile;
    QTC_ASSERT(!dirS.isEmpty() && !checkFile.isEmpty(), return {});

    const QString root = QDir::rootPath();
    const QString home = QDir::homePath();

    QDir directory(dirS.toString());
    do {
        const QString absDirPath = directory.absolutePath();
        if (absDirPath == root || absDirPath == home)
            break;

        if (QFileInfo(directory, checkFile).isFile()) {
            qCDebug(findRepoLog) << "<" << absDirPath;
            return FilePath::fromString(absDirPath);
        }
    } while (!directory.isRoot() && directory.cdUp());
    qCDebug(findRepoLog) << "< bailing out at" << directory.absolutePath();
    return {};
}

// Is SSH prompt configured?
QString sshPrompt()
{
    return Internal::VcsPlugin::instance()->settings().sshPasswordPrompt.value();
}

bool isSshPromptConfigured()
{
    return !sshPrompt().isEmpty();
}

static const char SOURCE_PROPERTY[] = "qtcreator_source";

void setSource(IDocument *document, const QString &source)
{
    document->setProperty(SOURCE_PROPERTY, source);
    m_listener->slotStateChanged();
}

QString source(IDocument *document)
{
    return document->property(SOURCE_PROPERTY).toString();
}

void setProcessEnvironment(Environment *e, bool forceCLocale, const QString &sshPromptBinary)
{
    if (forceCLocale) {
        e->set("LANG", "C");
        e->set("LANGUAGE", "C");
    }
    if (!sshPromptBinary.isEmpty())
        e->set("SSH_ASKPASS", sshPromptBinary);
}

} // namespace VcsBase

#include "vcsbaseplugin.moc"
