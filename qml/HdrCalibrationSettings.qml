pragma ComponentBehavior: Bound
import QtQuick
import "components"

SectionCard {
    id: root
    required property var controller
    title: qsTr("HDR display")
    readonly property var settings: controller ? controller.outputSettings : ({})
    property string editRevision: ""
    property real paperWhite: 200
    property real peak: 800
    property string submissionError: ""
    readonly property bool validValues: paperWhite >= 80 && paperWhite <= 500
        && peak > paperWhite && peak <= 10000

    function reload() {
        if (String(settings.revision || "") === editRevision) return
        editRevision = String(settings.revision || "")
        paperWhite = Number(settings.paperWhite || 200)
        peak = Number(settings.peak || 800)
        submissionError = ""
    }
    function save(remember) {
        if (!controller || !validValues) return
        const accepted = controller.saveHdrCalibration(editRevision, paperWhite, peak, remember)
        if (!accepted) submissionError = qsTr("The display changed or settings could not be saved. Review the current monitor and try again.")
        else submissionError = ""
    }
    onSettingsChanged: reload()
    Component.onCompleted: reload()

    Text {
        objectName: "hdrMonitorName"
        width: parent.width
        text: root.settings.monitorName || qsTr("No monitor")
        color: "white"; font.pixelSize: 14; font.weight: Font.Medium
        wrapMode: Text.Wrap
    }
    Text {
        objectName: "hdrFallbackStatus"
        width: parent.width
        text: root.settings.message || ""
        color: "#E6FFFFFF"; font.pixelSize: 13; wrapMode: Text.Wrap
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
    Text {
        width: parent.width
        text: root.settings.identityMessage || ""
        color: "#B3FFFFFF"; font.pixelSize: 12; wrapMode: Text.Wrap
    }
    ValueRow {
        objectName: "hdrPaperWhite"
        width: parent.width
        title: qsTr("SDR white brightness")
        subtitle: qsTr("Brightness of white controls and SDR content")
        from: 80; to: 500; step: 10
        value: root.paperWhite
        displayMultiplier: 1; displayDecimals: 0
        displaySuffix: qsTr(" nits"); accessibleUnit: qsTr("nits")
        onValueChangedByUser: function(value) { root.paperWhite = value }
    }
    ValueRow {
        objectName: "hdrPeakBrightness"
        width: parent.width
        title: qsTr("HDR peak brightness")
        subtitle: qsTr("Use the HDR peak brightness specified for this monitor")
        from: 100; to: 10000; step: 50
        value: root.peak
        displayMultiplier: 1; displayDecimals: 0
        displaySuffix: qsTr(" nits"); accessibleUnit: qsTr("nits")
        onValueChangedByUser: function(value) { root.peak = value }
    }
    Text {
        width: parent.width
        visible: !root.validValues
        text: qsTr("Peak brightness must be greater than SDR white brightness.")
        color: "#FFB4AB"; font.pixelSize: 12; wrapMode: Text.Wrap
    }
    ActionChip {
        objectName: "hdrSaveMonitor"
        width: parent.width
        text: qsTr("Save for this monitor")
        enabled: root.validValues && root.settings.canRemember === true
        onClicked: root.save(true)
    }
    ActionChip {
        objectName: "hdrUseSession"
        width: parent.width
        text: qsTr("Use for this session")
        enabled: root.validValues && root.editRevision.length > 0
        onClicked: root.save(false)
    }
    ActionChip {
        objectName: "hdrUseSdr"
        width: parent.width
        text: root.settings.canRemember === true ? qsTr("Use SDR on this monitor") : qsTr("Use SDR for this session")
        onClicked: {
            if (!root.controller.useSdr(root.editRevision)) root.submissionError = qsTr("The display changed. Review the current monitor and try again.")
        }
    }
    ActionChip {
        objectName: "hdrForgetMonitor"
        visible: root.settings.hasProfile === true
        width: parent.width
        text: qsTr("Forget saved calibration")
        onClicked: {
            if (!root.controller.forgetHdrCalibration(root.editRevision)) root.submissionError = qsTr("Saved calibration could not be removed.")
        }
    }
    Text {
        objectName: "hdrCalibrationError"
        width: parent.width
        text: root.submissionError || root.settings.error || ""
        visible: text.length > 0
        color: "#FFB4AB"; font.pixelSize: 12; wrapMode: Text.Wrap
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
}
