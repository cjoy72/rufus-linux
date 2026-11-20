// Portal file picker follows Omacut's xdg-desktop-portal pattern (MIT, David Heinemeier Hansson).
#include "portalfilepicker.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QRandomGenerator>
#include <QUrl>

namespace {

struct PortalFilterRule {
    uint type = 0; // 0 = glob pattern
    QString pattern;
};

struct PortalFilter {
    QString label;
    QList<PortalFilterRule> rules;
};

QDBusArgument &operator<<(QDBusArgument &arg, const PortalFilterRule &rule) {
    arg.beginStructure();
    arg << rule.type << rule.pattern;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalFilterRule &rule) {
    arg.beginStructure();
    arg >> rule.type >> rule.pattern;
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const PortalFilter &filter) {
    arg.beginStructure();
    arg << filter.label << filter.rules;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalFilter &filter) {
    arg.beginStructure();
    arg >> filter.label >> filter.rules;
    arg.endStructure();
    return arg;
}

QString randomToken() {
    return QString("rufus_linux_%1").arg(QRandomGenerator::global()->generate());
}

} // namespace

Q_DECLARE_METATYPE(PortalFilterRule)
Q_DECLARE_METATYPE(QList<PortalFilterRule>)
Q_DECLARE_METATYPE(PortalFilter)
Q_DECLARE_METATYPE(QList<PortalFilter>)

PortalFilePicker::PortalFilePicker(QObject *parent) : FilePicker(parent) {
    qDBusRegisterMetaType<PortalFilterRule>();
    qDBusRegisterMetaType<QList<PortalFilterRule>>();
    qDBusRegisterMetaType<PortalFilter>();
    qDBusRegisterMetaType<QList<PortalFilter>>();
}

void PortalFilePicker::openImage(const QString &title) {
    QDBusInterface portal("org.freedesktop.portal.Desktop",
                          "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.FileChooser",
                          QDBusConnection::sessionBus());
    if (!portal.isValid()) {
        emit failed(tr("Could not connect to xdg-desktop-portal to open a file dialog."));
        return;
    }

    const QList<PortalFilter> filters{
        {tr("Disk and ISO images"),
         {{0, "*.iso"}, {0, "*.ISO"}, {0, "*.img"}, {0, "*.IMG"}, {0, "*.raw"},
          {0, "*.bin"}, {0, "*.gz"}, {0, "*.xz"}, {0, "*.bz2"}, {0, "*.zst"}, {0, "*.zip"}}},
        {tr("All files"), {{0, "*"}}},
    };

    QVariantMap options;
    options.insert("handle_token", randomToken());
    options.insert("multiple", false);
    options.insert("filters", QVariant::fromValue(filters));

    const QDBusReply<QDBusObjectPath> reply = portal.call("OpenFile", QString(), title, options);
    if (!reply.isValid()) {
        emit failed(tr("File dialog failed: %1").arg(reply.error().message()));
        return;
    }

    if (!m_activeOpenPath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(
            "org.freedesktop.portal.Desktop", m_activeOpenPath,
            "org.freedesktop.portal.Request", "Response",
            this, SLOT(onOpenResponse(uint,QVariantMap)));
    }
    m_activeOpenPath = reply.value().path();
    QDBusConnection::sessionBus().connect(
        "org.freedesktop.portal.Desktop", m_activeOpenPath,
        "org.freedesktop.portal.Request", "Response",
        this, SLOT(onOpenResponse(uint,QVariantMap)));
}

void PortalFilePicker::saveFile(const QString &title, const QString &suggestedName, const QString &tag) {
    QDBusInterface portal("org.freedesktop.portal.Desktop",
                          "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.FileChooser",
                          QDBusConnection::sessionBus());
    if (!portal.isValid()) {
        emit failed(tr("Could not connect to xdg-desktop-portal to open a save dialog."));
        return;
    }

    QVariantMap options;
    options.insert("handle_token", randomToken());
    if (!suggestedName.isEmpty())
        options.insert("current_name", suggestedName);

    const QDBusReply<QDBusObjectPath> reply = portal.call("SaveFile", QString(), title, options);
    if (!reply.isValid()) {
        emit failed(tr("Save dialog failed: %1").arg(reply.error().message()));
        return;
    }

    if (!m_activeSavePath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(
            "org.freedesktop.portal.Desktop", m_activeSavePath,
            "org.freedesktop.portal.Request", "Response",
            this, SLOT(onSaveResponse(uint,QVariantMap)));
    }
    m_pendingSaveTag = tag;
    m_activeSavePath = reply.value().path();
    QDBusConnection::sessionBus().connect(
        "org.freedesktop.portal.Desktop", m_activeSavePath,
        "org.freedesktop.portal.Request", "Response",
        this, SLOT(onSaveResponse(uint,QVariantMap)));
}

void PortalFilePicker::onOpenResponse(uint response, const QVariantMap &results) {
    if (!m_activeOpenPath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(
            "org.freedesktop.portal.Desktop", m_activeOpenPath,
            "org.freedesktop.portal.Request", "Response",
            this, SLOT(onOpenResponse(uint,QVariantMap)));
        m_activeOpenPath.clear();
    }
    if (response != 0)
        return;
    const QStringList uris = results.value("uris").toStringList();
    if (uris.isEmpty())
        return;
    const QString localPath = QUrl(uris.first()).toLocalFile();
    if (!localPath.isEmpty())
        emit imagePicked(localPath);
}

void PortalFilePicker::onSaveResponse(uint response, const QVariantMap &results) {
    if (!m_activeSavePath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(
            "org.freedesktop.portal.Desktop", m_activeSavePath,
            "org.freedesktop.portal.Request", "Response",
            this, SLOT(onSaveResponse(uint,QVariantMap)));
        m_activeSavePath.clear();
    }
    if (response != 0)
        return;
    const QStringList uris = results.value("uris").toStringList();
    if (uris.isEmpty())
        return;
    const QString localPath = QUrl(uris.first()).toLocalFile();
    if (!localPath.isEmpty())
        emit savePathPicked(localPath, m_pendingSaveTag);
}
