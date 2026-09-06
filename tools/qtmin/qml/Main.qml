import QtQuick
import QtQuick.Window
import com.min

Window {
    visible: true
    width: 320
    height: 200
    title: "qtmin"
    Thing { id: thing }
    Text { anchors.centerIn: parent; text: thing.label }
}
