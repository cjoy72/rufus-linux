// Ported from Rufus src/wue.c (Copyright (C) 2022-2026 Pete Batard, GPLv3).
#include "unattend.h"

#include <QStringList>

namespace {

// Reserved Windows account names from Rufus src/wue.c.
bool isDisallowedUsername(const QString &name) {
    static const QStringList kDisallowed = {
        "Administrator", "Järjestelmänvalvoja", "Administrateur", "Rendszergazda",
        "Administrador", "Администратор", "Administratör", "Guest",
        "DefaultAccount", "WDAGUtilityAccount", "HelpAssistant", "KRBTGT",
        "Local", "NONE", "SYSTEM"
    };
    for (const QString &entry : kDisallowed) {
        if (name.compare(entry, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QString xmlEscape(const QString &text) {
    QString out = text;
    out.replace('&', "&amp;");
    out.replace('<', "&lt;");
    out.replace('>', "&gt;");
    out.replace('"', "&quot;");
    out.replace('\'', "&apos;");
    return out;
}

} // namespace

bool WindowsUnattendOptions::hasAnyOption() const {
    if (!enabled)
        return false;
    return bypassTpmSecureBootRam || bypassOnlineAccount ||
           (createLocalAccount && !UnattendGenerator::sanitizeUsername(localUsername).isEmpty()) ||
           disableDataCollection || disableBitLocker || qolEnhancements;
}

QJsonObject WindowsUnattendOptions::toJson() const {
    return QJsonObject{
        {"enabled", enabled},
        {"arch", arch},
        {"bypassTpmSecureBootRam", bypassTpmSecureBootRam},
        {"bypassOnlineAccount", bypassOnlineAccount},
        {"createLocalAccount", createLocalAccount},
        {"localUsername", localUsername},
        {"disableDataCollection", disableDataCollection},
        {"disableBitLocker", disableBitLocker},
        {"qolEnhancements", qolEnhancements},
    };
}

WindowsUnattendOptions WindowsUnattendOptions::fromJson(const QJsonObject &obj) {
    WindowsUnattendOptions opts;
    opts.enabled = obj.value("enabled").toBool(false);
    opts.arch = obj.value("arch").toString("amd64");
    opts.bypassTpmSecureBootRam = obj.value("bypassTpmSecureBootRam").toBool(false);
    opts.bypassOnlineAccount = obj.value("bypassOnlineAccount").toBool(false);
    opts.createLocalAccount = obj.value("createLocalAccount").toBool(false);
    opts.localUsername = obj.value("localUsername").toString();
    opts.disableDataCollection = obj.value("disableDataCollection").toBool(false);
    opts.disableBitLocker = obj.value("disableBitLocker").toBool(false);
    opts.qolEnhancements = obj.value("qolEnhancements").toBool(false);
    return opts;
}

QString UnattendGenerator::sanitizeUsername(const QString &raw) {
    QString name = raw.trimmed();
    if (name.isEmpty() || isDisallowedUsername(name))
        return {};

    // USERNAME_INVALID_CHARS from Rufus src/rufus.h.
    static const QString kInvalid = R"(/\[]:;|=,+*?<>%@&")";
    for (QChar &ch : name) {
        if (kInvalid.contains(ch))
            ch = '_';
    }
    name = name.left(20).trimmed();
    if (name.isEmpty() || isDisallowedUsername(name))
        return {};
    return name;
}

QString UnattendGenerator::generate(const WindowsUnattendOptions &options) {
    if (!options.hasAnyOption())
        return {};

    QString arch = options.arch.toLower();
    if (arch != "amd64" && arch != "arm64" && arch != "x86")
        arch = "amd64";

    const QString safeUser = options.createLocalAccount ? sanitizeUsername(options.localUsername) : QString();

    QString xml;
    xml += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    xml += "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n";

    // Pass 1: windowsPE (TPM/SecureBoot/RAM bypass + BitLocker disable).
    if (options.bypassTpmSecureBootRam || options.disableBitLocker) {
        xml += "  <settings pass=\"windowsPE\">\n";
        xml += QString("    <component name=\"Microsoft-Windows-Setup\" processorArchitecture=\"%1\" language=\"neutral\" "
                       "xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" "
                       "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                       "publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n").arg(arch);
        xml += "      <UserData>\n";
        xml += "        <AcceptEula>true</AcceptEula>\n";
        xml += "        <ProductKey>\n";
        xml += "          <Key />\n";
        xml += "        </ProductKey>\n";
        xml += "      </UserData>\n";

        if (options.disableBitLocker) {
            xml += "      <DiskConfiguration>\n";
            xml += "        <DisableEncryptedDiskProvisioning>true</DisableEncryptedDiskProvisioning>\n";
            xml += "      </DiskConfiguration>\n";
        }

        if (options.bypassTpmSecureBootRam) {
            static const QStringList kBypasses = {
                "BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck"
            };
            xml += "      <RunSynchronous>\n";
            int order = 1;
            for (const QString &key : kBypasses) {
                xml += "        <RunSynchronousCommand wcm:action=\"add\">\n";
                xml += QString("          <Order>%1</Order>\n").arg(order++);
                xml += QString("          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v %1 /t REG_DWORD /d 1 /f</Path>\n").arg(key);
                xml += "        </RunSynchronousCommand>\n";
            }
            xml += "      </RunSynchronous>\n";
        }

        xml += "    </component>\n";
        xml += "  </settings>\n";
    }

    // Pass 2: specialize (BypassNRO + QoL OneDrive/Outlook/Teams removal).
    if (options.bypassOnlineAccount || options.qolEnhancements) {
        QStringList commands;
        if (options.bypassOnlineAccount) {
            commands << R"(reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\OOBE" /v BypassNRO /t REG_DWORD /d 1 /f)";
        }
        if (options.qolEnhancements) {
            commands << R"(reg add "HKLM\Software\Policies\Microsoft\Windows\OneDrive" /v DisableFileSyncNGSC /t REG_DWORD /d 1 /f)";
            commands << R"(PowerShell -NonInteractive -WindowStyle Hidden -Command "Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Teams*'} | Remove-AppxProvisionedPackage -Online")";
        }

        xml += "  <settings pass=\"specialize\">\n";
        xml += QString("    <component name=\"Microsoft-Windows-Deployment\" processorArchitecture=\"%1\" language=\"neutral\" "
                       "xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" "
                       "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                       "publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n").arg(arch);
        xml += "      <RunSynchronous>\n";
        int order = 1;
        for (const QString &cmd : commands) {
            xml += "        <RunSynchronousCommand wcm:action=\"add\">\n";
            xml += QString("          <Order>%1</Order>\n").arg(order++);
            xml += QString("          <Path>%1</Path>\n").arg(xmlEscape(cmd));
            xml += "        </RunSynchronousCommand>\n";
        }
        xml += "      </RunSynchronous>\n";
        xml += "    </component>\n";
        xml += "  </settings>\n";
    }

    // Pass 3: oobeSystem (Privacy, Local Account, QoL tweaks).
    if (options.disableDataCollection || !safeUser.isEmpty() || options.qolEnhancements) {
        QStringList firstLogonCommands;
        xml += "  <settings pass=\"oobeSystem\">\n";
        xml += QString("    <component name=\"Microsoft-Windows-Shell-Setup\" processorArchitecture=\"%1\" language=\"neutral\" "
                       "xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" "
                       "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                       "publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n").arg(arch);

        if (options.disableDataCollection) {
            xml += "      <OOBE>\n";
            xml += "        <HideEULAPage>true</HideEULAPage>\n";
            xml += "        <ProtectYourPC>3</ProtectYourPC>\n";
            xml += "      </OOBE>\n";
        }

        if (!safeUser.isEmpty()) {
            const QString escapedUser = xmlEscape(safeUser);
            xml += "      <UserAccounts>\n";
            xml += "        <LocalAccounts>\n";
            xml += "          <LocalAccount wcm:action=\"add\">\n";
            xml += QString("            <Name>%1</Name>\n").arg(escapedUser);
            xml += QString("            <DisplayName>%1</DisplayName>\n").arg(escapedUser);
            xml += "            <Group>Administrators;Power Users</Group>\n";
            // Empty password in Windows unattend format (UTF-16LE base64 of "Password"), matching Rufus.
            xml += "            <Password>\n";
            xml += "              <Value>UABhAHMAcwB3AG8AcgBkAA==</Value>\n";
            xml += "              <PlainText>false</PlainText>\n";
            xml += "            </Password>\n";
            xml += "          </LocalAccount>\n";
            xml += "        </LocalAccounts>\n";
            xml += "      </UserAccounts>\n";

            firstLogonCommands << QString(R"(net user "%1" /logonpasswordchg:yes)").arg(safeUser);
            firstLogonCommands << "net accounts /maxpwage:unlimited";
        }

        if (options.qolEnhancements) {
            firstLogonCommands << R"(reg add "HKLM\System\CurrentControlSet\Control\Session Manager\Power" /v HiberbootEnabled /t REG_DWORD /d 0 /f)";
            firstLogonCommands << R"(reg add "HKLM\Software\Policies\Microsoft\Windows\WindowsCopilot" /v TurnOffWindowsCopilot /t REG_DWORD /d 1 /f)";
            firstLogonCommands << R"(reg add "HKLM\Software\Policies\Microsoft\Windows\CloudContent" /v DisableWindowsConsumerFeatures /t REG_DWORD /d 1 /f)";
            firstLogonCommands << R"(reg add "HKLM\Software\Policies\Microsoft\Dsh" /v AllowNewsAndInterests /t REG_DWORD /d 0 /f)";
            firstLogonCommands << R"(reg add "HKCU\Software\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\InprocServer32" /ve /t REG_SZ /d "" /f)";
        }

        if (!firstLogonCommands.isEmpty()) {
            xml += "      <FirstLogonCommands>\n";
            int order = 1;
            for (const QString &cmd : firstLogonCommands) {
                xml += "        <SynchronousCommand wcm:action=\"add\">\n";
                xml += QString("          <Order>%1</Order>\n").arg(order++);
                xml += QString("          <CommandLine>%1</CommandLine>\n").arg(xmlEscape(cmd));
                xml += "        </SynchronousCommand>\n";
            }
            xml += "      </FirstLogonCommands>\n";
        }

        xml += "    </component>\n";
        xml += "  </settings>\n";
    }

    xml += "</unattend>\n";
    return xml;
}
