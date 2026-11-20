#pragma once

#include <QJsonObject>
#include <QString>

// Windows User Experience options ported from Pete Batard's Rufus (src/wue.c).
struct WindowsUnattendOptions {
    bool enabled = false;
    QString arch = "amd64"; // "amd64", "arm64", "x86"
    bool bypassTpmSecureBootRam = true;
    bool bypassOnlineAccount = true;
    bool createLocalAccount = false;
    QString localUsername;
    bool disableDataCollection = true;
    bool disableBitLocker = false;
    bool qolEnhancements = false;

    bool hasAnyOption() const;
    QJsonObject toJson() const;
    static WindowsUnattendOptions fromJson(const QJsonObject &obj);
};

class UnattendGenerator {
public:
    // Sanitizes a Windows local account name using Rufus's invalid-character
    // filter and reserved-account rejection list. Returns empty if disallowed.
    static QString sanitizeUsername(const QString &raw);

    // Generates a complete autounattend.xml document, or empty string if no
    // options are active.
    static QString generate(const WindowsUnattendOptions &options);
};
