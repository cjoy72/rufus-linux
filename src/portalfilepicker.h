#pragma once

#include "filepicker.h"

#include <QDBusObjectPath>
#include <QVariantMap>

// Talks to org.freedesktop.portal.FileChooser over the session D-Bus, following
// Omacut's portal file picker pattern so Wayland apps never need QFileDialog.
class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    void openImage(const QString &title) override;
    void saveFile(const QString &title, const QString &suggestedName, const QString &tag) override;

private slots:
    void onOpenResponse(uint response, const QVariantMap &results);
    void onSaveResponse(uint response, const QVariantMap &results);

private:
    QString m_activeOpenPath;
    QString m_activeSavePath;
    QString m_pendingSaveTag;
};
