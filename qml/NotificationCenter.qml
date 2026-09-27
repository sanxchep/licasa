import QtQuick
import "components"

Item {
    id: root

    property bool appVisible: true
    property bool fullScreenMode: true
    visible: appVisible && fullScreenMode
    onFullScreenModeChanged: {
        if (!fullScreenMode)
            historyOpen = false
    }
    property real cornerControlMargin: 24
    property real closeButtonSize: 48
    property bool historyOpen: false
    property bool sessionActive: true
    property bool toastVisible: false
    property string latestCategory: ""
    property string toastCategory: ""
    property string toastTitle: ""
    property string toastDetail: ""
    property var notifications: []
    readonly property int notificationCount: notifications.length
    readonly property bool anyLoading: {
        for (let index = 0; index < notifications.length; ++index) {
            if (notifications[index].ongoing)
                return true
        }
        return false
    }

    function categoryColor(category) {
        if (category === "success")
            return "#42D99B"
        if (category === "error")
            return "#FF727B"
        return "#F5C451"
    }

    function pendingIndex(key) {
        for (let index = 0; index < notifications.length; ++index) {
            const entry = notifications[index]
            if (entry.ongoing && entry.operationKey === key)
                return index
        }
        return -1
    }

    function isLoading(key) {
        return pendingIndex(key) >= 0
    }

    function clear() {
        hideTimer.stop()
        notifications = []
        historyOpen = false
        toastVisible = false
        latestCategory = ""
        toastCategory = ""
        toastTitle = ""
        toastDetail = ""
    }

    function endSession() {
        sessionActive = false
        clear()
    }

    function startSession() {
        if (sessionActive)
            return
        clear()
        sessionActive = true
    }

    function showToast(category, title, detail, ongoing) {
        toastCategory = category
        toastTitle = title
        toastDetail = detail
        toastVisible = true
        hideTimer.stop()
        if (!ongoing)
            hideTimer.start()
    }

    function begin(key, title, detail) {
        if (!sessionActive)
            return
        const index = pendingIndex(key)
        if (index >= 0) {
            const entries = notifications.slice()
            entries[index] = Object.assign({}, entries[index], { "title": title, "detail": detail })
            notifications = entries
        } else {
            const entries = notifications.slice()
            entries.unshift({
                "operationKey": key,
                "category": "warning",
                "title": title,
                "detail": detail,
                "timeText": Qt.formatTime(new Date(), "hh:mm"),
                "ongoing": true
            })
            notifications = entries
        }
        latestCategory = "warning"
        showToast("warning", title, detail, true)
    }

    function update(key, title, detail) {
        if (!sessionActive)
            return
        const index = pendingIndex(key)
        if (index < 0)
            return
        const entries = notifications.slice()
        entries[index] = Object.assign({}, entries[index], { "title": title, "detail": detail })
        notifications = entries
        showToast("warning", title, detail, true)
    }

    function finish(key, category, title, detail) {
        if (!sessionActive)
            return
        const index = pendingIndex(key)
        const entries = notifications.slice()
        if (index >= 0)
            entries[index] = Object.assign({}, entries[index], { "ongoing": false })
        entries.unshift({
            "operationKey": "",
            "category": category,
            "title": title,
            "detail": detail,
            "timeText": Qt.formatTime(new Date(), "hh:mm"),
            "ongoing": false
        })
        notifications = entries
        latestCategory = category
        showToast(category, title, detail, false)
    }

    function stop(key) {
        const index = pendingIndex(key)
        if (index < 0)
            return
        const entries = notifications.slice()
        entries[index] = Object.assign({}, entries[index], { "ongoing": false })
        notifications = entries
        if (toastCategory === "warning") {
            hideTimer.stop()
            toastVisible = false
        }
    }

    function showPendingOrHide() {
        for (let index = 0; index < notifications.length; ++index) {
            const entry = notifications[index]
            if (entry.ongoing) {
                showToast("warning", entry.title, entry.detail, true)
                return
            }
        }
        toastVisible = false
    }

    Timer {
        id: hideTimer
        interval: 2000
        repeat: false
        onTriggered: root.showPendingOrHide()
    }

    RoundIconButton {
        id: historyButton
        objectName: "notificationHistoryButton"
        width: root.fullScreenMode ? 42 : 34
        height: width
        x: root.fullScreenMode
            ? root.width - root.cornerControlMargin - root.closeButtonSize - 12 - width
            : 8
        y: root.fullScreenMode ? root.cornerControlMargin + 3 : 8
        visible: root.appVisible
        iconName: "notifications"
        accessibleName: "Notifications"
        checked: root.historyOpen
        onClicked: root.historyOpen = !root.historyOpen

        Rectangle {
            objectName: "notificationIndicator"
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: -3
            anchors.topMargin: -3
            width: 11
            height: width
            radius: width / 2
            visible: root.notificationCount > 0
            color: root.categoryColor(root.anyLoading ? "warning" : root.latestCategory)
            border.width: 2
            border.color: "#202126"
        }
    }

    Rectangle {
        id: toast
        objectName: "notificationToast"
        width: Math.max(0, Math.min(360, root.width - 16))
        height: toastContent.implicitHeight + 24
        x: root.fullScreenMode
            ? Math.max(8, historyButton.x + historyButton.width - width)
            : 8
        y: historyButton.y + historyButton.height + 10
        visible: root.appVisible && root.toastVisible && !root.historyOpen
            && root.width >= 180 && root.height >= 100
        radius: 16
        color: "#F21A1D22"
        border.width: 1
        border.color: root.categoryColor(root.toastCategory)

        Rectangle {
            x: 12
            y: 17
            width: 9
            height: width
            radius: width / 2
            color: root.categoryColor(root.toastCategory)
        }

        Column {
            id: toastContent
            x: 30
            y: 12
            width: parent.width - 42
            spacing: 3

            Text {
                width: parent.width
                text: root.toastTitle
                color: "#FFFFFF"
                font.pixelSize: 13
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }

            Text {
                width: parent.width
                text: root.toastDetail
                visible: text.length > 0
                color: "#C6CDD4"
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
        }
    }

    Rectangle {
        id: historyPanel
        objectName: "notificationHistoryPanel"
        width: Math.max(0, Math.min(380, root.width - 16))
        height: Math.max(0, Math.min(420, root.height - y - 8))
        x: root.fullScreenMode
            ? Math.max(8, historyButton.x + historyButton.width - width)
            : 8
        y: historyButton.y + historyButton.height + 10
        visible: root.appVisible && root.historyOpen
        radius: 18
        color: "#F21A1D22"
        border.width: 1
        border.color: "#4BFFFFFF"
        clip: true

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }

        Text {
            id: historyTitle
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 18
            text: "Notifications"
            color: "white"
            font.pixelSize: 17
            font.weight: Font.DemiBold
        }

        Text {
            id: emptyHistory
            anchors.centerIn: parent
            visible: root.notificationCount === 0
            text: "No notifications yet"
            color: "#BAC1CA"
            font.pixelSize: 12
        }

        ListView {
            id: historyList
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: historyTitle.bottom
            anchors.bottom: parent.bottom
            anchors.margins: 12
            anchors.topMargin: 14
            clip: true
            spacing: 7
            model: root.notifications

            delegate: Rectangle {
                id: notificationEntry
                required property var modelData
                width: ListView.view.width
                height: entryText.implicitHeight + 24
                radius: 12
                color: "#20FFFFFF"

                Rectangle {
                    x: 11
                    y: 15
                    width: 8
                    height: width
                    radius: width / 2
                    color: notificationEntry.modelData.category === "success" ? "#42D99B"
                        : notificationEntry.modelData.category === "error" ? "#FF727B"
                        : "#F5C451"
                }

                Column {
                    id: entryText
                    x: 28
                    y: 12
                    width: parent.width - 80
                    spacing: 3

                    Text {
                        width: parent.width
                        text: notificationEntry.modelData.title
                        color: "white"
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                    }

                    Text {
                        width: parent.width
                        text: notificationEntry.modelData.detail
                        visible: text.length > 0
                        color: "#C6CDD4"
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                }

                Text {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 12
                    text: notificationEntry.modelData.timeText
                    color: "#9EA8B2"
                    font.pixelSize: 10
                }
            }
        }
    }
}
