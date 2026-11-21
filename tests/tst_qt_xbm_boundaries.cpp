#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QtGlobal>

#include <array>
#include <cstdio>

static bool check(const char* name, const QByteArray& body, bool valid = true)
{
    QByteArray source = "#define sample_width 16\n"
                        "#define sample_height 2\n"
                        "static unsigned char sample_bits[] = {\n" +
                        body + "};\n";
    QBuffer buffer(&source);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return false;
    }
    QImageReader reader(&buffer, "xbm");
    const QImage image = reader.read();
    if (!valid) {
        return image.isNull();
    }
    if (image.size() != QSize(16, 2)) {
        std::fprintf(stderr, "%s: missing or wrong-sized raster\n", name);
        return false;
    }

    constexpr std::array<unsigned, 4> expected = {0x81, 0xa6, 0x42, 0x19};
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const bool black = (expected[2 * y + x / 8] >> (x % 8)) & 1;
            const QRgb pixel = black ? qRgb(0, 0, 0) : qRgb(255, 255, 255);
            if (image.pixel(x, y) != pixel) {
                std::fprintf(stderr, "%s: wrong pixel at (%d, %d)\n", name, x, y);
                return false;
            }
        }
    }
    return true;
}

int main()
{
    const QByteArray tokens = "0x81,0xA6,\n /* separator */\t0x42, 0x19\n";
    int failed = 0;
    const auto test = [&failed](const char* name, const QByteArray& body, bool valid = true) {
        const bool passed = check(name, body, valid);
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
        failed += !passed;
    };

    test("ordinary-line", tokens);
    test("token-starts-at-boundary", QByteArray(299, ' ') + tokens);
    test("boundary-after-0", QByteArray(298, ' ') + tokens);
    test("boundary-after-x", QByteArray(297, ' ') + tokens);
    test("boundary-after-high-nibble", QByteArray(296, ' ') + tokens);
    test("boundary-after-complete-token", QByteArray(295, ' ') + tokens);
    test("boundary-in-second-token", QByteArray(293, ' ') + tokens);
    test("newline-is-not-a-read-boundary", QByteArray(297, ' ') + "0x\n81,\n" + tokens);
    test("bad-hex-in-split-token", QByteArray(296, ' ') + "0x8Z," + tokens, false);
    return failed ? 1 : 0;
}
