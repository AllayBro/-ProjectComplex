import QtQuick
import QtQuick.Controls
import QtLocation
import QtPositioning

Item {
    property var pointsModel: []

    Map {
        anchors.fill: parent
        plugin: Plugin { name: "osm" }

        Repeater {
            model: pointsModel
            delegate: MapQuickItem {
                coordinate: QtPositioning.coordinate(modelData.lat, modelData.lon)
                anchorPoint.x: 6
                anchorPoint.y: 6
                sourceItem: Rectangle {
                    width: 12
                    height: 12
                    radius: 6
                    color: "red"
                }
            }
        }
    }
}