// rufus-linux — Bootable USB creator and disk formatter for Linux (Omarchy).

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>
#include <QUrl>

#include "backend.h"
#include "isoanalyzer.h"
#include "theme.h"
#include "translator.h"
#include "worker.h"

namespace {

int runCliList(bool includeAllUsb, bool jsonOut) {
    QString err;
    const QJsonArray devices = DiskWorker::listDevices(includeAllUsb, &err);
    if (!err.isEmpty()) {
        QTextStream(stderr) << err << "\n";
        return 1;
    }
    if (jsonOut) {
        QTextStream(stdout) << QJsonDocument(devices).toJson(QJsonDocument::Indented);
        return 0;
    }
    QTextStream out(stdout);
    if (devices.isEmpty()) {
        out << "No removable USB drives detected.\n";
        return 0;
    }
    for (const QJsonValue &v : devices) {
        const QJsonObject d = v.toObject();
        out << d.value("path").toString() << "\t"
            << d.value("sizeText").toString() << "\t"
            << d.value("model").toString() << "\n";
    }
    return 0;
}

int runCliInspect(const QString &path, bool jsonOut) {
    const IsoAnalysis info = IsoAnalyzer::analyze(path);
    if (jsonOut) {
        QTextStream(stdout) << QJsonDocument(info.toJson()).toJson(QJsonDocument::Indented);
        return info.valid ? 0 : 1;
    }
    if (!info.valid) {
        QTextStream(stderr) << info.error << "\n";
        return 1;
    }
    QTextStream out(stdout);
    out << "File:         " << info.fileName << " (" << info.fileSize << " bytes)\n"
        << "Kind:         " << info.kind << "\n"
        << "Summary:      " << info.summary << "\n"
        << "Volume label: " << info.volumeLabel << "\n"
        << "Recommended:  mode=" << info.recommendedWriteMode
        << " scheme=" << info.recommendedScheme
        << " fs=" << info.recommendedFileSystem
        << (info.recommendUefiNtfs ? " uefi-ntfs=yes" : "") << "\n";
    return 0;
}

int runCliHash(const QString &path, bool jsonOut) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QTextStream(stderr) << "Cannot open " << path << ": " << file.errorString() << "\n";
        return 1;
    }
    QCryptographicHash hMd5(QCryptographicHash::Md5);
    QCryptographicHash hSha1(QCryptographicHash::Sha1);
    QCryptographicHash hSha256(QCryptographicHash::Sha256);
    QCryptographicHash hSha512(QCryptographicHash::Sha512);
    QByteArray buf(1024 * 1024, Qt::Uninitialized);
    while (true) {
        const qint64 n = file.read(buf.data(), buf.size());
        if (n <= 0) break;
        const QByteArrayView view(buf.constData(), n);
        hMd5.addData(view);
        hSha1.addData(view);
        hSha256.addData(view);
        hSha512.addData(view);
    }
    const QJsonObject res{
        {"path", path},
        {"md5", QString::fromLatin1(hMd5.result().toHex())},
        {"sha1", QString::fromLatin1(hSha1.result().toHex())},
        {"sha256", QString::fromLatin1(hSha256.result().toHex())},
        {"sha512", QString::fromLatin1(hSha512.result().toHex())},
    };
    if (jsonOut) {
        QTextStream(stdout) << QJsonDocument(res).toJson(QJsonDocument::Indented);
    } else {
        QTextStream out(stdout);
        out << "MD5:    " << res.value("md5").toString() << "\n"
            << "SHA1:   " << res.value("sha1").toString() << "\n"
            << "SHA256: " << res.value("sha256").toString() << "\n"
            << "SHA512: " << res.value("sha512").toString() << "\n";
    }
    return 0;
}

void printHelp() {
    QTextStream(stdout)
        << "Usage:\n"
        << "  rufus-linux [image-file]              Launch the Rufus desktop app\n"
        << "  rufus-linux list [--all-usb] [--json] List safe removable USB drives\n"
        << "  rufus-linux inspect <file> [--json]   Inspect an ISO or disk image\n"
        << "  rufus-linux hash <file> [--json]      Compute MD5/SHA-1/SHA-256/SHA-512\n";
}

} // namespace

int main(int argc, char *argv[]) {
    QStringList rawArgs;
    for (int i = 1; i < argc; ++i)
        rawArgs << QString::fromLocal8Bit(argv[i]);

    // Headless worker and CLI subcommands never need a display server.
    if (!rawArgs.isEmpty()) {
        const QString first = rawArgs.first();
        if (first == "--worker" || first == "list" || first == "inspect" ||
            first == "hash" || first == "--help" || first == "-h") {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            QCoreApplication cliApp(argc, argv);
            cliApp.setApplicationName("rufus-linux");
            cliApp.setApplicationVersion("1.0.0");

            const bool jsonOut = rawArgs.contains("--json");
            if (first == "--help" || first == "-h") {
                printHelp();
                return 0;
            }
            if (first == "--worker") {
                return DiskWorker::runFromStdin(rawArgs.contains("--allow-file"));
            }
            if (first == "list") {
                return runCliList(rawArgs.contains("--all-usb"), jsonOut);
            }
            if (first == "inspect") {
                if (rawArgs.size() < 2) {
                    QTextStream(stderr) << "Missing image path.\n";
                    return 1;
                }
                return runCliInspect(rawArgs.at(1), jsonOut);
            }
            if (first == "hash") {
                if (rawArgs.size() < 2) {
                    QTextStream(stderr) << "Missing file path.\n";
                    return 1;
                }
                return runCliHash(rawArgs.at(1), jsonOut);
            }
        }
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("rufus-linux");
    app.setApplicationVersion("1.0.0");

    // Matches the window to rufus-linux.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("rufus-linux");
    app.setWindowIcon(QIcon::fromTheme("rufus-linux"));

    QQuickStyle::setStyle("Material");

    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;

    // Optional file argument (e.g. from terminal or file manager Open With).
    for (const QString &arg : rawArgs) {
        if (arg == "open" || arg.startsWith('-'))
            continue;
        if (QFileInfo::exists(arg)) {
            backend.selectImage(arg);
            break;
        }
    }

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    QObject::connect(&translator, &Translator::changed, &backend, [&backend]() {
        emit backend.optionsChanged();
        emit backend.imageChanged();
        emit backend.stateChanged();
    });
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
