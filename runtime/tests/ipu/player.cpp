#include "player.h"

#include <algorithm>
#include <cstring>

namespace IpuTest
{
namespace
{
constexpr u32 Busy = 0x80000000u;
constexpr u32 IpuCmd = 0x10002000;
constexpr u32 IpuCtrl = 0x10002010;
constexpr u32 IpuIn = 0x10007010;
constexpr u32 D3Chcr = 0x1000B000, D3Madr = 0x1000B010, D3Qwc = 0x1000B020;
constexpr u32 D4Chcr = 0x1000B400, D4Madr = 0x1000B410, D4Qwc = 0x1000B420;

// The standard's default intra matrix in zigzag order (as Sony's libipu loads it at 0x1F02B0 in File 2's executable)
constexpr u8 kDefaultIntra[64] = {8,  16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26,
                                  27, 27, 27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38,
                                  37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83};

// macroblock_type's flags (VDEC's)
constexpr u32 MbQuant = 0x10, MbForward = 0x08, MbBackward = 0x04, MbPattern = 0x02, MbIntra = 0x01;

// Polls until the IPU is idle without feeding toIPU (the colour conversion has toIPU for its own data)
bool WaitIdleNoFeed()
{
    for (int i = 0; i < 1000; i++)
    {
        if ((Read32(IpuCtrl) & Busy) == 0)
        {
            return true;
        }
    }
    return false;
}

u8 Clamp(int v) { return static_cast<u8>(std::clamp(v, 0, 255)); }

// A reference's sample at a half sample position (the integer part by >> 1, floor), clamped into the picture
inline int Sample(const u8* plane, int w, int h, int x, int y, int hx, int hy, u64& outside)
{
    auto at = [&](int sx, int sy) {
        if (sx < 0 || sy < 0 || sx >= w || sy >= h)
        {
            outside++;
            sx = std::clamp(sx, 0, w - 1);
            sy = std::clamp(sy, 0, h - 1);
        }
        return int(plane[sy * w + sx]);
    };
    if (!hx && !hy)
    {
        return at(x, y);
    }
    if (hx && !hy)
    {
        return (at(x, y) + at(x + 1, y) + 1) >> 1;
    }
    if (!hx && hy)
    {
        return (at(x, y) + at(x, y + 1) + 1) >> 1;
    }
    return (at(x, y) + at(x + 1, y) + at(x, y + 1) + at(x + 1, y + 1) + 2) >> 2;
}

void PredictFrom(const Picture& ref, int mbx, int mby, const int mv[2], u8* y, u8* cb, u8* cr, u64& outside)
{
    for (int j = 0; j < 16; j++)
    {
        for (int i = 0; i < 16; i++)
        {
            y[j * 16 + i] = static_cast<u8>(Sample(ref.y.data(), ref.width, ref.height, mbx * 16 + i + (mv[0] >> 1),
                                                   mby * 16 + j + (mv[1] >> 1), mv[0] & 1, mv[1] & 1, outside));
        }
    }
    // 4:2:0 chroma's vector: the luma's halved toward zero (7.6.3.7)
    int cx = mv[0] / 2, cy = mv[1] / 2;
    for (int j = 0; j < 8; j++)
    {
        for (int i = 0; i < 8; i++)
        {
            int sx = mbx * 8 + i + (cx >> 1), sy = mby * 8 + j + (cy >> 1);
            cb[j * 8 + i] = static_cast<u8>(Sample(ref.cb.data(), ref.width / 2, ref.height / 2, sx, sy, cx & 1, cy & 1, outside));
            cr[j * 8 + i] = static_cast<u8>(Sample(ref.cr.data(), ref.width / 2, ref.height / 2, sx, sy, cx & 1, cy & 1, outside));
        }
    }
}
}

u32 Player::CtrlSettings() const
{
    return (u32(dcPrecision_) << 16) | (alternate_ ? 1u << 20 : 0) | (intraVlc_ ? 1u << 21 : 0) | (nonLinear_ ? 1u << 22 : 0) |
           (mpeg1_ ? 1u << 23 : 0) | (u32(type_) << 24);
}

bool Player::LoadMatrix(bool nonIntra, bool fromStream, std::string& error)
{
    u32 setiq = 0x50000000u | (nonIntra ? 0x08000000u : 0);
    if (fromStream)
    {
        // The load flag skipped by SETIQ's FB, the IPU reading the 64 bytes from the bitstream
        if (!ee_.Command(setiq | 1))
        {
            error = "SETIQ from the stream didn't finish";
            return false;
        }
        return true;
    }
    if (!ee_.Skip(1))
    {
        error = "the stream ended in a matrix flag";
        return false;
    }
    // The default matrix through IPU_IN_FIFO, toIPU stopped meanwhile and put back after (libipu's way)
    ee_.StopDma();
    ee_.Command(0); // BCLR
    u8 matrix[64];
    for (int i = 0; i < 64; i++)
    {
        matrix[i] = nonIntra ? 16 : kDefaultIntra[i];
    }
    for (int q = 0; q < 4; q++)
    {
        u128 v;
        std::memcpy(&v, matrix + q * 16, 16);
        ps2_io_write128(IpuIn, v);
    }
    Write32(IpuCmd, setiq);
    if (!WaitIdleNoFeed())
    {
        error = "SETIQ from IPU_IN_FIFO didn't finish";
        return false;
    }
    ee_.RestartDma();
    return true;
}

bool Player::SequenceHeader(std::string& error)
{
    u32 w, h, v;
    if (!ee_.Get(12, w) || !ee_.Get(12, h) || !ee_.Get(4, v) || !ee_.Get(4, v) || !ee_.Get(18, v) || !ee_.Get(1, v) ||
        !ee_.Get(10, v) || !ee_.Get(1, v))
    {
        error = "the sequence header was cut short";
        return false;
    }
    if (width_ != 0 && (int(w) != width_ || int(h) != height_))
    {
        error = "the picture's size changed";
        return false;
    }
    width_ = int(w);
    height_ = int(h);
    mbw_ = (width_ + 15) / 16;
    mbh_ = (height_ + 15) / 16;
    if (width_ % 16 != 0 || height_ % 16 != 0)
    {
        error = "a picture size that isn't whole macroblocks (" + std::to_string(w) + "x" + std::to_string(h) + ")";
        return false;
    }
    mpeg1_ = true; // until a sequence extension says otherwise
    u32 load;
    if (!ee_.Peek(1, load) || !LoadMatrix(false, load != 0, error))
    {
        return false;
    }
    return ee_.Peek(1, load) && LoadMatrix(true, load != 0, error);
}

bool Player::Extension(std::string& error)
{
    u32 id, v;
    if (!ee_.Get(4, id))
    {
        error = "an extension was cut short";
        return false;
    }
    switch (id)
    {
    case 1: // sequence extension
    {
        u32 chroma;
        if (!ee_.Get(8, v) || !ee_.Get(1, v) || !ee_.Get(2, chroma))
        {
            return false;
        }
        if (chroma != 1)
        {
            error = "a chroma format other than 4:2:0";
            return false;
        }
        mpeg1_ = false;
        return true;
    }
    case 3: // quant matrix extension
    {
        u32 load;
        if (!ee_.Peek(1, load))
        {
            return false;
        }
        if (load ? !LoadMatrix(false, true, error) : !ee_.Skip(1))
        {
            return false;
        }
        if (!ee_.Peek(1, load))
        {
            return false;
        }
        return load ? LoadMatrix(true, true, error) : ee_.Skip(1);
    }
    case 8: // picture coding extension
    {
        u32 f[4], idp, ps, tff, fpfd, cmv, qst, ivf, as;
        for (u32& c : f)
        {
            if (!ee_.Get(4, c))
            {
                return false;
            }
        }
        if (!ee_.Get(2, idp) || !ee_.Get(2, ps) || !ee_.Get(1, tff) || !ee_.Get(1, fpfd) || !ee_.Get(1, cmv) ||
            !ee_.Get(1, qst) || !ee_.Get(1, ivf) || !ee_.Get(1, as))
        {
            return false;
        }
        fcode_[0][0] = int(f[0]);
        fcode_[0][1] = int(f[1]);
        fcode_[1][0] = int(f[2]);
        fcode_[1][1] = int(f[3]);
        dcPrecision_ = int(idp);
        structure_ = int(ps);
        framePredDct_ = fpfd != 0;
        concealment_ = cmv != 0;
        nonLinear_ = qst != 0;
        intraVlc_ = ivf != 0;
        alternate_ = as != 0;
        if (structure_ != 3 || !framePredDct_ || concealment_)
        {
            error = "field pictures, field prediction or concealment vectors (not done by this test)";
            return false;
        }
        return true;
    }
    default:
        return true; // the others (display extensions) aren't needed; the next start code skips them
    }
}

bool Player::PictureHeader(std::string& error)
{
    u32 v, type;
    if (!ee_.Get(10, v) || !ee_.Get(3, type) || !ee_.Get(16, v))
    {
        error = "a picture header was cut short";
        return false;
    }
    if (type < 1 || type > 3)
    {
        error = "picture coding type " + std::to_string(type);
        return false;
    }
    type_ = int(type);
    if (type >= 2)
    {
        u32 fullPel, f;
        if (!ee_.Get(1, fullPel) || !ee_.Get(3, f))
        {
            return false;
        }
        if (fullPel)
        {
            error = "full pel vectors (MPEG-1's; not done by this test)";
            return false;
        }
        fcode_[0][0] = fcode_[0][1] = int(f);
    }
    if (type == 3)
    {
        u32 fullPel, f;
        if (!ee_.Get(1, fullPel) || !ee_.Get(3, f))
        {
            return false;
        }
        fcode_[1][0] = fcode_[1][1] = int(f);
    }
    if (mpeg1_)
    {
        dcPrecision_ = 0;
        nonLinear_ = intraVlc_ = alternate_ = false;
    }
    current_ = Picture{};
    current_.width = width_;
    current_.height = height_;
    current_.type = type_;
    current_.y.assign(size_t(width_) * height_, 0);
    current_.cb.assign(size_t(width_) * height_ / 4, 0);
    current_.cr.assign(size_t(width_) * height_ / 4, 0);
    current_.decodeIndex = decoded_;
    inPicture_ = true;
    // Room made in the ring while the transfer runs (CRI's code adds data as it demuxes it)
    ee_.Feed();
    return true;
}

bool Player::MotionVector(int s, std::string& error)
{
    for (int t = 0; t < 2; t++)
    {
        u32 word;
        if (!ee_.Vdec(2, word) || word == 0)
        {
            error = "a motion_code no table has";
            return false;
        }
        int code = static_cast<s16>(word & 0xFFFF);
        int rsize = fcode_[s][t] - 1;
        int f = 1 << rsize;
        int delta = code;
        if (f != 1 && code != 0)
        {
            u32 residual;
            if (!ee_.Get(u32(rsize), residual))
            {
                return false;
            }
            delta = (std::abs(code) - 1) * f + int(residual) + 1;
            if (code < 0)
            {
                delta = -delta;
            }
        }
        int low = -16 * f, high = 16 * f - 1, range = 32 * f;
        int v = pmv_[s][t] + delta;
        if (v < low)
        {
            v += range;
        }
        else if (v > high)
        {
            v -= range;
        }
        pmv_[s][t] = v;
        last_.mv[s][t] = v;
    }
    return true;
}

void Player::Predict(int mbx, int mby, const Motion& m, u8* y, u8* cb, u8* cr)
{
    const Picture& forward = type_ == 2 ? newer_ : older_;
    if (m.forward && m.backward)
    {
        u8 by[256], bcb[64], bcr[64];
        PredictFrom(forward, mbx, mby, m.mv[0], y, cb, cr, outsideReferences);
        PredictFrom(newer_, mbx, mby, m.mv[1], by, bcb, bcr, outsideReferences);
        for (int i = 0; i < 256; i++)
        {
            y[i] = static_cast<u8>((y[i] + by[i] + 1) >> 1);
        }
        for (int i = 0; i < 64; i++)
        {
            cb[i] = static_cast<u8>((cb[i] + bcb[i] + 1) >> 1);
            cr[i] = static_cast<u8>((cr[i] + bcr[i] + 1) >> 1);
        }
        return;
    }
    if (m.backward)
    {
        PredictFrom(newer_, mbx, mby, m.mv[1], y, cb, cr, outsideReferences);
        return;
    }
    // Forward, or a P picture's macroblock with no vector (zero)
    int zero[2] = {0, 0};
    PredictFrom(forward, mbx, mby, m.forward ? m.mv[0] : zero, y, cb, cr, outsideReferences);
}

void Player::StoreMacroblock(int mbx, int mby, const u8* predY, const u8* predCb, const u8* predCr, const s16* residual,
                             bool intra)
{
    int w = width_, cw = width_ / 2;
    for (int j = 0; j < 16; j++)
    {
        for (int i = 0; i < 16; i++)
        {
            int r = residual ? residual[j * 16 + i] : 0;
            current_.y[size_t(mby * 16 + j) * w + mbx * 16 + i] = intra ? Clamp(r) : Clamp(predY[j * 16 + i] + r);
        }
    }
    for (int j = 0; j < 8; j++)
    {
        for (int i = 0; i < 8; i++)
        {
            int rb = residual ? residual[256 + j * 8 + i] : 0;
            int rr = residual ? residual[320 + j * 8 + i] : 0;
            size_t at = size_t(mby * 8 + j) * cw + mbx * 8 + i;
            current_.cb[at] = intra ? Clamp(rb) : Clamp(predCb[j * 8 + i] + rb);
            current_.cr[at] = intra ? Clamp(rr) : Clamp(predCr[j * 8 + i] + rr);
        }
    }
}

bool Player::Slice(u32 code, std::string& error)
{
    int row = int(code) - 1;
    if (row >= mbh_)
    {
        error = "a slice below the picture";
        return false;
    }
    u32 qsc, flag, v;
    if (!ee_.Get(5, qsc) || !ee_.Peek(1, flag))
    {
        return false;
    }
    if (flag)
    {
        // intra_slice_flag, intra_slice, reserved 7, then extra information bytes
        if (!ee_.Skip(9))
        {
            return false;
        }
        for (;;)
        {
            if (!ee_.Get(1, flag))
            {
                return false;
            }
            if (!flag)
            {
                break;
            }
            ee_.Skip(8);
        }
    }
    else if (!ee_.Get(1, v))
    {
        return false;
    }
    std::memset(pmv_, 0, sizeof(pmv_));
    last_ = Motion{};
    int address = row * mbw_ - 1;
    bool first = true;
    bool dcReset = true;
    for (;;)
    {
        int increment = 0;
        for (;;)
        {
            u32 word;
            if (!ee_.Vdec(0, word) || word == 0)
            {
                error = "a macroblock_address_increment no table has (slice " + std::to_string(code) + ")";
                return false;
            }
            int value = static_cast<s16>(word & 0xFFFF);
            if (value == 0x23)
            {
                increment += 33;
                continue;
            }
            if (value == 0x22)
            {
                continue;
            }
            increment += value;
            break;
        }
        if (!first && increment > 1)
        {
            // Skipped macroblocks: P's predicted with a zero vector (and the vectors reset), B's as the last one was
            if (type_ == 1)
            {
                error = "skipped macroblocks in an I picture";
                return false;
            }
            Motion skip = type_ == 2 ? Motion{} : last_;
            if (type_ == 2)
            {
                std::memset(pmv_, 0, sizeof(pmv_));
            }
            for (int k = 1; k < increment; k++)
            {
                int a = address + k;
                u8 py[256], pcb[64], pcr[64];
                Predict(a % mbw_, a / mbw_, skip, py, pcb, pcr);
                StoreMacroblock(a % mbw_, a / mbw_, py, pcb, pcr, nullptr, false);
                skippedMacroblocks++;
            }
            dcReset = true;
        }
        address += increment;
        first = false;
        if (address >= mbw_ * mbh_)
        {
            error = "a macroblock past the picture's end";
            return false;
        }
        int mbx = address % mbw_, mby = address / mbw_;
        u32 word;
        if (!ee_.Vdec(1, word) || word == 0)
        {
            error = "a macroblock_type no table has";
            return false;
        }
        u32 type = word & 0x1F;
        if (type & MbQuant)
        {
            if (!ee_.Get(5, qsc))
            {
                return false;
            }
        }
        bool intra = (type & MbIntra) != 0;
        Motion m;
        m.forward = (type & MbForward) != 0;
        m.backward = (type & MbBackward) != 0;
        if (m.forward && !MotionVector(0, error))
        {
            return false;
        }
        if (m.backward && !MotionVector(1, error))
        {
            return false;
        }
        std::memcpy(m.mv, last_.mv, sizeof(m.mv));
        if (intra || (type_ == 2 && !m.forward))
        {
            // Intra macroblocks (no concealment vectors) and P's without a vector reset the predictors
            std::memset(pmv_, 0, sizeof(pmv_));
            std::memset(last_.mv, 0, sizeof(last_.mv));
        }
        last_.forward = m.forward;
        last_.backward = m.backward;
        const s16* residual = nullptr;
        bool sliceEnd;
        if (intra || (type & MbPattern))
        {
            // fromIPU into the scratchpad, then BDEC (libmpeg's _decMB0)
            Write32(D3Madr, SprMacroblock);
            Write32(D3Qwc, 48);
            Write32(D3Chcr, 0x100);
            u32 bdec = 0x20000000u | (intra ? 0x08000000u : 0) | (intra && dcReset ? 0x04000000u : 0) | (qsc << 16);
            if (!ee_.Command(bdec))
            {
                error = "BDEC didn't finish";
                return false;
            }
            bdecs++;
            u32 ctrl = ee_.Ctrl();
            if (ctrl & 0x4000)
            {
                error = "BDEC set ECD (a bad code in the blocks) at macroblock " + std::to_string(address);
                return false;
            }
            if ((Read32(D3Chcr) & 0x100) != 0 || Read32(D3Qwc) != 0)
            {
                error = "fromIPU didn't get BDEC's 48 quadwords";
                return false;
            }
            residual = reinterpret_cast<const s16*>(ps2_mem + 0x70000000);
            sliceEnd = (ctrl & 0x8000) != 0;
        }
        else
        {
            u32 next;
            if (!ee_.Peek(23, next))
            {
                return false;
            }
            sliceEnd = next == 0;
        }
        dcReset = !intra;
        u8 py[256], pcb[64], pcr[64];
        if (!intra)
        {
            Predict(mbx, mby, m, py, pcb, pcr);
        }
        StoreMacroblock(mbx, mby, py, pcb, pcr, residual, intra);
        macroblocks++;
        if (sliceEnd)
        {
            return true;
        }
    }
}

bool Player::FinishPicture(const std::function<bool(const Picture&)>& onPicture)
{
    inPicture_ = false;
    decoded_++;
    if (current_.type == 3)
    {
        return onPicture(current_);
    }
    bool go = true;
    if (haveNewer_ && !newerShown_)
    {
        go = onPicture(newer_);
    }
    older_ = std::move(newer_);
    haveOlder_ = haveNewer_;
    newer_ = std::move(current_);
    haveNewer_ = true;
    newerShown_ = false;
    return go;
}

bool Player::Run(const std::function<bool(const Picture&)>& onPicture, std::string& error)
{
    if (!ee_.Feed())
    {
        error = "no data";
        return false;
    }
    u32 code;
    while (ee_.NextStartCode(code))
    {
        bool slice = code >= 0x01 && code <= 0xAF;
        if (inPicture_ && !slice && code != 0xB5 && code != 0xB2)
        {
            // Not a slice nor the picture's extension or user data: the picture is whole
            if (!FinishPicture(onPicture))
            {
                return true;
            }
        }
        bool ok = true;
        switch (code)
        {
        case 0xB3: ok = SequenceHeader(error); break;
        case 0xB5: ok = Extension(error); break;
        case 0x00:
            if (width_ == 0)
            {
                error = "a picture before the sequence header";
                return false;
            }
            ok = PictureHeader(error);
            break;
        case 0xB7:
            if (haveNewer_ && !newerShown_)
            {
                newerShown_ = true;
                onPicture(newer_);
            }
            return true;
        default:
            if (slice)
            {
                if (!inPicture_)
                {
                    error = "a slice outside a picture";
                    return false;
                }
                if (type_ >= 2 && !haveNewer_)
                {
                    error = "a predicted picture with no reference before it";
                    return false;
                }
                // The picture's settings (its coding extension's, read by now) in IPU_CTRL: IDP, AS, IVF, QST, MP1 and
                // the picture type (VDEC's macroblock_type)
                Write32(IpuCtrl, CtrlSettings());
                ok = Slice(code, error);
            }
            break;
        }
        if (!ok)
        {
            if (error.empty())
            {
                error = "the stream ended inside a header";
            }
            return false;
        }
    }
    if (inPicture_)
    {
        FinishPicture(onPicture);
    }
    if (haveNewer_ && !newerShown_)
    {
        onPicture(newer_);
    }
    return ee_.StreamDone() ? true : (error = "the IPU stopped before the stream's end", false);
}

bool Player::ConvertWithIpu(const Picture& p, std::vector<u32>& rgba, std::string& error)
{
    int mbw = p.width / 16, mbh = p.height / 16;
    u32 count = u32(mbw * mbh);
    if (count > 0x3FF)
    {
        error = "more than 1023 macroblocks for one CSC";
        return false;
    }
    // The picture's macroblocks as RAW8 (Y 16 x 16, Cb 8 x 8, Cr 8 x 8 each), libmpeg's reference image layout
    for (int mby = 0; mby < mbh; mby++)
    {
        for (int mbx = 0; mbx < mbw; mbx++)
        {
            u8* mb = Guest(Raw8Base + u32(mby * mbw + mbx) * 384);
            for (int j = 0; j < 16; j++)
            {
                std::memcpy(mb + j * 16, &p.y[size_t(mby * 16 + j) * p.width + mbx * 16], 16);
            }
            for (int j = 0; j < 8; j++)
            {
                std::memcpy(mb + 256 + j * 8, &p.cb[size_t(mby * 8 + j) * (p.width / 2) + mbx * 8], 8);
                std::memcpy(mb + 320 + j * 8, &p.cr[size_t(mby * 8 + j) * (p.width / 2) + mbx * 8], 8);
            }
        }
    }
    // libmpeg's _sceMpegCscStoreRefImage: the bitstream's DMA stopped, BCLR, toIPU with the macroblocks, fromIPU for the
    // RGB32, CSC; then the bitstream put back
    ee_.StopDma();
    ee_.Command(0);
    Write32(D4Madr, Raw8Base);
    Write32(D4Qwc, count * 24);
    Write32(D4Chcr, 0x101);
    Write32(D3Madr, RgbBase);
    Write32(D3Qwc, count * 64);
    Write32(D3Chcr, 0x100);
    Write32(IpuCmd, 0x70000000u | count);
    if (!WaitIdleNoFeed())
    {
        error = "CSC didn't finish";
        return false;
    }
    if ((Read32(D3Chcr) & 0x100) != 0 || (Read32(D4Chcr) & 0x100) != 0)
    {
        error = "CSC's DMA didn't end (fromIPU QWC " + std::to_string(Read32(D3Qwc)) + ", toIPU QWC " +
                std::to_string(Read32(D4Qwc)) + ")";
        return false;
    }
    rgba.assign(size_t(p.width) * p.height, 0);
    for (int mby = 0; mby < mbh; mby++)
    {
        for (int mbx = 0; mbx < mbw; mbx++)
        {
            const u8* mb = Guest(RgbBase + u32(mby * mbw + mbx) * 1024);
            for (int j = 0; j < 16; j++)
            {
                for (int i = 0; i < 16; i++)
                {
                    const u8* px = mb + (j * 16 + i) * 4;
                    rgba[size_t(mby * 16 + j) * p.width + mbx * 16 + i] =
                        px[0] | (u32(px[1]) << 8) | (u32(px[2]) << 16) | 0xFF000000u;
                }
            }
        }
    }
    ee_.RestartDma();
    return true;
}
}
