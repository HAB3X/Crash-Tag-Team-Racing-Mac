// An MPEG-2 video player on the EE's side, as Sony's libmpeg is one: the headers read through the IPU (FDEC, IPU_TOP), each
// macroblock's address increment, type and motion codes by VDEC, its blocks by BDEC into the scratchpad through fromIPU, the
// motion compensation done here (the EE's work on the console), and each picture converted to RGB32 by the IPU's CSC from
// its macroblocks (RAW8) through toIPU and fromIPU. Frame pictures with frame prediction only (all the .sfd files seen):
// anything else is reported as not done
#pragma once

#include "ee.h"

#include <functional>
#include <string>

namespace IpuTest
{
struct Picture
{
    int width = 0, height = 0;
    std::vector<u8> y, cb, cr;
    int type = 0;          // 1 I, 2 P, 3 B
    u64 decodeIndex = 0;
};

class Player
{
public:
    explicit Player(Driver& ee) : ee_(ee) {}
    // Pictures decoded and handed over in display order until the stream ends, `onPicture` returns false, or an error (in
    // `error`). Returns whether it ended without error
    bool Run(const std::function<bool(const Picture&)>& onPicture, std::string& error);
    // A picture's colour conversion by the IPU (CSC to RGB32, macroblock by macroblock), as RGBA pixels row by row
    bool ConvertWithIpu(const Picture& p, std::vector<u32>& rgba, std::string& error);

    // IPU_CTRL's settings for the current picture
    u32 CtrlSettings() const;

    u64 macroblocks = 0;
    u64 skippedMacroblocks = 0;
    u64 bdecs = 0;
    u64 outsideReferences = 0;

private:
    struct Motion
    {
        bool forward = false, backward = false;
        int mv[2][2] = {};
    };
    bool SequenceHeader(std::string& error);
    bool Extension(std::string& error);
    bool PictureHeader(std::string& error);
    bool Slice(u32 code, std::string& error);
    bool LoadMatrix(bool nonIntra, bool fromStream, std::string& error);
    bool MotionVector(int s, std::string& error);
    void Predict(int mbx, int mby, const Motion& m, u8* y, u8* cb, u8* cr);
    void StoreMacroblock(int mbx, int mby, const u8* predY, const u8* predCb, const u8* predCr, const s16* residual,
                         bool intra);
    bool FinishPicture(const std::function<bool(const Picture&)>& onPicture);

    Driver& ee_;
    int width_ = 0, height_ = 0, mbw_ = 0, mbh_ = 0;
    bool mpeg1_ = true;
    // The picture's settings
    int type_ = 0;
    int fcode_[2][2] = {};
    int dcPrecision_ = 0, structure_ = 3;
    bool framePredDct_ = true, concealment_ = false, nonLinear_ = false, intraVlc_ = false, alternate_ = false;
    int pmv_[2][2] = {};
    Motion last_;
    // The pictures: being decoded, and the anchors (older and newer) for prediction
    Picture current_;
    Picture older_, newer_;
    bool haveOlder_ = false, haveNewer_ = false, newerShown_ = true;
    bool inPicture_ = false;
    u64 decoded_ = 0;
};
}
