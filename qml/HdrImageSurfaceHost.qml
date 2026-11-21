import QtQuick
import Licasa

HdrImageItem {
    readonly property string photoStatusText: {
        switch (photoStatus) {
        case HdrImageItem.Sdr: return qsTr("Photo shown in SDR")
        case HdrImageItem.Preparing: return qsTr("Preparing HDR photo")
        case HdrImageItem.Unavailable: return qsTr("HDR photo unavailable. Showing SDR.")
        case HdrImageItem.Hdr: return qsTr("Photo shown in HDR")
        default: return ""
        }
    }
}
