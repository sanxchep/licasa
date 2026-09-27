import QtQuick
import QtTest
import "../qml/FileUrls.js" as FileUrls

TestCase {
    name: "FileUrls"

    function test_roundTripReservedCharacters() {
        const path = "/tmp/folder with spaces/日本語 #100%?.jpg"
        const url = FileUrls.fromLocalPath(path)
        verify(FileUrls.isLocal(url))
        compare(FileUrls.localPath(url), path)
        compare(FileUrls.fileName(url), "日本語 #100%?.jpg")
        compare(FileUrls.localPath(FileUrls.suggestedCopy(url, "frame", "png")),
                "/tmp/folder with spaces/日本語 #100%?-frame.png")
    }

    function test_suffixAndFallback_data() {
        return [
            { tag: "jpeg", source: "file:///tmp/photo.JPEG", format: "original", expected: "file:///tmp/photo-edited.jpeg" },
            { tag: "raw", source: "file:///tmp/photo.CR2", format: "original", expected: "file:///tmp/photo-edited.png" },
            { tag: "override", source: "file:///tmp/photo.png", format: "jpeg", expected: "file:///tmp/photo-edited.jpg" },
            { tag: "no-extension", source: "file:///tmp/photo", format: "png", expected: "file:///tmp/photo-edited.png" },
            { tag: "dot-file", source: "file:///tmp/.photo", format: "png", expected: "file:///tmp/.photo-edited.png" },
            { tag: "multi-dot", source: "file:///tmp/photo.v2.GIF", format: "webp", expected: "file:///tmp/photo.v2-edited.webp" }
        ]
    }

    function test_suffixAndFallback(data) {
        compare(FileUrls.suggestedEditedCopy(data.source, data.format), data.expected)
    }

    function test_invalidSourcesHaveNoSuggestion() {
        for (const source of [undefined, null, "", "https://example.org/image.jpg", "file:///tmp/%ZZ"]) {
            compare(FileUrls.localPath(source), "")
            compare(FileUrls.suggestedCopy(source, "motion", "mp4"), "")
        }
    }

    function test_literalPathsAreNotUrlDecoded() {
        const path = "/tmp/100% and %20 #? back\\slash.jpg"
        compare(FileUrls.localPath(path), path)
        compare(FileUrls.localPath(FileUrls.fromLocalPath(path)), path)
        compare(FileUrls.fileName(path), "100% and %20 #? back\\slash.jpg")
        compare(FileUrls.localPath(FileUrls.suggestedCopy(path, "frame", "png")),
                "/tmp/100% and %20 #? back\\slash-frame.png")
    }

    function test_urlComponentsStayOutOfTheFilename() {
        compare(FileUrls.localPath("file:///tmp/a%23b%3Fc.jpg?query=1#fragment"),
                "/tmp/a#b?c.jpg")
        compare(FileUrls.localPath("FILE:///tmp/a.jpg"), "/tmp/a.jpg")
        compare(FileUrls.localPath("file://server/share/a.jpg"), "//server/share/a.jpg")
        compare(FileUrls.localPath("file://"), "")
        compare(FileUrls.fromLocalPath("relative.jpg"), "")
    }
}
