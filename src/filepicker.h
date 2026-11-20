#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// Abstract file picker interface so tests can inject a fake picker while the
// desktop app talks to xdg-desktop-portal over D-Bus.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    ~FilePicker() override = default;

    virtual void openImage(const QString &title) = 0;
    virtual void saveFile(const QString &title, const QString &suggestedName, const QString &tag) = 0;

signals:
    void imagePicked(const QString &path);
    void savePathPicked(const QString &path, const QString &tag);
    void failed(const QString &message);
};
