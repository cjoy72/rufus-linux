import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 680
    height: 820
    minimumWidth: 560
    minimumHeight: 660
    visible: true
    title: qsTr("Rufus")
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property bool logVisible: false
    property bool hashVisible: false
    property bool wueVisible: false
    property bool confirmVisible: false
    property bool advancedDriveOpen: false
    property bool advancedFormatOpen: false
    property bool wuePreviewOpen: false
    property bool quitConfirmVisible: false

    readonly property bool textInputFocused:
        activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit
    readonly property bool anyModalOpen:
        helpVisible || logVisible || hashVisible || wueVisible || confirmVisible || quitConfirmVisible
    readonly property bool ddModeActive:
        backend.bootSelection === "image" && backend.writeMode === "dd"

    function requestStart() {
        if (!backend.canStart)
            return
        confirmVisible = true
    }

    function requestQuit() {
        if (backend.busy) quitConfirmVisible = true
        else Qt.quit()
    }

    onClosing: function(close) {
        if (backend.busy && !quitConfirmVisible) {
            close.accepted = false
            quitConfirmVisible = true
        }
    }

    // Reusable styled button so dark surfaces and primary accent buttons are
    // unmistakable on every Omarchy theme and Qt Material version.
    component ActionButton: Button {
        property bool primary: false
        property bool danger: false
        implicitHeight: 36
        leftPadding: 14
        rightPadding: 14
        font.pixelSize: 12
        font.bold: primary || danger
        Material.roundedScale: theme.radius

        contentItem: Text {
            text: parent.text
            font: parent.font
            color: !parent.enabled ? "#686876"
                 : parent.danger ? "#ffffff"
                 : parent.primary ? theme.accentForeground
                 : "#ececf2"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            radius: Math.min(6, theme.radius)
            color: !parent.enabled ? "#1c1c22"
                 : parent.danger ? (parent.down ? "#b33846" : parent.hovered ? "#e05565" : "#d94a5a")
                 : parent.primary ? (parent.down ? Qt.darker(theme.accent, 1.15) : parent.hovered ? Qt.lighter(theme.accent, 1.08) : theme.accent)
                 : (parent.down ? "#2f2f3c" : parent.hovered ? "#2a2a35" : "#22222b")
            border.width: (parent.primary || parent.danger) ? 0 : 1
            border.color: parent.enabled ? "#383846" : "#25252e"
        }
    }

    // Keyboard shortcuts (letter keys pause while editing a text input).
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !textInputFocused; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        enabled: anyModalOpen
        onActivated: {
            if (confirmVisible) confirmVisible = false
            else if (quitConfirmVisible) quitConfirmVisible = false
            else if (helpVisible) helpVisible = false
            else if (wueVisible) wueVisible = false
            else if (hashVisible) hashVisible = false
            else if (logVisible) logVisible = false
        }
    }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !textInputFocused && !anyModalOpen; onActivated: win.requestQuit() }
    Shortcut { sequence: "O"; context: Qt.ApplicationShortcut; enabled: !textInputFocused && !anyModalOpen && !backend.busy; onActivated: backend.openImageDialog() }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; enabled: !textInputFocused && !anyModalOpen && !backend.busy; onActivated: backend.refreshDevices() }
    Shortcut { sequence: "L"; context: Qt.ApplicationShortcut; enabled: !textInputFocused && !confirmVisible; onActivated: win.logVisible = !win.logVisible }
    Shortcut {
        sequence: "H"
        context: Qt.ApplicationShortcut
        enabled: !textInputFocused && !confirmVisible && backend.imageValid
        onActivated: {
            win.hashVisible = !win.hashVisible
            if (win.hashVisible && backend.sha256.length === 0 && !backend.hashing)
                backend.computeChecksums()
        }
    }
    Shortcut {
        sequence: "W"
        context: Qt.ApplicationShortcut
        enabled: !textInputFocused && !confirmVisible && backend.isWindowsIso && backend.writeMode === "iso"
        onActivated: win.wueVisible = !win.wueVisible
    }
    Shortcut {
        sequence: "S"
        context: Qt.ApplicationShortcut
        enabled: !textInputFocused && !anyModalOpen && !backend.busy && backend.deviceCount > 0
        onActivated: backend.openSaveDriveDialog()
    }
    Shortcut {
        sequence: "Space"
        context: Qt.ApplicationShortcut
        enabled: !textInputFocused && !anyModalOpen
        onActivated: {
            if (backend.busy) backend.cancelOperation()
            else win.requestStart()
        }
    }

    function formatProgress(pct) {
        void(translator.language)
        return Math.round(pct).toLocaleString(Qt.locale(), 'f', 0) + "%"
    }

    function formatDeviceCount(count) {
        void(translator.language)
        return qsTr("%n drives", "", count)
    }

    function translatedStatus(raw) {
        void(translator.language)
        if (raw === "READY") return qsTr("READY")
        if (raw === "FAILED") return qsTr("FAILED")
        if (raw === "CANCELLED") return qsTr("CANCELLED")
        return raw
    }

    function translatedTargetSystem() {
        void(translator.language)
        return backend.partitionScheme === "gpt" ? qsTr("UEFI (non CSM)") : qsTr("BIOS or UEFI")
    }

    function translatedImageSummary() {
        void(translator.language)
        if (!backend.imageValid) return ""
        if (backend.isWindowsIso) {
            return backend.hasLargeFile
                ? qsTr("Windows ISO (%1) — >4 GiB file (NTFS + UEFI:NTFS recommended)").arg(backend.windowsArch)
                : qsTr("Windows ISO (%1) — ISO Image mode").arg(backend.windowsArch)
        }
        if (backend.isCompressedImage)
            return qsTr("Compressed disk image — DD Image mode")
        if (backend.isHybridIso) {
            return backend.supportsPersistence
                ? qsTr("Hybrid Linux ISO — DD or ISO mode (persistence supported)")
                : qsTr("Hybrid ISO image — DD mode recommended, ISO mode available")
        }
        return backend.imageSummary
    }

    // Right-to-left languages mirror the window; Translator sets layoutDirection.
    Item {
        id: root
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        DropArea {
            id: dropArea
            anchors.fill: parent
            enabled: !backend.busy
            onDropped: function(drop) {
                if (drop.hasUrls && drop.urls.length > 0)
                    backend.selectImage(drop.urls[0].toString())
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

            // Top Header
            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Rectangle {
                    width: 34
                    height: 34
                    radius: Math.min(8, Math.max(4, theme.radius))
                    color: theme.accent

                    Text {
                        anchors.centerIn: parent
                        text: "R"
                        color: theme.accentForeground
                        font.pixelSize: 18
                        font.bold: true
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        text: qsTr("Rufus")
                        color: "#f4f4f6"
                        font.pixelSize: 17
                        font.bold: true
                    }
                    Text {
                        text: qsTr("Bootable USB Creator for Linux") + " • " + win.formatDeviceCount(backend.deviceCount)
                        color: "#8e8e9a"
                        font.pixelSize: 12
                    }
                }

                ActionButton {
                    text: qsTr("Refresh")
                    enabled: !backend.busy
                    implicitHeight: 30
                    onClicked: backend.refreshDevices()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Refresh USB devices (R)")
                }

                ActionButton {
                    text: "#"
                    enabled: backend.imageValid
                    implicitHeight: 30
                    leftPadding: 11
                    rightPadding: 11
                    font.bold: true
                    onClicked: {
                        win.hashVisible = true
                        if (backend.sha256.length === 0 && !backend.hashing)
                            backend.computeChecksums()
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Compute image checksums (H)")
                }

                ActionButton {
                    text: qsTr("Log")
                    implicitHeight: 30
                    onClicked: win.logVisible = !win.logVisible
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Toggle operation log (L)")
                }

                ActionButton {
                    text: "?"
                    implicitHeight: 30
                    leftPadding: 11
                    rightPadding: 11
                    font.bold: true
                    onClicked: win.helpVisible = !win.helpVisible
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Keyboard shortcuts (?)")
                }
            }

            // Warning / Error / Success Banner
            Rectangle {
                Layout.fillWidth: true
                visible: backend.errorText.length > 0 || backend.successText.length > 0 || backend.missingToolWarning.length > 0
                implicitHeight: visible ? bannerRow.implicitHeight + 14 : 0
                radius: theme.radius
                color: backend.errorText.length > 0 ? "#38171c"
                     : backend.missingToolWarning.length > 0 ? "#332914"
                     : "#143022"
                border.width: 1
                border.color: backend.errorText.length > 0 ? "#d94a5a"
                            : backend.missingToolWarning.length > 0 ? "#d9a23b"
                            : "#2fb573"

                RowLayout {
                    id: bannerRow
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        text: backend.errorText.length > 0 ? backend.errorText
                            : backend.missingToolWarning.length > 0 ? backend.missingToolWarning
                            : backend.successText
                        color: "#f4f4f6"
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }

                    ToolButton {
                        visible: backend.errorText.length > 0 || backend.successText.length > 0
                        text: "×"
                        font.pixelSize: 14
                        onClicked: backend.clearBanner()
                    }
                }
            }

            // Scrollable Main Form Area
            Flickable {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: width
                contentHeight: formCol.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                ColumnLayout {
                    id: formCol
                    width: parent.width
                    spacing: 10

                    // ==================== DRIVE PROPERTIES ====================
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: driveCol.implicitHeight + 24
                        radius: theme.radius
                        color: "#16161a"
                        border.color: "#26262e"
                        border.width: 1

                        ColumnLayout {
                            id: driveCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 12
                            spacing: 7

                            Text {
                                text: qsTr("Drive Properties")
                                color: theme.accent
                                font.pixelSize: 13
                                font.bold: true
                            }

                            // Device selector
                            Text {
                                text: qsTr("Device")
                                color: "#c8c8d2"
                                font.pixelSize: 12
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                ComboBox {
                                    id: deviceCombo
                                    Layout.fillWidth: true
                                    implicitHeight: 38
                                    enabled: !backend.busy && backend.deviceCount > 0
                                    Material.roundedScale: theme.radius
                                    model: {
                                        void(translator.language)
                                        if (backend.deviceCount === 0)
                                            return [qsTr("No removable USB drives detected")]
                                        var list = []
                                        for (var i = 0; i < backend.devices.length; ++i)
                                            list.push(backend.devices[i].displayName)
                                        return list
                                    }
                                    currentIndex: backend.deviceCount > 0 ? Math.max(0, backend.selectedDeviceIndex) : 0
                                    onModelChanged: currentIndex = backend.deviceCount > 0 ? Math.max(0, backend.selectedDeviceIndex) : 0
                                    onActivated: function(idx) {
                                        if (backend.deviceCount > 0)
                                            backend.selectedDeviceIndex = idx
                                    }
                                }

                                ActionButton {
                                    text: qsTr("Save")
                                    implicitHeight: 38
                                    enabled: !backend.busy && backend.deviceCount > 0
                                    onClicked: backend.openSaveDriveDialog()
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Save drive contents to raw .img file (S)")
                                }
                            }

                            // Boot selection
                            Text {
                                text: qsTr("Boot selection")
                                color: "#c8c8d2"
                                font.pixelSize: 12
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                ComboBox {
                                    id: bootCombo
                                    Layout.fillWidth: true
                                    implicitHeight: 38
                                    enabled: !backend.busy
                                    Material.roundedScale: theme.radius
                                    function desiredIndex() {
                                        return backend.bootSelection === "image" ? 0
                                             : backend.bootSelection === "non_bootable" ? 1 : 2
                                    }
                                    model: {
                                        void(translator.language)
                                        return [
                                            qsTr("Disk or ISO image (Please select)"),
                                            qsTr("Non bootable"),
                                            qsTr("FreeDOS")
                                        ]
                                    }
                                    currentIndex: desiredIndex()
                                    onModelChanged: currentIndex = desiredIndex()
                                    onActivated: function(idx) {
                                        backend.bootSelection = idx === 0 ? "image"
                                                              : idx === 1 ? "non_bootable" : "freedos"
                                    }
                                }

                                ActionButton {
                                    text: qsTr("SELECT")
                                    implicitHeight: 38
                                    enabled: !backend.busy
                                    primary: backend.bootSelection === "image" && !backend.imageValid
                                    onClicked: backend.openImageDialog()
                                }
                            }

                            // Selected Image Summary Pill
                            Rectangle {
                                Layout.fillWidth: true
                                visible: backend.bootSelection === "image" && backend.imageValid
                                implicitHeight: visible ? imgInfoCol.implicitHeight + 14 : 0
                                radius: Math.min(6, theme.radius)
                                color: "#1e1e26"
                                border.color: "#343442"
                                border.width: 1

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 7
                                    spacing: 8

                                    ColumnLayout {
                                        id: imgInfoCol
                                        Layout.fillWidth: true
                                        spacing: 2

                                        Text {
                                            Layout.fillWidth: true
                                            text: backend.imageName + " (" + backend.imageSizeText + ")"
                                            color: "#f4f4f6"
                                            font.pixelSize: 12
                                            font.bold: true
                                            elide: Text.ElideMiddle
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: win.translatedImageSummary()
                                            color: "#a0a0b0"
                                            font.pixelSize: 11
                                            wrapMode: Text.WordWrap
                                        }
                                    }

                                    ActionButton {
                                        visible: backend.isWindowsIso && backend.writeMode === "iso"
                                        text: qsTr("Customize")
                                        implicitHeight: 30
                                        enabled: !backend.busy
                                        onClicked: win.wueVisible = true
                                        ToolTip.visible: hovered
                                        ToolTip.text: qsTr("Windows User Experience options (W)")
                                    }

                                    ToolButton {
                                        text: "×"
                                        enabled: !backend.busy
                                        onClicked: backend.clearImage()
                                    }
                                }
                            }

                            // Write Mode selector (Hybrid ISOs)
                            RowLayout {
                                Layout.fillWidth: true
                                visible: backend.bootSelection === "image" && backend.imageValid && backend.isHybridIso
                                spacing: 10

                                Text {
                                    text: qsTr("Write mode")
                                    color: "#c8c8d2"
                                    font.pixelSize: 12
                                }

                                ComboBox {
                                    id: writeModeCombo
                                    Layout.fillWidth: true
                                    implicitHeight: 38
                                    enabled: !backend.busy
                                    Material.roundedScale: theme.radius
                                    function desiredIndex() {
                                        return backend.writeMode === "dd" ? 0 : 1
                                    }
                                    model: {
                                        void(translator.language)
                                        return [
                                            qsTr("DD Image mode (Recommended)"),
                                            qsTr("ISO Image mode (Files)")
                                        ]
                                    }
                                    currentIndex: desiredIndex()
                                    onModelChanged: currentIndex = desiredIndex()
                                    onActivated: function(idx) {
                                        backend.writeMode = idx === 0 ? "dd" : "iso"
                                    }
                                }
                            }

                            // Persistence Partition Slider
                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: backend.supportsPersistence && backend.maxPersistenceMB > 0
                                spacing: 2

                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        text: qsTr("Persistent partition size")
                                        color: "#c8c8d2"
                                        font.pixelSize: 12
                                    }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: backend.persistenceMB === 0
                                              ? qsTr("0 (No persistence)")
                                              : (backend.persistenceMB >= 1024
                                                 ? (backend.persistenceMB / 1024.0).toFixed(1) + " GB"
                                                 : backend.persistenceMB + " MB")
                                        color: theme.accent
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                }

                                Slider {
                                    Layout.fillWidth: true
                                    enabled: !backend.busy
                                    from: 0
                                    to: backend.maxPersistenceMB
                                    stepSize: 256
                                    value: backend.persistenceMB
                                    onMoved: backend.persistenceMB = Math.round(value)
                                }
                            }

                            // Partition scheme & Target system
                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                columnSpacing: 12
                                rowSpacing: 4
                                visible: !win.ddModeActive

                                Text { text: qsTr("Partition scheme"); color: "#c8c8d2"; font.pixelSize: 12 }
                                Text { text: qsTr("Target system"); color: "#c8c8d2"; font.pixelSize: 12 }

                                ComboBox {
                                    Layout.fillWidth: true
                                    implicitHeight: 38
                                    enabled: !backend.busy
                                    Material.roundedScale: theme.radius
                                    model: ["GPT", "MBR"]
                                    currentIndex: backend.partitionScheme === "gpt" ? 0 : 1
                                    onActivated: function(idx) {
                                        backend.partitionScheme = idx === 0 ? "gpt" : "mbr"
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 38
                                    radius: Math.min(4, theme.radius)
                                    color: "#1e1e26"
                                    border.color: "#2f2f39"
                                    border.width: 1

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.margins: 12
                                        text: win.translatedTargetSystem()
                                        color: "#dcdce4"
                                        font.pixelSize: 13
                                        elide: Text.ElideRight
                                    }
                                }
                            }

                            // Advanced Drive Properties toggle
                            ToolButton {
                                text: (win.advancedDriveOpen ? "▾ " : "▸ ") + qsTr("Show advanced drive properties")
                                font.pixelSize: 12
                                implicitHeight: 28
                                Layout.alignment: Qt.AlignLeft
                                onClicked: win.advancedDriveOpen = !win.advancedDriveOpen
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: win.advancedDriveOpen
                                spacing: 0

                                CheckBox {
                                    Layout.fillWidth: true
                                    text: qsTr("List USB Hard Drives")
                                    enabled: !backend.busy
                                    checked: backend.listUsbHdd
                                    onToggled: backend.listUsbHdd = checked
                                }

                                CheckBox {
                                    Layout.fillWidth: true
                                    text: qsTr("Add UEFI:NTFS bootloader partition (boot >4 GiB Windows ISOs)")
                                    enabled: !backend.busy && !win.ddModeActive && (backend.fileSystem === "ntfs" || backend.fileSystem === "exfat")
                                    checked: backend.uefiNtfs
                                    onToggled: backend.uefiNtfs = checked
                                }
                            }
                        }
                    }

                    // ==================== FORMAT OPTIONS ====================
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: formatCol.implicitHeight + 24
                        radius: theme.radius
                        color: "#16161a"
                        border.color: "#26262e"
                        border.width: 1
                        opacity: win.ddModeActive ? 0.55 : 1.0

                        ColumnLayout {
                            id: formatCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 12
                            spacing: 7

                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    text: qsTr("Format Options")
                                    color: theme.accent
                                    font.pixelSize: 13
                                    font.bold: true
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    visible: win.ddModeActive
                                    text: qsTr("Bypassed in DD Image mode")
                                    color: "#8e8e9a"
                                    font.pixelSize: 11
                                }
                            }

                            // Volume label
                            Text {
                                text: qsTr("Volume label")
                                color: "#c8c8d2"
                                font.pixelSize: 12
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                height: 36
                                radius: Math.min(4, theme.radius)
                                color: "#1e1e26"
                                border.color: labelInput.activeFocus ? theme.accent : "#32323e"
                                border.width: 1

                                TextInput {
                                    id: labelInput
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    verticalAlignment: TextInput.AlignVCenter
                                    enabled: !backend.busy && !win.ddModeActive
                                    color: "#f4f4f6"
                                    selectionColor: theme.accent
                                    selectedTextColor: theme.accentForeground
                                    font.pixelSize: 13
                                    text: backend.volumeLabel
                                    onTextEdited: backend.volumeLabel = text
                                }
                            }

                            // File system & Cluster size
                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                columnSpacing: 12
                                rowSpacing: 4

                                Text { text: qsTr("File system"); color: "#c8c8d2"; font.pixelSize: 12 }
                                Text { text: qsTr("Cluster size"); color: "#c8c8d2"; font.pixelSize: 12 }

                                ComboBox {
                                    id: fsCombo
                                    Layout.fillWidth: true
                                    implicitHeight: 38
                                    enabled: !backend.busy && !win.ddModeActive
                                    Material.roundedScale: theme.radius
                                    readonly property var fsKeys: ["fat32", "ntfs", "exfat", "ext4", "ext3", "ext2", "udf"]
                                    model: ["FAT32", "NTFS", "exFAT", "ext4", "ext3", "ext2", "UDF"]
                                    currentIndex: Math.max(0, fsKeys.indexOf(backend.fileSystem))
                                    onActivated: function(idx) {
                                        backend.fileSystem = fsKeys[idx]
                                    }
                                }

                                ComboBox {
                                    id: clusterCombo
                                    Layout.fillWidth: true
                                    implicitHeight: 38
                                    enabled: !backend.busy && !win.ddModeActive
                                    Material.roundedScale: theme.radius
                                    readonly property var clusterValues: [0, 4096, 8192, 16384, 32768, 65536]
                                    function desiredIndex() {
                                        return Math.max(0, clusterValues.indexOf(backend.clusterSize))
                                    }
                                    model: {
                                        void(translator.language)
                                        return [
                                            qsTr("Default"),
                                            "4096 bytes",
                                            "8192 bytes",
                                            "16 kilobytes",
                                            "32 kilobytes",
                                            "64 kilobytes"
                                        ]
                                    }
                                    currentIndex: desiredIndex()
                                    onModelChanged: currentIndex = desiredIndex()
                                    onActivated: function(idx) {
                                        backend.clusterSize = clusterValues[idx]
                                    }
                                }
                            }

                            // Advanced Format Options toggle
                            ToolButton {
                                text: (win.advancedFormatOpen ? "▾ " : "▸ ") + qsTr("Show advanced format options")
                                font.pixelSize: 12
                                implicitHeight: 28
                                Layout.alignment: Qt.AlignLeft
                                onClicked: win.advancedFormatOpen = !win.advancedFormatOpen
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: win.advancedFormatOpen
                                spacing: 2

                                CheckBox {
                                    Layout.fillWidth: true
                                    text: qsTr("Quick format")
                                    enabled: !backend.busy && !win.ddModeActive
                                    checked: backend.quickFormat
                                    onToggled: backend.quickFormat = checked
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 12

                                    CheckBox {
                                        text: qsTr("Check device for bad blocks")
                                        enabled: !backend.busy
                                        checked: backend.checkBadBlocks
                                        onToggled: backend.checkBadBlocks = checked
                                    }

                                    ComboBox {
                                        Layout.fillWidth: true
                                        implicitHeight: 36
                                        enabled: !backend.busy && backend.checkBadBlocks
                                        Material.roundedScale: theme.radius
                                        model: {
                                            void(translator.language)
                                            return [
                                                qsTr("1 pass"),
                                                qsTr("2 passes"),
                                                qsTr("3 passes"),
                                                qsTr("4 passes")
                                            ]
                                        }
                                        currentIndex: backend.badBlocksPasses - 1
                                        onModelChanged: currentIndex = backend.badBlocksPasses - 1
                                        onActivated: function(idx) {
                                            backend.badBlocksPasses = idx + 1
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ==================== STATUS & PROGRESS ====================
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: statusCol.implicitHeight + 22
                radius: theme.radius
                color: "#16161a"
                border.color: "#26262e"
                border.width: 1

                ColumnLayout {
                    id: statusCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 11
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: qsTr("Status")
                            color: theme.accent
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            visible: backend.speedText.length > 0
                            text: backend.speedText
                            color: "#a8a8b6"
                            font.pixelSize: 12
                            font.family: "monospace"
                        }
                        Text {
                            text: backend.elapsedText
                            color: "#8e8e9a"
                            font.pixelSize: 12
                            font.family: "monospace"
                        }
                    }

                    // Rufus-style progress bar
                    Rectangle {
                        Layout.fillWidth: true
                        height: 28
                        radius: Math.min(4, theme.radius)
                        color: "#1e1e26"
                        border.color: "#32323e"
                        border.width: 1
                        clip: true

                        Rectangle {
                            height: parent.height
                            width: parent.width * Math.max(0, Math.min(1, backend.progress / 100.0))
                            color: theme.accent
                            Behavior on width { NumberAnimation { duration: 150 } }
                        }

                        Text {
                            anchors.centerIn: parent
                            text: backend.busy
                                  ? win.translatedStatus(backend.statusText) + " (" + win.formatProgress(backend.progress) + ")"
                                  : win.translatedStatus(backend.statusText)
                            color: backend.progress > 52 ? theme.accentForeground : "#f4f4f6"
                            font.pixelSize: 12
                            font.bold: true
                        }
                    }

                    // Bottom Action Buttons
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Text {
                            Layout.fillWidth: true
                            text: backend.deviceCount === 0
                                  ? qsTr("Plug in a USB drive or drop an ISO file here.")
                                  : qsTr("Press Space to start, ? for shortcuts.")
                            color: "#7f7f8c"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }

                        ActionButton {
                            text: backend.busy ? qsTr("CANCEL") : qsTr("START")
                            enabled: backend.busy || backend.canStart
                            primary: !backend.busy
                            danger: backend.busy
                            implicitWidth: 126
                            onClicked: {
                                if (backend.busy) backend.cancelOperation()
                                else win.requestStart()
                            }
                        }

                        ActionButton {
                            text: qsTr("CLOSE")
                            implicitWidth: 92
                            onClicked: win.requestQuit()
                        }
                    }
                }
            }
        }

        // Drag-and-drop highlight overlay
        Rectangle {
            anchors.fill: parent
            visible: dropArea.containsDrag
            color: "#cc0e0e10"
            border.color: theme.accent
            border.width: 2

            Text {
                anchors.centerIn: parent
                text: qsTr("Drop ISO or Disk Image to load")
                color: theme.accent
                font.pixelSize: 20
                font.bold: true
            }
        }

        // ==================== DESTRUCTIVE CONFIRMATION MODAL ====================
        Rectangle {
            anchors.fill: parent
            visible: win.confirmVisible
            color: "#b8000000"
            MouseArea { anchors.fill: parent }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 480)
                implicitHeight: confirmCol.implicitHeight + 36
                radius: theme.radius
                color: "#1a1a20"
                border.color: "#d94a5a"
                border.width: 1

                ColumnLayout {
                    id: confirmCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 18
                    spacing: 12

                    Text {
                        text: qsTr("WARNING: Erase Device?")
                        color: "#ff6b7a"
                        font.pixelSize: 16
                        font.bold: true
                    }

                    Text {
                        Layout.fillWidth: true
                        text: backend.confirmationSummary
                        color: "#e6e6ee"
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Item { Layout.fillWidth: true }
                        ActionButton {
                            text: qsTr("Cancel")
                            onClicked: win.confirmVisible = false
                        }
                        ActionButton {
                            text: qsTr("OK — Erase & Write")
                            danger: true
                            onClicked: {
                                win.confirmVisible = false
                                backend.startOperation()
                            }
                        }
                    }
                }
            }
        }

        // ==================== QUIT CONFIRMATION MODAL ====================
        Rectangle {
            anchors.fill: parent
            visible: win.quitConfirmVisible
            color: "#b8000000"
            MouseArea { anchors.fill: parent }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 420)
                implicitHeight: quitCol.implicitHeight + 36
                radius: theme.radius
                color: "#1a1a20"
                border.color: "#d9a23b"
                border.width: 1

                ColumnLayout {
                    id: quitCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 18
                    spacing: 12

                    Text {
                        text: qsTr("Operation in progress")
                        color: "#f4f4f6"
                        font.pixelSize: 16
                        font.bold: true
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("A disk write is currently running. Quitting now will cancel it and may leave the USB drive partially written.")
                        color: "#c8c8d2"
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Item { Layout.fillWidth: true }
                        ActionButton {
                            text: qsTr("Keep Running")
                            onClicked: win.quitConfirmVisible = false
                        }
                        ActionButton {
                            text: qsTr("Abort & Quit")
                            danger: true
                            onClicked: {
                                backend.cancelOperation()
                                Qt.quit()
                            }
                        }
                    }
                }
            }
        }

        // ==================== WINDOWS USER EXPERIENCE MODAL ====================
        Rectangle {
            anchors.fill: parent
            visible: win.wueVisible
            color: "#b8000000"
            MouseArea { anchors.fill: parent; onClicked: win.wueVisible = false }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 40, 560)
                height: Math.min(parent.height - 60, wueCol.implicitHeight + 36)
                radius: theme.radius
                color: "#18181e"
                border.color: "#32323e"
                border.width: 1

                MouseArea { anchors.fill: parent }

                Flickable {
                    id: wueFlick
                    anchors.fill: parent
                    anchors.margins: 18
                    contentWidth: width
                    contentHeight: wueCol.implicitHeight
                    clip: true

                    ColumnLayout {
                        id: wueCol
                        width: wueFlick.width
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Windows User Experience")
                                color: theme.accent
                                font.pixelSize: 16
                                font.bold: true
                            }
                            ToolButton { text: "×"; onClicked: win.wueVisible = false }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Customize the Windows installation via autounattend.xml (ported from Rufus).")
                            color: "#9a9aa8"
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }

                        CheckBox {
                            Layout.fillWidth: true
                            text: qsTr("Enable Windows User Experience customizations")
                            checked: backend.wueEnabled
                            onToggled: backend.wueEnabled = checked
                        }

                        CheckBox {
                            Layout.fillWidth: true
                            enabled: backend.wueEnabled
                            text: qsTr("Remove requirement for 4GB+ RAM, Secure Boot and TPM 2.0")
                            checked: backend.wueBypassTpm
                            onToggled: backend.wueBypassTpm = checked
                        }

                        CheckBox {
                            Layout.fillWidth: true
                            enabled: backend.wueEnabled
                            text: qsTr("Remove requirement for an online Microsoft account")
                            checked: backend.wueBypassMsa
                            onToggled: backend.wueBypassMsa = checked
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            CheckBox {
                                enabled: backend.wueEnabled
                                text: qsTr("Create a local account:")
                                checked: backend.wueCreateLocalAccount
                                onToggled: backend.wueCreateLocalAccount = checked
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                height: 34
                                radius: Math.min(4, theme.radius)
                                color: "#1e1e26"
                                border.color: userInput.activeFocus ? theme.accent : "#32323e"
                                border.width: 1

                                TextInput {
                                    id: userInput
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    verticalAlignment: TextInput.AlignVCenter
                                    enabled: backend.wueEnabled && backend.wueCreateLocalAccount
                                    color: "#f4f4f6"
                                    font.pixelSize: 13
                                    text: backend.wueLocalUsername
                                    onTextEdited: backend.wueLocalUsername = text
                                }
                            }
                        }

                        CheckBox {
                            Layout.fillWidth: true
                            enabled: backend.wueEnabled
                            text: qsTr("Disable data collection (Skip privacy questions)")
                            checked: backend.wueDisableDataCollection
                            onToggled: backend.wueDisableDataCollection = checked
                        }

                        CheckBox {
                            Layout.fillWidth: true
                            enabled: backend.wueEnabled
                            text: qsTr("Disable BitLocker automatic device encryption")
                            checked: backend.wueDisableBitLocker
                            onToggled: backend.wueDisableBitLocker = checked
                        }

                        CheckBox {
                            Layout.fillWidth: true
                            enabled: backend.wueEnabled
                            text: qsTr("QoL tweaks (disable OneDrive, Copilot, News, classic menu)")
                            checked: backend.wueQolEnhancements
                            onToggled: backend.wueQolEnhancements = checked
                        }

                        ToolButton {
                            text: (win.wuePreviewOpen ? "▾ " : "▸ ") + qsTr("Preview autounattend.xml")
                            font.pixelSize: 12
                            onClicked: win.wuePreviewOpen = !win.wuePreviewOpen
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            visible: win.wuePreviewOpen
                            height: 140
                            radius: Math.min(4, theme.radius)
                            color: "#0f0f13"
                            border.color: "#2c2c38"
                            border.width: 1

                            ScrollView {
                                anchors.fill: parent
                                anchors.margins: 8
                                TextArea {
                                    readOnly: true
                                    text: backend.wuePreviewXml.length > 0 ? backend.wuePreviewXml : qsTr("(No options enabled)")
                                    color: "#c8c8d8"
                                    font.family: "monospace"
                                    font.pixelSize: 11
                                    background: null
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Item { Layout.fillWidth: true }
                            ActionButton {
                                text: qsTr("Done")
                                primary: true
                                implicitWidth: 96
                                onClicked: win.wueVisible = false
                            }
                        }
                    }
                }
            }
        }

        // ==================== CHECKSUMS MODAL ====================
        Rectangle {
            anchors.fill: parent
            visible: win.hashVisible
            color: "#b8000000"
            MouseArea { anchors.fill: parent; onClicked: win.hashVisible = false }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 40, 580)
                implicitHeight: hashCol.implicitHeight + 36
                radius: theme.radius
                color: "#18181e"
                border.color: "#32323e"
                border.width: 1

                MouseArea { anchors.fill: parent }

                ColumnLayout {
                    id: hashCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 18
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: qsTr("Image Checksums")
                            color: theme.accent
                            font.pixelSize: 16
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        ToolButton { text: "×"; onClicked: win.hashVisible = false }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: backend.imageName
                        color: "#c8c8d2"
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                    }

                    ProgressBar {
                        Layout.fillWidth: true
                        visible: backend.hashing
                        from: 0
                        to: 100
                        value: backend.hashProgress
                    }

                    Repeater {
                        model: [
                            { label: "MD5", value: backend.md5 },
                            { label: "SHA-1", value: backend.sha1 },
                            { label: "SHA-256", value: backend.sha256 },
                            { label: "SHA-512", value: backend.sha512 }
                        ]
                        delegate: RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Text {
                                Layout.preferredWidth: 64
                                text: modelData.label
                                color: "#9a9aa8"
                                font.pixelSize: 12
                                font.bold: true
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                height: 32
                                radius: Math.min(4, theme.radius)
                                color: "#111116"
                                border.color: "#2a2a36"
                                border.width: 1

                                Text {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    verticalAlignment: Text.AlignVCenter
                                    text: modelData.value.length > 0 ? modelData.value : (backend.hashing ? qsTr("Computing...") : "—")
                                    color: "#e6e6ee"
                                    font.family: "monospace"
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }
                            ActionButton {
                                text: qsTr("Copy")
                                enabled: modelData.value.length > 0
                                implicitHeight: 32
                                onClicked: backend.copyToClipboard(modelData.value)
                            }
                        }
                    }

                    Text {
                        text: qsTr("Verify against expected hash (MD5 / SHA-1 / SHA-256 / SHA-512)")
                        color: "#9a9aa8"
                        font.pixelSize: 11
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            Layout.fillWidth: true
                            height: 36
                            radius: Math.min(4, theme.radius)
                            color: "#111116"
                            border.color: {
                                var r = backend.verifyHash(verifyInput.text)
                                if (r === "match") return "#2fb573"
                                if (r === "mismatch") return "#d94a5a"
                                return verifyInput.activeFocus ? theme.accent : "#2a2a36"
                            }
                            border.width: 1

                            TextInput {
                                id: verifyInput
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                verticalAlignment: TextInput.AlignVCenter
                                color: "#f4f4f6"
                                font.family: "monospace"
                                font.pixelSize: 11
                            }
                        }
                        Text {
                            readonly property string matchState: backend.verifyHash(verifyInput.text)
                            visible: matchState.length > 0
                            text: matchState === "match" ? qsTr("MATCH") : qsTr("MISMATCH")
                            color: matchState === "match" ? "#2fb573" : "#ff6b7a"
                            font.pixelSize: 12
                            font.bold: true
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Item { Layout.fillWidth: true }
                        ActionButton {
                            text: backend.hashing ? qsTr("Cancel") : qsTr("Recompute")
                            onClicked: {
                                if (backend.hashing) backend.cancelChecksums()
                                else backend.computeChecksums()
                            }
                        }
                        ActionButton {
                            text: qsTr("Close")
                            primary: true
                            onClicked: win.hashVisible = false
                        }
                    }
                }
            }
        }

        // ==================== LOG VIEWER MODAL ====================
        Rectangle {
            anchors.fill: parent
            visible: win.logVisible
            color: "#b8000000"
            MouseArea { anchors.fill: parent; onClicked: win.logVisible = false }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 40, 620)
                height: Math.min(parent.height - 60, 480)
                radius: theme.radius
                color: "#18181e"
                border.color: "#32323e"
                border.width: 1

                MouseArea { anchors.fill: parent }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: qsTr("Log")
                            color: theme.accent
                            font.pixelSize: 16
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        ToolButton { text: "×"; onClicked: win.logVisible = false }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: Math.min(4, theme.radius)
                        color: "#0e0e12"
                        border.color: "#2a2a36"
                        border.width: 1

                        ScrollView {
                            anchors.fill: parent
                            anchors.margins: 8
                            TextArea {
                                readOnly: true
                                text: backend.logText
                                color: "#d4d4df"
                                font.family: "monospace"
                                font.pixelSize: 11
                                wrapMode: TextEdit.WrapAnywhere
                                background: null
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        ActionButton {
                            text: qsTr("Copy")
                            onClicked: backend.copyToClipboard(backend.logText)
                        }
                        ActionButton {
                            text: qsTr("Save")
                            onClicked: backend.openSaveLogDialog()
                        }
                        ActionButton {
                            text: qsTr("Clear")
                            onClicked: backend.clearLog()
                        }
                        Item { Layout.fillWidth: true }
                        ActionButton {
                            text: qsTr("Close")
                            primary: true
                            onClicked: win.logVisible = false
                        }
                    }
                }
            }
        }

        // ==================== KEYBOARD SHORTCUTS OVERLAY ====================
        Rectangle {
            anchors.fill: parent
            visible: win.helpVisible
            color: "#b8000000"
            MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 440)
                implicitHeight: helpCol.implicitHeight + 36
                radius: theme.radius
                color: "#18181e"
                border.color: "#32323e"
                border.width: 1

                MouseArea { anchors.fill: parent }

                ColumnLayout {
                    id: helpCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 18
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: qsTr("Keyboard Shortcuts")
                            color: theme.accent
                            font.pixelSize: 16
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        ToolButton { text: "×"; onClicked: win.helpVisible = false }
                    }

                    Repeater {
                        model: [
                            { key: "O", desc: qsTr("Select ISO or disk image") },
                            { key: "R", desc: qsTr("Refresh USB devices") },
                            { key: "Space", desc: qsTr("Start / Cancel operation") },
                            { key: "H", desc: qsTr("Compute image checksums") },
                            { key: "W", desc: qsTr("Windows User Experience options") },
                            { key: "S", desc: qsTr("Save drive to raw .img file") },
                            { key: "L", desc: qsTr("Toggle operation log") },
                            { key: "?", desc: qsTr("Show or hide this help") },
                            { key: "Esc", desc: qsTr("Close active dialog") },
                            { key: "Q", desc: qsTr("Quit Rufus") }
                        ]
                        delegate: RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            Rectangle {
                                Layout.preferredWidth: 64
                                height: 24
                                radius: Math.min(4, theme.radius)
                                color: "#262632"
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData.key
                                    color: theme.accent
                                    font.family: "monospace"
                                    font.pixelSize: 11
                                    font.bold: true
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.desc
                                color: "#e0e0ea"
                                font.pixelSize: 12
                            }
                        }
                    }
                }
            }
        }
    }
}
