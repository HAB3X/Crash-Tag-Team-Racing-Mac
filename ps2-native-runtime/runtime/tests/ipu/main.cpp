// The IPU's test on a real movie: ps2_ipu_movie MOVIE.sfd OUTDIR [--frames N] [--png-every K] [--ring-blocks B]
//                                [--reference FILE.yuv]
//
// The movie's video is decoded through the runtime's IPU as a PS2 player does it (player.h, ee.h), each picture converted by
// the IPU's CSC and saved as a PNG every K pictures (OUTDIR/frameNNNNN.png). With --reference (the same movie decoded by
// another decoder to raw yuv420p, e.g. ffmpeg -f mpeg -i MOVIE.sfd -f rawvideo -pix_fmt yuv420p FILE.yuv) every picture's
// planes are compared with it (PSNR). Also: the inverse DCT against IEEE 1180's accuracy limits, IDEC against BDEC + CSC on
// the first I picture, PACK's RGB16 and INDX4 against their definitions. Prints the time the IPU took a picture. Exits
// non-zero on any failure
#include "../../src/ipu/mpeg.h"
#include "../../src/runtime.h"
#include "player.h"
#include "sfd.h"

#include "ee/vu1jit.h"
#include "png.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>

using namespace IpuTest;

namespace
{
std::vector<u8> ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

// IEEE 1180-1990: random blocks of samples in [-L, H] through a double precision forward DCT, rounded and clamped to 12 bits,
// then the inverse under test against the double precision inverse (both rounded, clamped to [-256, 255])
bool Ieee1180()
{
    struct Range
    {
        int low, high, sign;
    };
    const Range ranges[] = {{256, 255, 1}, {5, 5, 1}, {300, 300, 1}, {256, 255, -1}, {5, 5, -1}, {300, 300, -1}};
    bool ok = true;
    for (const Range& r : ranges)
    {
        std::mt19937 generator(1);
        auto random = [&](long low, long high) { return std::uniform_int_distribution<long>(-low, high)(generator); };
        double peak = 0, sumError[64] = {}, sumSquare[64] = {};
        const int blocks = 10000;
        for (int n = 0; n < blocks; n++)
        {
            double samples[64];
            for (double& s : samples)
            {
                s = static_cast<double>(random(r.low, r.high) * r.sign);
            }
            s32 coef[64];
            for (int v = 0; v < 8; v++)
            {
                for (int u = 0; u < 8; u++)
                {
                    double sum = 0;
                    for (int y = 0; y < 8; y++)
                    {
                        for (int x = 0; x < 8; x++)
                        {
                            sum += samples[y * 8 + x] * std::cos((2 * x + 1) * u * M_PI / 16) * std::cos((2 * y + 1) * v * M_PI / 16);
                        }
                    }
                    double cu = u == 0 ? std::sqrt(0.5) : 1.0, cv = v == 0 ? std::sqrt(0.5) : 1.0;
                    coef[v * 8 + u] = static_cast<s32>(std::clamp(std::lround(sum * cu * cv / 4), -2048L, 2047L));
                }
            }
            double reference[64];
            Ps2::Ipu::InverseDctReference(coef, reference);
            s32 test[64];
            std::copy(coef, coef + 64, test);
            Ps2::Ipu::InverseDct(test);
            for (int i = 0; i < 64; i++)
            {
                double want = std::clamp(static_cast<double>(std::lround(reference[i])), -256.0, 255.0);
                double got = std::clamp(static_cast<double>(test[i]), -256.0, 255.0);
                double e = got - want;
                peak = std::max(peak, std::fabs(e));
                sumError[i] += e;
                sumSquare[i] += e * e;
            }
        }
        double worstMse = 0, worstMean = 0, totalMse = 0, totalMean = 0;
        for (int i = 0; i < 64; i++)
        {
            worstMse = std::max(worstMse, sumSquare[i] / blocks);
            worstMean = std::max(worstMean, std::fabs(sumError[i] / blocks));
            totalMse += sumSquare[i];
            totalMean += sumError[i];
        }
        totalMse /= 64.0 * blocks;
        totalMean = std::fabs(totalMean / (64.0 * blocks));
        bool pass = peak <= 1 && worstMse <= 0.06 && totalMse <= 0.02 && worstMean <= 0.015 && totalMean <= 0.0015;
        printf("IEEE 1180 [-%d, %d] sign %+d: peak %.0f, pixel MSE %.4f (0.06), overall MSE %.4f (0.02), pixel mean %.4f "
               "(0.015), overall mean %.5f (0.0015): %s\n",
               r.low, r.high, r.sign, peak, worstMse, totalMse, worstMean, totalMean, pass ? "pass" : "FAIL");
        ok &= pass;
    }
    s32 zero[64] = {};
    Ps2::Ipu::InverseDct(zero);
    for (s32 v : zero)
    {
        ok &= v == 0;
    }
    return ok;
}

double Psnr(const std::vector<u8>& a, const u8* b, int* maxDiff)
{
    double se = 0;
    int peak = 0;
    for (size_t i = 0; i < a.size(); i++)
    {
        int d = int(a[i]) - int(b[i]);
        se += d * d;
        peak = std::max(peak, std::abs(d));
    }
    *maxDiff = std::max(*maxDiff, peak);
    if (se == 0)
    {
        return 99;
    }
    return 10 * std::log10(255.0 * 255.0 * a.size() / se);
}

// IDEC on the first I picture's slices, each fed by itself through toIPU (normal mode), against BDEC + CSC's RGB32
bool CheckIdec(const std::vector<u8>& video, const std::vector<u8>& firstRgb, int width, u32 ctrl, int& macroblocksChecked)
{
    size_t picture = 0;
    for (size_t i = 0; i + 6 < video.size(); i++)
    {
        if (video[i] == 0 && video[i + 1] == 0 && video[i + 2] == 1 && video[i + 3] == 0 && ((video[i + 5] >> 3) & 7) == 1)
        {
            picture = i;
            break;
        }
    }
    int mbw = width / 16;
    bool ok = true;
    for (size_t i = picture + 4; i + 4 < video.size(); i++)
    {
        if (video[i] != 0 || video[i + 1] != 0 || video[i + 2] != 1)
        {
            continue;
        }
        u32 code = video[i + 3];
        if (code == 0 || code > 0xAF)
        {
            if (code == 0 || code == 0xB3 || code == 0xB8 || code == 0xB7)
            {
                break; // the next picture
            }
            continue;
        }
        size_t end = i + 4;
        while (end + 3 < video.size() && !(video[end] == 0 && video[end + 1] == 0 && video[end + 2] == 1))
        {
            end++;
        }
        size_t bytes = end + 4 - i; // the next start code too, for IDEC's end check
        std::memset(Guest(SpareBase), 0, bytes + 64);
        std::memcpy(Guest(SpareBase), video.data() + i, bytes);
        Write32(0x1000B400, 0);
        Write32(0x1000B000, 0);
        Write32(0x10002000, 0); // BCLR
        Write32(0x10002010, ctrl);
        Write32(0x1000B410, SpareBase);
        Write32(0x1000B420, u32((bytes + 15) / 16 + 1));
        Write32(0x1000B400, 0x101);
        Write32(0x10002000, 0x40000020); // FDEC: past the start code
        u32 top = Read32(0x10002030);
        u32 qsc = top >> 27;
        if ((top >> 26) & 1)
        {
            printf("IDEC check: slice %02x has extra slice information (not done)\n", code);
            return false;
        }
        Write32(0x10002000, 0x40000006); // FDEC: past quantiser_scale_code and extra_bit_slice
        Write32(0x10002000, 0x30000000); // VDEC: the first macroblock's address increment
        int column = int(Read32(0x10002000) & 0xFFFF) - 1;
        u32 outAt = SpareBase + 0x80000;
        Write32(0x1000B010, outAt);
        Write32(0x1000B020, 0xFFFF);
        Write32(0x1000B000, 0x100);
        Write32(0x10002000, 0x10000000u | (qsc << 16)); // IDEC: RGB32, no DT, no sign
        u32 ctrlAfter = Read32(0x10002010);
        int count = int((Read32(0x1000B010) - outAt) / 1024);
        Write32(0x1000B000, 0);
        if (ctrlAfter & 0x80000000u)
        {
            printf("IDEC check: slice %02x: IDEC still busy\n", code);
            return false;
        }
        if ((ctrlAfter & 0x4000) || !(ctrlAfter & 0x8000))
        {
            printf("IDEC check: slice %02x: ECD %u SCD %u after IDEC\n", code, (ctrlAfter >> 14) & 1, (ctrlAfter >> 15) & 1);
            ok = false;
        }
        int row = int(code) - 1;
        for (int k = 0; k < count; k++)
        {
            size_t mb = size_t(row * mbw + column + k);
            if (std::memcmp(Guest(outAt + u32(k) * 1024), firstRgb.data() + mb * 1024, 1024) != 0)
            {
                printf("IDEC check: slice %02x macroblock %d differs from BDEC + CSC\n", code, k);
                ok = false;
                break;
            }
            macroblocksChecked++;
        }
        i = end - 1;
    }
    return ok && macroblocksChecked > 0;
}

// PACK: RGB32 macroblocks to RGB16 (with and without the dither) and to INDX4 by a CLUT given with SETVQ, each against the
// definitions
bool CheckPack(const std::vector<u8>& rgb32Macroblocks)
{
    static const int dither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
    const u32 count = 4;
    bool ok = true;
    u16 clut[16];
    for (int k = 0; k < 16; k++)
    {
        clut[k] = static_cast<u16>((k * 2) | ((31 - k * 2) << 5) | ((k * 37 % 32) << 10));
    }
    for (int mode = 0; mode < 3; mode++)
    {
        bool rgb16 = mode < 2, withDither = mode == 1;
        std::memcpy(Guest(SpareBase), rgb32Macroblocks.data(), count * 1024);
        Write32(0x1000B400, 0);
        Write32(0x10002000, 0); // BCLR
        if (!rgb16)
        {
            u128 q[2];
            std::memcpy(q, clut, 32);
            ps2_io_write128(0x10007010, q[0]);
            ps2_io_write128(0x10007010, q[1]);
            Write32(0x10002000, 0x60000000); // SETVQ
        }
        u32 outQw = rgb16 ? 32 : 8;
        Write32(0x1000B410, SpareBase);
        Write32(0x1000B420, count * 64);
        Write32(0x1000B400, 0x101);
        Write32(0x1000B010, SpareBase + 0x10000);
        Write32(0x1000B020, count * outQw);
        Write32(0x1000B000, 0x100);
        Write32(0x10002000, 0x80000000u | (rgb16 ? 0x08000000u : 0) | (withDither ? 0x04000000u : 0) | count);
        if ((Read32(0x10002010) & 0x80000000u) || (Read32(0x1000B000) & 0x100))
        {
            printf("PACK mode %d didn't finish\n", mode);
            return false;
        }
        for (u32 mb = 0; mb < count && ok; mb++)
        {
            const u8* in = rgb32Macroblocks.data() + mb * 1024;
            const u8* out = Guest(SpareBase + 0x10000 + mb * outQw * 16);
            for (int p = 0; p < 256; p++)
            {
                const u8* px = in + p * 4;
                int d = withDither ? dither[(p / 16) & 3][(p % 16) & 3] : 0;
                auto five = [&](int c) { return u32(std::clamp(c + d, 0, 255)) >> 3; };
                u16 want = static_cast<u16>(five(px[0]) | (five(px[1]) << 5) | (five(px[2]) << 10) | (px[3] == 0x40 ? 0x8000 : 0));
                if (rgb16)
                {
                    u16 got;
                    std::memcpy(&got, out + p * 2, 2);
                    if (got != want)
                    {
                        printf("PACK RGB16%s: macroblock %u pixel %d: %04x, not %04x\n", withDither ? " dithered" : "", mb, p, got, want);
                        ok = false;
                        break;
                    }
                    continue;
                }
                int best = 0, bestDistance = 1 << 30;
                for (int k = 0; k < 16; k++)
                {
                    int dr = int(want & 31) - (clut[k] & 31), dg = int((want >> 5) & 31) - ((clut[k] >> 5) & 31),
                        db = int((want >> 10) & 31) - ((clut[k] >> 10) & 31);
                    if (dr * dr + dg * dg + db * db < bestDistance)
                    {
                        bestDistance = dr * dr + dg * dg + db * db;
                        best = k;
                    }
                }
                int got = (out[p / 2] >> ((p & 1) * 4)) & 15;
                if (got != best)
                {
                    printf("PACK INDX4: macroblock %u pixel %d: %d, not %d\n", mb, p, got, best);
                    ok = false;
                    break;
                }
            }
        }
    }
    return ok;
}
}

// The runtime's tables a game would give: none
extern "C" {
const Ps2FunctionEntry ps2_function_table[1] = {};
const u32 ps2_function_count = 0;
const Ps2OverlayFunction ps2_overlay_function_table[1] = {};
const u32 ps2_overlay_function_count = 0;
const Ps2Overlay ps2_overlays[1] = {};
const u32 ps2_overlay_count = 0;
}

// VU1's translated programs a game would give: none
namespace Ee::Vu1Jit
{
const Signature g_Signatures[1] = {};
const u32 g_SignatureCount = 0;
const Program g_Programs[1] = {};
const u32 g_ProgramCount = 0;
const Signature g_SignaturesVu0[1] = {};
const u32 g_SignatureCountVu0 = 0;
const Program g_ProgramsVu0[1] = {};
const u32 g_ProgramCountVu0 = 0;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s MOVIE.sfd OUTDIR [--frames N] [--png-every K] [--ring-blocks B] [--reference FILE.yuv]\n", argv[0]);
        return 2;
    }
    std::string movie = argv[1], outDir = argv[2], referencePath;
    long maxFrames = 1L << 30;
    long pngEvery = 30;
    u32 ringBlocks = RingBlocks;
    for (int i = 3; i + 1 < argc; i += 2)
    {
        std::string a = argv[i];
        if (a == "--frames")
        {
            maxFrames = atol(argv[i + 1]);
        }
        else if (a == "--png-every")
        {
            pngEvery = atol(argv[i + 1]);
        }
        else if (a == "--ring-blocks")
        {
            ringBlocks = u32(std::clamp(atol(argv[i + 1]), 2L, long(RingBlocks)));
        }
        else if (a == "--reference")
        {
            referencePath = argv[i + 1];
        }
    }
    bool ok = Ieee1180();

    std::vector<u8> file = ReadFile(movie);
    std::vector<u8> video;
    std::string error;
    if (!DemuxVideo(file, video, error))
    {
        fprintf(stderr, "%s: %s\n", movie.c_str(), error.c_str());
        return 1;
    }
    printf("%s: %zu bytes, video %zu bytes\n", movie.c_str(), file.size(), video.size());
    std::ifstream reference;
    if (!referencePath.empty())
    {
        reference.open(referencePath, std::ios::binary);
        if (!reference)
        {
            fprintf(stderr, "no reference %s\n", referencePath.c_str());
            return 1;
        }
    }

    Ps2::MemoryInit();
    Driver ee(video, ringBlocks);
    Player player(ee);
    long shown = 0;
    double decodeTotal = 0, decodeMax = 0, cscTotal = 0, cscMax = 0, wallTotal = 0;
    double lastIpu = 0;
    std::vector<double> decodeTimes, cscTimes;
    double psnrMin[3] = {99, 99, 99}, psnrSum[3] = {};
    int maxDiff[3] = {};
    std::vector<u8> firstRgb;
    int width = 0;
    u32 firstCtrl = 0;
    auto wall0 = std::chrono::steady_clock::now();
    auto lastWall = wall0;
    bool converted = true;
    bool ran = player.Run(
        [&](const Picture& p) {
            // The IPU's time since the last picture was its decoding (BDEC, VDEC, FDEC); then CSC
            double decode = IpuSeconds() - lastIpu;
            std::vector<u32> rgba;
            double before = IpuSeconds();
            if (!player.ConvertWithIpu(p, rgba, error))
            {
                converted = false;
                return false;
            }
            double csc = IpuSeconds() - before;
            lastIpu = IpuSeconds();
            auto now = std::chrono::steady_clock::now();
            wallTotal += std::chrono::duration<double>(now - lastWall).count();
            lastWall = now;
            decodeTimes.push_back(decode);
            cscTimes.push_back(csc);
            decodeTotal += decode;
            decodeMax = std::max(decodeMax, decode);
            cscTotal += csc;
            cscMax = std::max(cscMax, csc);
            if (p.decodeIndex == 0)
            {
                firstRgb.assign(Guest(RgbBase), Guest(RgbBase) + size_t(p.width / 16) * (p.height / 16) * 1024);
                width = p.width;
                firstCtrl = (player.CtrlSettings() & ~0x07000000u) | 0x01000000u;
            }
            if (reference.is_open())
            {
                std::vector<u8> ref(p.y.size() + p.cb.size() * 2);
                if (!reference.read(reinterpret_cast<char*>(ref.data()), std::streamsize(ref.size())))
                {
                    error = "the reference has fewer pictures";
                    converted = false;
                    return false;
                }
                const u8* planes[3] = {ref.data(), ref.data() + p.y.size(), ref.data() + p.y.size() + p.cb.size()};
                const std::vector<u8>* ours[3] = {&p.y, &p.cb, &p.cr};
                for (int c = 0; c < 3; c++)
                {
                    double v = Psnr(*ours[c], planes[c], &maxDiff[c]);
                    if (v < 40)
                    {
                        printf("picture %ld (decoded %llu, type %d): plane %d at %.2f dB\n", shown,
                               (unsigned long long)p.decodeIndex, p.type, c, v);
                    }
                    psnrMin[c] = std::min(psnrMin[c], v);
                    psnrSum[c] += v;
                }
            }
            if (shown % pngEvery == 0)
            {
                char name[64];
                snprintf(name, sizeof(name), "/frame%05ld.png", shown);
                WritePng(outDir + name, rgba.data(), u32(p.width), u32(p.height));
            }
            shown++;
            return shown < maxFrames;
        },
        error);
    if (!ran || !converted)
    {
        fprintf(stderr, "decoding stopped after %ld pictures: %s\n", shown, error.c_str());
        ok = false;
    }
    printf("%ld pictures shown, %llu macroblocks (%llu skipped), %llu BDECs, %llu prediction samples outside the picture\n",
           shown, (unsigned long long)player.macroblocks, (unsigned long long)player.skippedMacroblocks,
           (unsigned long long)player.bdecs, (unsigned long long)player.outsideReferences);
    printf("toIPU ring: %u refills while running, %u after it ended; %u restarts (sceIpuRestartDMA's), %u across blocks\n",
           ee.feedsWhileRunning, ee.feedsAfterEnd, ee.restarts, ee.restartsAcrossBlocks);
    if (shown > 0)
    {
        auto p99 = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1, v.size() * 99 / 100)];
        };
        printf("IPU time a picture: decoding %.3f ms (99%% under %.3f, most %.3f), CSC %.3f ms (99%% under %.3f, most %.3f); "
               "everything (the test's motion compensation too) %.3f ms\n",
               decodeTotal / shown * 1000, p99(decodeTimes) * 1000, decodeMax * 1000, cscTotal / shown * 1000,
               p99(cscTimes) * 1000, cscMax * 1000, wallTotal / shown * 1000);
    }
    if (reference.is_open() && shown > 0)
    {
        const char* names[3] = {"Y", "Cb", "Cr"};
        for (int c = 0; c < 3; c++)
        {
            printf("against the reference, %s: PSNR mean %.2f dB, lowest %.2f dB, largest difference %d\n", names[c],
                   psnrSum[c] / shown, psnrMin[c], maxDiff[c]);
        }
        ok &= psnrMin[0] > 40 && psnrMin[1] > 40 && psnrMin[2] > 40;
        char extra;
        if (shown < maxFrames && reference.read(&extra, 1))
        {
            printf("the reference has more pictures than were decoded\n");
            ok = false;
        }
    }
    if (!firstRgb.empty())
    {
        int checked = 0;
        bool idec = CheckIdec(video, firstRgb, width, firstCtrl, checked);
        printf("IDEC against BDEC + CSC on the first I picture: %d macroblocks, %s\n", checked, idec ? "the same" : "FAIL");
        bool pack = CheckPack(firstRgb);
        printf("PACK (RGB16, RGB16 dithered, INDX4 by SETVQ's CLUT): %s\n", pack ? "as defined" : "FAIL");
        ok &= idec && pack;
    }
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
