#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "filepicker.h"
#include "isoanalyzer.h"
#include "theme.h"
#include "translator.h"
#include "unattend.h"
#include "worker.h"

namespace {

class FakeFilePicker : public FilePicker {
    Q_OBJECT
public:
    explicit FakeFilePicker(QObject *parent = nullptr) : FilePicker(parent) {}
    QString nextImage;
    void openImage(const QString &) override {
        if (!nextImage.isEmpty())
            emit imagePicked(nextImage);
    }
    void saveFile(const QString &, const QString &, const QString &) override {}
};

} // namespace

class RufusLinuxTests : public QObject {
    Q_OBJECT

private slots:
    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding() {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json");
        const QString command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":8}");
        QTRY_COMPARE(theme.radius(), 8);
    }

    void translatorFallsBackAndPlurals() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "da\n");
        write(dir.path() + "/en.json", R"({"%n drives": {"one": "%n drive", "other": "%n drives"}})");
        write(dir.path() + "/da.json", R"({"Save": "Gem", "%n drives": {"one": "%n drev", "other": "%n drev"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("da"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Gem"));
        QCOMPARE(translator.translate("Main", "%n drives", nullptr, 2), QString("%n drev"));
        QVERIFY(translator.translate("Main", "UnknownKey").isNull());
    }

    void isoAnalyzerDetectsGptRawImage() {
        QTemporaryDir dir;
        const QString imgPath = dir.filePath("arch-arm.img");
        QByteArray data(4096, '\0');
        data[510] = 0x55;
        data[511] = static_cast<char>(0xAA);
        data[446] = static_cast<char>(0x80);
        memcpy(data.data() + 512, "EFI PART", 8);
        write(imgPath, data);

        const IsoAnalysis info = IsoAnalyzer::analyze(imgPath);
        QVERIFY(info.valid);
        QCOMPARE(info.kind, QString("raw_image"));
        QCOMPARE(info.recommendedWriteMode, QString("dd"));
        QCOMPARE(info.recommendedScheme, QString("gpt"));
        QVERIFY(info.hasUefiBoot);
    }

    void isoAnalyzerDetectsCompressedImage() {
        QTemporaryDir dir;
        const QString gzPath = dir.filePath("disk.img.gz");
        QByteArray data;
        data.append(static_cast<char>(0x1F));
        data.append(static_cast<char>(0x8B));
        data.append(QByteArray(32, '\0'));
        write(gzPath, data);

        const IsoAnalysis info = IsoAnalyzer::analyze(gzPath);
        QVERIFY(info.valid);
        QVERIFY(info.isCompressed);
        QCOMPARE(info.compressionFormat, QString("gzip"));
        QCOMPARE(info.recommendedWriteMode, QString("dd"));
    }

    void isoAnalyzerReadsIso9660PvdAndHybridSignature() {
        QTemporaryDir dir;
        const QString isoPath = dir.filePath("omarchy-live.iso");
        QByteArray data(0x8800, '\0');
        // Hybrid MBR boot signature + partition entry byte
        data[450] = 0x17;
        data[510] = 0x55;
        data[511] = static_cast<char>(0xAA);
        // ISO-9660 Primary Volume Descriptor at sector 16 (0x8000)
        data[0x8000] = 1;
        memcpy(data.data() + 0x8001, "CD001", 5);
        memcpy(data.data() + 0x8028, "OMARCHY_2026    ", 16);
        write(isoPath, data);

        const IsoAnalysis info = IsoAnalyzer::analyze(isoPath);
        QVERIFY(info.valid);
        QVERIFY(info.isIso9660);
        QVERIFY(info.isHybrid);
        QCOMPARE(info.volumeLabel, QString("OMARCHY_2026"));
        QCOMPARE(info.recommendedWriteMode, QString("dd"));
    }

    void labelSanitizationRespectsFileSystemLimits() {
        QCOMPARE(IsoAnalyzer::sanitizeLabel("My Cool:ISO/2026!", "fat32"), QString("MY COOL_ISO"));
        QCOMPARE(IsoAnalyzer::sanitizeLabel("Linux_Persistence_Partition", "ext4").size(), 16);
        QCOMPARE(IsoAnalyzer::sanitizeLabel("   ", "ntfs"), QString("RUFUS_USB"));
    }

    void unattendXmlGeneratesRufusOptionsAndSanitizesUser() {
        WindowsUnattendOptions opts;
        opts.enabled = false;
        QVERIFY(UnattendGenerator::generate(opts).isEmpty());

        QCOMPARE(UnattendGenerator::sanitizeUsername("Administrator"), QString());
        QCOMPARE(UnattendGenerator::sanitizeUsername("SYSTEM"), QString());
        QCOMPARE(UnattendGenerator::sanitizeUsername("alice/bob*1"), QString("alice_bob_1"));

        opts.enabled = true;
        opts.arch = "arm64";
        opts.bypassTpmSecureBootRam = true;
        opts.bypassOnlineAccount = true;
        opts.createLocalAccount = true;
        opts.localUsername = "cjoy720";
        opts.disableDataCollection = true;
        opts.disableBitLocker = true;
        opts.qolEnhancements = true;

        const QString xml = UnattendGenerator::generate(opts);
        QVERIFY(xml.contains("processorArchitecture=\"arm64\""));
        QVERIFY(xml.contains("BypassTPMCheck"));
        QVERIFY(xml.contains("BypassSecureBootCheck"));
        QVERIFY(xml.contains("BypassRAMCheck"));
        QVERIFY(xml.contains("BypassNRO"));
        QVERIFY(xml.contains("<Name>cjoy720</Name>"));
        QVERIFY(xml.contains("UABhAHMAcwB3AG8AcgBkAA=="));
        QVERIFY(xml.contains("<ProtectYourPC>3</ProtectYourPC>"));
        QVERIFY(xml.contains("<DisableEncryptedDiskProvisioning>true</DisableEncryptedDiskProvisioning>"));
        QVERIFY(xml.contains("TurnOffWindowsCopilot"));
    }

    void deviceFilterProtectsSystemDisks() {
        const QByteArray sampleLsblk = R"({
          "blockdevices": [
            {
              "name": "nvme0n1", "path": "/dev/nvme0n1", "size": 250000000000,
              "model": "APPLE SSD", "tran": "nvme", "rm": false, "ro": false,
              "type": "disk", "hotplug": false, "mountpoints": [],
              "children": [
                { "name": "nvme0n1p1", "path": "/dev/nvme0n1p1", "type": "part", "mountpoints": ["/boot/efi"] },
                { "name": "nvme0n1p2", "path": "/dev/nvme0n1p2", "type": "part", "mountpoints": ["/"] }
              ]
            },
            {
              "name": "sda", "path": "/dev/sda", "size": 32000000000,
              "vendor": "SanDisk", "model": "Ultra Fit", "tran": "usb", "rm": true, "ro": false,
              "type": "disk", "hotplug": true, "mountpoints": []
            },
            {
              "name": "sdb", "path": "/dev/sdb", "size": 1000000000000,
              "vendor": "Samsung", "model": "T7 Shield", "tran": "usb", "rm": false, "ro": false,
              "type": "disk", "hotplug": false, "mountpoints": []
            },
            {
              "name": "sdc", "path": "/dev/sdc", "size": 64000000000,
              "vendor": "BootUSB", "model": "LiveRoot", "tran": "usb", "rm": true, "ro": false,
              "type": "disk", "hotplug": true, "mountpoints": [],
              "children": [
                { "name": "sdc1", "path": "/dev/sdc1", "type": "part", "mountpoints": ["/"] }
              ]
            },
            {
              "name": "zram0", "path": "/dev/zram0", "size": 8000000000,
              "tran": null, "rm": true, "ro": false, "type": "disk", "hotplug": false, "mountpoints": []
            }
          ]
        })";

        const QJsonObject root = QJsonDocument::fromJson(sampleLsblk).object();

        // Default: only removable flash drive sda (never nvme0n1, never sdc with '/', never zram0, never non-rm sdb).
        const QJsonArray defaultDevs = DiskWorker::filterDevices(root, false);
        QCOMPARE(defaultDevs.size(), 1);
        QCOMPARE(defaultDevs.at(0).toObject().value("path").toString(), QString("/dev/sda"));

        // With List USB Hard Drives: sda and sdb (still never nvme0n1 or sdc with '/').
        const QJsonArray allUsb = DiskWorker::filterDevices(root, true);
        QCOMPARE(allUsb.size(), 2);
        QCOMPARE(allUsb.at(1).toObject().value("path").toString(), QString("/dev/sdb"));
    }

    void backendChecksumsAndVerificationWork() {
        QTemporaryDir dir;
        const QString imgPath = dir.filePath("test.img");
        const QByteArray payload("RufusLinuxTestImagePayload1234567890");
        write(imgPath, payload);

        FakeFilePicker picker;
        picker.nextImage = imgPath;
        Backend backend(&picker, false);

        backend.openImageDialog();
        QVERIFY(backend.imageValid());
        QCOMPARE(backend.imageName(), QString("test.img"));

        backend.computeChecksums();
        QTRY_VERIFY(!backend.hashing() && !backend.sha256().isEmpty());

        const QString expectedSha256 =
            QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
        QCOMPARE(backend.sha256(), expectedSha256);
        QCOMPARE(backend.verifyHash(expectedSha256.toUpper()), QString("match"));
        QCOMPARE(backend.verifyHash("deadbeef"), QString("mismatch"));
    }

    void workerRefusesInternalNvmeAndPerformsDdAndSaveOnTempFiles() {
        // Safety check: worker MUST reject internal system disks.
        QString reason;
        QVERIFY(!DiskWorker::validateTargetDevice("/dev/nvme0n1", true, false, &reason));
        QVERIFY(reason.contains("Refusing"));

        QTemporaryDir dir;
        const QString srcImg = dir.filePath("source.img");
        const QString dstDisk = dir.filePath("virtual-usb.img");
        const QString backupImg = dir.filePath("backup.img");

        QByteArray content(65536, '\0');
        for (int i = 0; i < content.size(); ++i)
            content[i] = static_cast<char>((i * 31 + 7) & 0xFF);
        write(srcImg, content);
        write(dstDisk, QByteArray(content.size(), '\0'));

        // DD write test
        const QJsonObject ddJob{
            {"action", "write"},
            {"device", dstDisk},
            {"bootSelection", "image"},
            {"imagePath", srcImg},
            {"writeMode", "dd"},
        };
        QCOMPARE(DiskWorker::runJob(ddJob, true), 0);

        QFile written(dstDisk);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(written.readAll(), content);
        written.close();

        // Save drive image test
        const QJsonObject saveJob{
            {"action", "save_image"},
            {"device", dstDisk},
            {"outputPath", backupImg},
        };
        QCOMPARE(DiskWorker::runJob(saveJob, true), 0);

        QFile saved(backupImg);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), content);
        saved.close();

        // GPT partition table test with UEFI:NTFS reservation on 32 MiB temp disk file
        const QString gptDisk = dir.filePath("gpt-usb.img");
        {
            QFile f(gptDisk);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QVERIFY(f.resize(32LL * 1024 * 1024));
        }
        const QJsonObject partJob{
            {"action", "write"},
            {"device", gptDisk},
            {"bootSelection", "image"},
            {"writeMode", "iso"},
            {"partitionScheme", "gpt"},
            {"fileSystem", "ntfs"},
            {"uefiNtfs", true},
        };
        QCOMPARE(DiskWorker::runJob(partJob, true), 0);

        // Verify GPT header "EFI PART" was actually written at LBA 1 (offset 512).
        QFile verifyGpt(gptDisk);
        QVERIFY(verifyGpt.open(QIODevice::ReadOnly));
        verifyGpt.seek(512);
        QCOMPARE(verifyGpt.read(8), QByteArray("EFI PART", 8));
    }

    void embeddedRufusAssetsExist() {
        QFile uefiImg(":/assets/uefi/uefi-ntfs.img");
        QVERIFY(uefiImg.exists());
        QCOMPARE(uefiImg.size(), 1048576);
        QVERIFY(QFile(":/assets/freedos/KERNEL.SYS").exists());
        QVERIFY(QFile(":/assets/freedos/COMMAND.COM").exists());
    }

private:
    static void write(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(contents), contents.size());
    }
};

QTEST_MAIN(RufusLinuxTests)
#include "rufus-linux_tests.moc"
