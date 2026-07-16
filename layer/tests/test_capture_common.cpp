#include <gtest/gtest.h>
#include "capture_common.h"
#include <vector>
#include <string>
#include <cstring>

using namespace vr_agent;
using json = nlohmann::json;

/*
このテストは `RepackRows` 関数に対して、パディング(rowPitch > width * 4)が含まれている場合でも
正しく各行が抽出され、詰まった(tightly-packed)バッファが返されるかを検査します。
理由: 境界値となるパディングサイズや、行の先頭と末尾でのメモリ操作を間違えると、
画像が斜めにずれたり、バッファオーバーランでクラッシュする可能性があるため。正常系の代表例です。
*/
TEST(CaptureCommonTest, RepackRows_RemovesPadding) {
    const uint32_t w = 2;
    const uint32_t h = 2;
    // 1ピクセル4バイト。w=2なら本来1行8バイトだが、rowPitch=12として4バイトのパディングを入れる。
    const size_t rowPitch = 12;
    std::vector<unsigned char> src = {
        1, 2, 3, 4,   5, 6, 7, 8,   255, 255, 255, 255, // Row 1 (with 4 bytes padding)
        9, 10, 11, 12, 13, 14, 15, 16, 255, 255, 255, 255  // Row 2
    };

    auto dst = RepackRows(src.data(), rowPitch, w, h, false);

    ASSERT_EQ(dst.size(), w * h * 4);
    std::vector<unsigned char> expected = {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16
    };
    EXPECT_EQ(dst, expected);
}

/*
このテストは `RepackRows` 関数において、`bgra=true` と指定された場合、
各ピクセルのBとR（0番目と2番目のバイト）がスワップされるかを検査します。
理由: OpenXRキャプチャにおいてBGRAフォーマットをRGBAに正しく変換できないと、
保存された画像の色が青と赤で反転してしまう不具合が起きるため。
*/
TEST(CaptureCommonTest, RepackRows_SwizzlesBGRA) {
    const uint32_t w = 2;
    const uint32_t h = 1;
    const size_t rowPitch = 8;
    // (B, G, R, A)
    std::vector<unsigned char> src = {
        30, 20, 10, 255, // Pixel 1: Blue=30, Red=10
        60, 50, 40, 255  // Pixel 2: Blue=60, Red=40
    };

    auto dst = RepackRows(src.data(), rowPitch, w, h, true);

    ASSERT_EQ(dst.size(), 8);
    std::vector<unsigned char> expected = {
        10, 20, 30, 255, // (R, G, B, A)
        40, 50, 60, 255
    };
    EXPECT_EQ(dst, expected);
}

/*
このテストは `BuildCaptureSuccessJson` が、API固有のパラメータを正しくJSON構造に
マッピングして返すかを検査します。
理由: 多くの情報をもつJSONの生成処理で、型の間違い(stringがintになる等)や
キー名のタイポがあると、TypeScript側のMCPサーバーでパースエラーや情報欠落が起きるため。
*/
TEST(CaptureCommonTest, BuildCaptureSuccessJson_FormatsCorrectly) {
    auto j = BuildCaptureSuccessJson("C:\\temp\\test.png", "right", 1, "D3D11", 1920, 1080, 0, 87);

    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_EQ(j["path"].get<std::string>(), "C:\\temp\\test.png");
    EXPECT_EQ(j["eye"].get<std::string>(), "right");
    EXPECT_EQ(j["viewIndex"].get<int>(), 1);
    EXPECT_EQ(j["api"].get<std::string>(), "D3D11");
    EXPECT_EQ(j["width"].get<uint32_t>(), 1920);
    EXPECT_EQ(j["height"].get<uint32_t>(), 1080);
    EXPECT_EQ(j["arrayIndex"].get<uint32_t>(), 0);
    EXPECT_EQ(j["format"].get<int64_t>(), 87);
}

/*
このテストは `DecodeHdrRowsToSrgb` (R10) が、行パディング(rowPitch > w*8)を含む
R16G16B16A16_FLOAT バッファを正しく 8-bit RGBA へ decode するかを検査します。
理由: 本関数の新規リスクはまさに行ピッチ処理(Vulkan のタイト詰めループとの唯一の差分)で、
ピッチずれは「斜行した壊れ画像」を無言で生むため。値は sRGB 変換の丸めに依存しない
端点(linear 0.0 -> 0, linear 1.0 -> 255)のみを使います(half: 0x0000 / 0x3C00)。
*/
TEST(CaptureCommonTest, DecodeHdrRowsToSrgb_HonorsRowPitchAndEndpoints) {
    const uint32_t w = 2, h = 2;
    const size_t rowPitch = w * 8 + 8;  // 8 bytes of padding per row
    std::vector<unsigned char> src(rowPitch * h, 0xAB);  // sentinel padding

    const uint16_t kOne = 0x3C00, kZero = 0x0000;
    // texel = 4 half (R,G,B,A)
    const uint16_t texels[2][2][4] = {
        {{kOne, kZero, kZero, kOne},  {kZero, kOne, kZero, kZero}},
        {{kZero, kZero, kOne, kOne},  {kOne, kOne, kOne, kOne}},
    };
    for (uint32_t r = 0; r < h; ++r)
        for (uint32_t p = 0; p < w; ++p)
            std::memcpy(src.data() + r * rowPitch + p * 8, texels[r][p], 8);

    auto dst = DecodeHdrRowsToSrgb(src.data(), rowPitch, w, h);

    ASSERT_EQ(dst.size(), static_cast<size_t>(w) * h * 4);
    const std::vector<unsigned char> expected = {
        255, 0,   0,   255,   // red, opaque
        0,   255, 0,   0,     // green, transparent
        0,   0,   255, 255,   // blue, opaque
        255, 255, 255, 255,   // white, opaque
    };
    EXPECT_EQ(dst, expected);
}
