import QtQuick
import QtTest
import "../qml"

TestCase {
    name: "EditorSessionHistory"

    Component {
        id: sessionComponent
        EditorSession {}
    }

    function test_equalSnapshotsIgnorePropertyOrder() {
        const session = createTemporaryObject(sessionComponent, this)
        const current = session.snapshot()
        const reordered = {}
        for (const key of Object.keys(current).reverse())
            reordered[key] = current[key]
        verify(session.snapshotsEqual(current, reordered))
        session.commitCheckpoint(reordered)
        verify(!session.canUndo)
        reordered.exposure = 0.25
        verify(!session.snapshotsEqual(current, reordered))
    }

    function test_presetPreservesGeometryAndResetIsUndoable() {
        const session = createTemporaryObject(sessionComponent, this)
        session.cropX = 0.2
        session.cropWidth = 0.5
        session.quarterTurns = 1
        session.flipHorizontal = true
        session.applyPreset("soft")
        compare(session.cropX, 0.2)
        compare(session.cropWidth, 0.5)
        compare(session.quarterTurns, 1)
        compare(session.flipHorizontal, true)
        const values = session.editValues()
        compare(values.soften, session.blur)
        verify(values.blur === undefined)
        values.cropX = 0.9
        compare(session.cropX, 0.2)
        session.reset()
        verify(!session.hasAdjustments)
        session.undo()
        compare(session.cropX, 0.2)
        compare(session.blur, 0.08)
        session.redo()
        verify(!session.hasAdjustments)
    }

    function test_historyRemainsBoundedAndNewEditsDiscardRedo() {
        const session = createTemporaryObject(sessionComponent, this)
        for (let i = 1; i <= 60; ++i) {
            session.beginInteraction()
            session.setAdjustment("exposure", i / 100)
            session.finishInteraction()
        }
        compare(session.undoStack.length, 50)
        session.undo()
        verify(session.canRedo)
        session.beginInteraction()
        session.setAdjustment("contrast", 0.4)
        session.finishInteraction()
        verify(!session.canRedo)
        compare(session.undoStack.length, 50)
    }
}
