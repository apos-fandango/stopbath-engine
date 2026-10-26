/*
 *  StopBath engine helper - step 2.
 *
 *  A small program built from ART's own processing engine (ART by Alberto Griggio, a fork of RawTherapee), for
 *  StopBath. StopBath starts it once and sends it the same requests it would give ART-cli; it answers them without
 *  starting again, keeps the last photos loaded and reuses ART's raw steps (preprocessing, demosaicing) when only
 *  later settings changed. 0.3: also the photo as it is before ART's colour and tone steps (read out of the raw steps,
 *  in the working colours, denoised, its spots, dehaze and lens corrections done) is kept for the last photo, so a
 *  change of exposure, colour or tone - or the second of a film's two pictures - starts from there; and "-R" writes a
 *  picture as three planes of 32-bit numbers (StopBath's film reads it at once). 0.4: each answer also says how long
 *  ART's steps took (ART's own per-step timing). 0.5: "-Q" writes a quick 8-bit TIFF (ART-cli's pixels, its colour
 *  profile, no metadata: ART's saving rewrites the whole file to add that). 0.6: the picture after ART's stage 1 (exposure,
 *  channel mixer, HSL, tone equalizer) and after stage 2 (sharpening, colour correction, smoothing...) is kept too, as
 *  ART's editor does: a change in a later stage starts there; "-N" (a picture made ahead) keeps nothing, so it doesn't
 *  push out the photo being edited. 0.7: each answer also says the picture's geometry - the full-size picture's size for
 *  these settings (the crop's, else the photo's, as turned), where the crop sits, the scale of the quick route and the
 *  size of the picture made ("geo:" in <what was reused>), so StopBath needn't work them out itself:
 *
 *    stopbath-engine --version
 *    stopbath-engine --serve [--keep <n>] [--memory small|normal|large]   (0.7: how much of ART's steps is kept)
 *        requests on standard input, one at a time:
 *          run <id> <count>        then <count> lines: ART-cli's arguments (-o/-O, -p, -d, -Y, -f, -j, -js, -t, -b,
 *                                  -n, -c <one photo>), one per line; StopBath's own: -R, -Q, -N
 *          cancel <id>             stop that request at the next step
 *          quit
 *        answers on standard output (other lines there are ART's own and can be ignored):
 *          @@SB ready <helper version> <ART version>
 *          @@SB done <id> <exit code> <ms> <what was reused> <message>
 *        (0.4: <what was reused> ends with ",steps:" and ART's steps that took a millisecond or more, slowest first:
 *        <name>=<ms>, joined by ";" - ART's own per-step timing, ART_PROFILE, switched on here)
 *        exit code 0: done as ART-cli would; 99: not something the helper does (use ART-cli); 98: stopped
 *    stopbath-engine --render <photo> <out.jpg> [--profile <file.arp>]... [--fast] [--repeat <n>]
 *        times one photo, developed n times in one go (step 1's test)
 *
 *  Copyright (c) 2026 the StopBath engine helper's authors. Based on ART's main-cli.cc and simpleprocess.cc
 *  (Copyright (c) 2004-2010 Gabor Horvath, Alberto Griggio and the RawTherapee and ART authors).
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version (see LICENSE).
 */

#include "../engine/colortemp.h"
#include "../engine/curves.h"
#include "../engine/dcp.h"
#include "../engine/iccstore.h"
#include "../engine/imagesource.h"
#include "../engine/imgiomanager.h"
#include "../engine/improcfun.h"
#include "../engine/metadata.h"
#include "../engine/processingjob.h"
#include "../engine/pipelineprofile.h"
#include "../engine/procparams.h"
#include "../engine/profilestore.h"
#include "../engine/rawimagesource.h"
#include "../engine/rtengine.h"
#include "../engine/settings.h"
#include "../utils/config.h"
#include "../utils/pathutils.h"
#include "../utils/version.h"
#include "fastexport.h"
#include "options.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <giomm.h>
#include <iostream>
#include <sstream>
#include <locale.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <tiffio.h>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#undef THREAD_PRIORITY_NORMAL

namespace art { namespace gui {
using namespace utils;
extern Options options;
// (ART's other command-line sources expect these)
Glib::ustring creditsPath;
Glib::ustring licensePath;
Glib::ustring argv1;
bool progress = false;
} } // namespace art::gui

using Clock = std::chrono::steady_clock;

namespace {

const char *HelperVersion = "0.7";

double ms(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

} // namespace

// ---- ART's export pipeline (simpleprocess.cc), with the raw steps reused and stopping between steps ----

namespace art { namespace engine {
extern const Settings *settings;

namespace sb {

/** What ART's raw steps (preprocessing, demosaicing, the highlight reconstruction done on their result) depend on:
 *  the same again, their result in the image source is reused (as ART's editor does). */
struct RawKey {
    bool valid = false;
    procparams::RAWParams raw;
    procparams::LensProfParams lens;
    procparams::CoarseTransformParams coarse;
    bool denoise = false;
    double wbTemp = 0, wbGreen = 0, wbEqual = 0, mul[3] = {0, 0, 0};
    bool hrOn = false;
    int hrMode = 0, hrBlur = 0;
    int frame = 0, border = 0;
    bool wbOn = false;
    int wbMethod = 0;
    double wbEqualSet = 1;

    bool same(const RawKey &o) const
    {
        // (0.3: ART's own comparison of the lens settings leaves out its vignetting and distortion switches)
        return valid && o.valid && raw == o.raw && lens == o.lens && lens.useVign == o.lens.useVign && lens.useDist == o.lens.useDist &&
               coarse == o.coarse && denoise == o.denoise &&
               wbTemp == o.wbTemp && wbGreen == o.wbGreen && wbEqual == o.wbEqual && mul[0] == o.mul[0] &&
               mul[1] == o.mul[1] && mul[2] == o.mul[2] && hrOn == o.hrOn && hrMode == o.hrMode &&
               hrBlur == o.hrBlur && frame == o.frame && border == o.border && wbOn == o.wbOn &&
               wbMethod == o.wbMethod && wbEqualSet == o.wbEqualSet;
    }
};

/** What the noise measured for automatic chroma noise reduction depends on (ART-cli measures it every time): the
 *  raw steps' result, the white balance, the turn, the colour settings, highlight reconstruction, the noise settings. */
struct DnKey {
    bool valid = false;
    RawKey raw;
    double wb[6] = {0, 0, 0, 0, 0, 0};
    procparams::CoarseTransformParams coarse;
    procparams::ColorManagementParams icm;
    bool hrOn = false;
    int hrMode = 0, hrBlur = 0;
    procparams::DenoiseParams denoise;

    bool same(const DnKey &o) const
    {
        return valid && o.valid && raw.same(o.raw) && std::equal(wb, wb + 6, o.wb) && coarse == o.coarse &&
               icm == o.icm && hrOn == o.hrOn && hrMode == o.hrMode && hrBlur == o.hrBlur && denoise == o.denoise;
    }
};

/** ART keeps the automatic white balance it worked out in the image source: worked out again after the raw steps
 *  are done again (as ART-cli, which starts afresh each time). */
struct AwbReset: ImageSource {
    static void reset(ImageSource *src)
    {
        src->*(&AwbReset::redAWBMul) = -1.;
        src->*(&AwbReset::greenAWBMul) = -1.;
        src->*(&AwbReset::blueAWBMul) = -1.;
    }
};

/** What the screen-size picture ART starts from (the raw steps' result read out, its spots removed, made small on
 *  the quick route) depends on: the same again, a copy of it is reused. */
struct EarlyKey {
    bool valid = false;
    RawKey raw;
    double wb[6] = {0, 0, 0, 0, 0, 0};
    int tr = 0, fw = 0, fh = 0, imw = 0, imh = 0;
    double scale = 1;
    bool allow = false;
    procparams::SpotParams spot;
    int outW = 0, outH = 0;

    bool same(const EarlyKey &o) const
    {
        return valid && o.valid && raw.same(o.raw) && std::equal(wb, wb + 6, o.wb) && tr == o.tr && fw == o.fw &&
               fh == o.fh && imw == o.imw && imh == o.imh && scale == o.scale && allow == o.allow && spot == o.spot;
    }
};

/** 0.3: what the photo before ART's colour and tone steps (the end of the transform: before the crop and stage 1)
 *  depends on - every setting but those only the later steps read (and the date-and-rank kind), as ART's editor
 *  starts an exposure change from its kept picture. The same again: a copy of that picture is the start. */
struct MidKey {
    bool valid = false;
    RawKey raw;
    double wb[6] = {0, 0, 0, 0, 0, 0};
    int tr = 0;
    bool fast = false;
    double scale = 1;
    procparams::ProcParams p;

    static procparams::ProcParams early(const procparams::ProcParams &all, bool fast)
    {
        procparams::ProcParams e = all;
        const procparams::ProcParams d;
        // (read only in stage 1, 2 and 3, after the resize, or at the output)
        e.saturation = d.saturation;
        e.toneCurve = d.toneCurve;
        e.labCurve = d.labCurve;
        e.localContrast = d.localContrast;
        e.rgbCurves = d.rgbCurves;
        e.sharpening = d.sharpening;
        e.prsharpening = d.prsharpening;
        e.impulseDenoise = d.impulseDenoise;
        e.defringe = d.defringe;
        e.textureBoost = d.textureBoost;
        e.logenc = d.logenc;
        e.toneEqualizer = d.toneEqualizer;
        e.chmixer = d.chmixer;
        e.blackwhite = d.blackwhite;
        e.hsl = d.hsl;
        e.filmSimulation = d.filmSimulation;
        e.softlight = d.softlight;
        e.grain = d.grain;
        e.smoothing = d.smoothing;
        e.colorcorrection = d.colorcorrection;
        e.metadata = d.metadata;
        e.gradient = d.gradient;       // (the transform leaves the creative ones to stage 3)
        e.pcvignette = d.pcvignette;
        e.crop = d.crop;               // (cut out after the kept picture)
        e.rank = d.rank;
        e.colorlabel = d.colorlabel;
        e.inTrash = d.inTrash;
        e.appVersion = d.appVersion;
        e.ppVersion = d.ppVersion;
        // (the exposure's highlight reconstruction is read early; its amount and black in stage 1 - but noise reduction
        // works on the photo brightened by a positive amount, as ART's editor redoes it after an exposure change)
        e.exposure.expcomp = all.denoise.enabled && all.exposure.enabled && all.exposure.expcomp > 0 ? all.exposure.expcomp : d.exposure.expcomp;
        e.exposure.black = d.exposure.black;
        e.icm.outputProfile = d.icm.outputProfile;
        e.icm.outputIntent = d.icm.outputIntent;
        e.icm.outputBPC = d.icm.outputBPC;
        if (!fast) {
            e.resize = d.resize;   // (the quick route resizes early: then it counts)
        }
        return e;
    }

    bool same(const MidKey &o) const
    {
        return valid && o.valid && raw.same(o.raw) && std::equal(wb, wb + 6, o.wb) && tr == o.tr && fast == o.fast &&
               scale == o.scale && p == o.p && p.lensProf.useDist == o.p.lensProf.useDist && p.lensProf.useVign == o.p.lensProf.useVign;
    }
};

/** 0.6: what the picture after ART's stage 1 or 2 depends on: the kept middle picture's (MidKey), the crop, and every
 *  setting but those only the later stages (and the output) read. */
struct LateKey {
    bool valid = false;
    MidKey mid;
    procparams::ProcParams p;

    static procparams::ProcParams upTo(const procparams::ProcParams &all, int stage)
    {
        procparams::ProcParams e = all;
        const procparams::ProcParams d;
        if (stage < 2) {   // (stage 2's)
            e.sharpening = d.sharpening;
            e.impulseDenoise = d.impulseDenoise;
            e.defringe = d.defringe;
            e.colorcorrection = d.colorcorrection;
            e.smoothing = d.smoothing;
            e.prsharpening = d.prsharpening;   // (ART's sharpening step reads it too: kept in stage 2's key)
        }
        // (stage 3's)
        e.gradient = d.gradient;
        e.pcvignette = d.pcvignette;
        e.textureBoost = d.textureBoost;
        e.grain = d.grain;
        e.logenc = d.logenc;
        e.saturation = d.saturation;
        e.filmSimulation = d.filmSimulation;
        e.toneCurve = d.toneCurve;
        e.rgbCurves = d.rgbCurves;
        e.labCurve = d.labCurve;
        e.softlight = d.softlight;
        e.localContrast = d.localContrast;
        e.blackwhite = d.blackwhite;
        // (the output's)
        e.resize = d.resize;   // (the quick route's scale is in the middle picture's key)
        e.icm.outputProfile = d.icm.outputProfile;
        e.icm.outputIntent = d.icm.outputIntent;
        e.icm.outputBPC = d.icm.outputBPC;
        e.metadata = d.metadata;
        e.rank = d.rank;
        e.colorlabel = d.colorlabel;
        e.inTrash = d.inTrash;
        e.appVersion = d.appVersion;
        e.ppVersion = d.ppVersion;
        return e;
    }

    bool same(const LateKey &o) const
    {
        return valid && o.valid && mid.same(o.mid) && p == o.p && p.lensProf.useDist == o.p.lensProf.useDist &&
               p.lensProf.useVign == o.p.lensProf.useVign;
    }
};

/** What one photo keeps between requests. */
struct Kept {
    RawKey raw;
    EarlyKey early;
    std::unique_ptr<Imagefloat> earlyImg;
    ImProcFunctions::DenoiseInfoStore dn;   // (the noise measured for automatic chroma noise reduction)
    DnKey dnKey;
    /** 0.3: the photo before ART's colour and tone steps, kept: one per size of picture asked for (the loupe's,
     *  the quick stand-in's, the meter's...), at most three, the least used going. */
    struct Mid {
        MidKey key;
        std::unique_ptr<Imagefloat> img;
        int fw = 0, fh = 0;
        bool stop = false;
        unsigned long used = 0;
    };
    std::vector<Mid> mids;
    unsigned long midClock = 0;
    /** 0.6: the picture after ART's stage 1 and after stage 2, kept: one pair per size and crop of picture (the loupe's,
     *  a part's, the quick stand-in's), at most three, the least used going. Stage 2's only when no mask of a later step
     *  is linked to one of its masks (those are passed from step to step while developing). */
    struct Late {
        LateKey key1, key2;
        std::unique_ptr<Imagefloat> img1, img2;
        bool stop1 = false, stop2 = false;
        bool viewport = false;
        int vx = 0, vy = 0, vw = 0, vh = 0;
        unsigned long used = 0;
    };
    std::vector<Late> lates;

    void dropMid()
    {
        mids.clear();
        lates.clear();
    }
};

/** 0.7: how much is kept ("--memory"): the middle pictures (per size), the later pairs (per size), and up to what size a
 *  later pair is kept (normal: 3, 3, 12 MP; small: 2, none; large: 4, 4, 30 MP). */
struct Memory {
    size_t mids = 3, lates = 3;
    long long lateMaxPixels = 12000000;
};
inline Memory &memory()
{
    static Memory m;
    return m;
}

struct Times {
    double raw = 0, develop = 0;
    bool rawReused = false, earlyReused = false, midReused = false;
    int lateReused = 0;   // 0.6: after ART's stage 1 or 2 (0: neither)
    // 0.7: the picture's geometry: the photo's full size as turned, the crop (in it), the quick route's scale
    int fullW = 0, fullH = 0, cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    bool cropOn = false;
    double scale = 1;
};

enum { STOPPED = -100 };

class Processor {
public:
    /** @param keep 0.6: false - a picture made ahead of a photo kept: what it makes isn't kept (the photo's kept pictures
     *  stay). @param keepLate false: the pictures after stage 1 and 2 aren't kept (a photo made ahead: its middle one is). */
    Processor(ProcessingJobImpl &job, Kept &kept, const std::atomic<bool> &cancel, Times &times, int &errorCode, bool keep = true,
              bool keepLate = true)
        : job(job), kept(kept), cancel(cancel), times(times), errorCode(errorCode), keep(keep), keepLate(keep && keepLate)
    {
    }

    Imagefloat *operator()()
    {
        auto t0 = Clock::now();
        Imagefloat *res = nullptr;
        bool fast = job.fast && job.pparams.resize.enabled;
        try {
            res = run(fast);
        } catch (...) {
            delete img;
            img = nullptr;
            throw;
        }
        if (!res) {
            delete img;
            img = nullptr;
        }
        times.develop = ms(t0, Clock::now()) - times.raw;
        return res;
    }

private:
    Imagefloat *run(bool fast)
    {
        Imagefloat *res = nullptr;
        if (stage_init(fast) && !stopped()) {
            late_keys();   // 0.6 (the settings as the steps before the transform leave them)
            if (midHit) {
                if (!find_late()) {
                    stage_mid_kept();
                }
            } else {
                if (!stopped()) {
                    stage_denoise();
                }
                if (!stopped()) {
                    stage_transform();
                }
                if (!stopped() && keep) {
                    keep_mid();
                }
            }
            if (!stopped()) {
                res = stage_finish(fast);
            }
        }
        return res;
    }

    bool stopped()
    {
        if (cancel.load()) {
            errorCode = STOPPED;
            return true;
        }
        return false;
    }

    bool stage_init(bool is_fast)
    {
        errorCode = 0;
        ii = job.initialImage;
        procparams::ProcParams &params = job.pparams;

        imgsrc = ii->getImageSource();

        tr = getCoarseBitMask(params.coarse);
        int border = 0;
        if (imgsrc->getSensorType() == ST_BAYER) {
            border = params.raw.bayersensor.method != RAWParams::BayerSensor::Method::PIXELSHIFT
                         ? params.raw.bayersensor.border
                         : std::max(params.raw.bayersensor.border, 2);
            imgsrc->setBorder(border);
        } else if (imgsrc->getSensorType() == ST_FUJI_XTRANS) {
            border = params.raw.xtranssensor.border;
            imgsrc->setBorder(border);
        }
        imgsrc->getFullSize(fw, fh, tr);

        if (!params.crop.hasGeometry()) {
            params.crop.setDefaultGeometry(fw, fh);
        }

        if (params.crop.x > fw || params.crop.y > fh) {
            params.crop.enabled = false;
            params.crop.x = 0;
            params.crop.y = 0;
            params.crop.w = fw;
            params.crop.h = fh;
        } else {
            if (params.crop.x < 0) {
                params.crop.x = 0;
            }
            if (params.crop.y < 0) {
                params.crop.y = 0;
            }
            if (params.crop.w <= 0 || (params.crop.x + params.crop.w) > fw) {
                params.crop.w = fw - params.crop.x;
            }
            if (params.crop.h <= 0 || (params.crop.y + params.crop.h) > fh) {
                params.crop.h = fh - params.crop.y;
            }
        }

        ipf_p.reset(new ImProcFunctions(&params, true));
        ImProcFunctions &ipf = *(ipf_p.get());
        ipf.setPipeline(ImProcFunctions::Pipeline::OUTPUT);
        scale_factor = 1.0;
        if (is_fast) {
            int imw, imh;
            scale_factor = ipf.resizeScale(&params, fw, fh, imw, imh);
            adjust_procparams(scale_factor);
        }
        // 0.7: the geometry, for the answer
        times.fullW = fw;
        times.fullH = fh;
        times.cropOn = params.crop.enabled;
        times.cropX = params.crop.x;
        times.cropY = params.crop.y;
        times.cropW = params.crop.w;
        times.cropH = params.crop.h;
        times.scale = scale_factor;

        imgsrc->setCurrentFrame(params.raw.bayersensor.imageNum);

        currWB = ColorTemp();
        if (params.wb.enabled) {
            switch (params.wb.method) {
            case WBParams::CAMERA:
                currWB = imgsrc->getWB();
                break;
            case WBParams::CUSTOM_TEMP:
                currWB = ColorTemp(params.wb.temperature, params.wb.green, params.wb.equal, "Custom");
                break;
            case WBParams::CUSTOM_MULT_LEGACY:
                currWB = ColorTemp(params.wb.mult[0], params.wb.mult[1], params.wb.mult[2], 1.0);
                break;
            case WBParams::CUSTOM_MULT: {
                double rm = params.wb.mult[0];
                double gm = params.wb.mult[1];
                double bm = params.wb.mult[2];
                imgsrc->wbCamera2Mul(rm, gm, bm);
                currWB = ColorTemp(rm, gm, bm);
            } break;
            case WBParams::AUTO:
            default:
                currWB = ColorTemp();
            }
        }

        // the raw steps: reused when what they depend on is the same as last time on this photo
        RawKey key;
        key.valid = true;
        key.raw = params.raw;
        // (the other kind of sensor's settings don't matter - the quick route changes X-Trans ones on any photo)
        if (imgsrc->getSensorType() != ST_FUJI_XTRANS) {
            key.raw.xtranssensor = procparams::RAWParams::XTransSensor();
        }
        if (imgsrc->getSensorType() != ST_BAYER) {
            key.raw.bayersensor = procparams::RAWParams::BayerSensor();
        }
        key.lens = params.lensProf;
        key.coarse = params.coarse;
        key.denoise = params.denoise.enabled;
        key.wbTemp = currWB.getTemp();
        key.wbGreen = currWB.getGreen();
        key.wbEqual = currWB.getEqual();
        if (currWB.getTemp() > 0) {
            currWB.getMultipliers(key.mul[0], key.mul[1], key.mul[2]);
        }
        key.hrOn = params.exposure.enabled;
        key.hrMode = int(params.exposure.hrmode);
        key.hrBlur = params.exposure.hrblur;
        key.frame = params.raw.bayersensor.imageNum;
        key.border = border;
        key.wbOn = params.wb.enabled;
        key.wbMethod = int(params.wb.method);
        key.wbEqualSet = params.wb.equal;

        auto r0 = Clock::now();
        if (key.same(kept.raw)) {
            times.rawReused = true;
        } else {
            kept.dropMid();
            kept.raw.valid = false;
            kept.early.valid = false;
            kept.earlyImg.reset();
            kept.dnKey.valid = false;
            kept.dn.reset();
            AwbReset::reset(imgsrc);
            imgsrc->preprocess(params.raw, params.lensProf, params.coarse, params.denoise.enabled, currWB);
            if (stopped()) {
                return false;
            }
            bool autoContrast = imgsrc->getSensorType() == ST_BAYER
                                    ? params.raw.bayersensor.dualDemosaicAutoContrast
                                    : params.raw.xtranssensor.dualDemosaicAutoContrast;
            double contrastThreshold = imgsrc->getSensorType() == ST_BAYER
                                           ? params.raw.bayersensor.dualDemosaicContrast
                                           : params.raw.xtranssensor.dualDemosaicContrast;
            imgsrc->demosaic(params.raw, autoContrast, contrastThreshold);
            kept.raw = key;
        }
        times.raw = ms(r0, Clock::now());
        if (stopped()) {
            return false;
        }

        if (params.wb.method == WBParams::AUTO) {
            double rm, gm, bm;
            { ART_PROFILE_SCOPE("sb.autoWB"); imgsrc->getAutoWBMultipliers(rm, gm, bm); }
            if (rm != -1.) {
                if (params.wb.equal == 1) {
                    currWB = ColorTemp(rm, gm, bm);
                } else {
                    currWB = ColorTemp(rm, gm, bm, params.wb.equal);
                }
            } else {
                currWB.useDefaults(params.wb.equal);
            }
        }

        pp = PreviewProps(0, 0, fw, fh, 1);

        // the noise measured for automatic chroma noise reduction: kept while what it depends on is the same
        if (params.denoise.enabled) {
            DnKey dkey;
            dkey.valid = true;
            dkey.raw = kept.raw;
            dkey.wb[0] = currWB.getTemp();
            dkey.wb[1] = currWB.getGreen();
            dkey.wb[2] = currWB.getEqual();
            if (currWB.getTemp() > 0) {
                currWB.getMultipliers(dkey.wb[3], dkey.wb[4], dkey.wb[5]);
            }
            dkey.coarse = params.coarse;
            dkey.icm = params.icm;
            // (0.3: nor the output profile - the picture before the film has none: measured once for both)
            dkey.icm.outputProfile = procparams::ColorManagementParams().outputProfile;
            dkey.icm.outputIntent = procparams::ColorManagementParams().outputIntent;
            dkey.icm.outputBPC = procparams::ColorManagementParams().outputBPC;
            dkey.hrOn = params.exposure.enabled;
            dkey.hrMode = int(params.exposure.hrmode);
            dkey.hrBlur = params.exposure.hrblur;
            dkey.denoise = params.denoise;
            if (!dkey.same(kept.dnKey)) {
                kept.dn.reset();
            }
            kept.dnKey = dkey;
            kept.dnKey.valid = false;   // (until it has been measured)
            { ART_PROFILE_SCOPE("sb.denoiseParams"); ipf.denoiseComputeParams(imgsrc, currWB, kept.dn, params.denoise); }
            kept.dnKey.valid = kept.dn.valid;
        }

        // 0.3: the photo before ART's colour and tone steps, kept from last time: the start, when what it depends on is
        // the same (the settings that only later steps read may differ)
        midKey = MidKey();
        midKey.valid = true;
        midKey.raw = kept.raw;
        midKey.wb[0] = currWB.getTemp();
        midKey.wb[1] = currWB.getGreen();
        midKey.wb[2] = currWB.getEqual();
        if (currWB.getTemp() > 0) {
            currWB.getMultipliers(midKey.wb[3], midKey.wb[4], midKey.wb[5]);
        }
        midKey.tr = tr;
        midKey.fast = is_fast;
        midKey.scale = scale_factor;
        midKey.p = MidKey::early(params, is_fast);
        for (auto &m : kept.mids) {
            if (m.img && midKey.same(m.key)) {
                midHit = &m;
                break;
            }
        }
        if (midHit) {
            midHit->used = ++kept.midClock;
            // (0.5: copied in stage_mid_kept - only the crop's part of it, when cropped)
            fw = midHit->fw;
            fh = midHit->fh;
            // (what these steps work out for the later ones, from the image source: as below)
            if (params.toneCurve.histmatching) {
                if (!params.toneCurve.fromHistMatching) {
                    { ART_PROFILE_SCOPE("sb.autoToneCurve"); imgsrc->getAutoMatchedToneCurve(params.icm, params.toneCurve.curve, params.toneCurve.curve2); }
                }
            }
            if (params.logenc.enabled && params.logenc.autocompute) {
                { ART_PROFILE_SCOPE("sb.autoLog"); ipf.getAutoLog(imgsrc, params.logenc); }
            }
            if (params.sharpening.enabled && params.sharpening.deconvAutoRadius) {
                float r = 0.f;
                bool gotRadius = false;
                { ART_PROFILE_SCOPE("sb.autoDeconv"); gotRadius = imgsrc->getDeconvAutoRadius(&r); }
                if (gotRadius) {
                    params.sharpening.deconvradius = r;
                }
            }
            times.midReused = true;
            return true;
        }

        // the small picture ART starts from on the quick route: reused while what it depends on is the same
        EarlyKey ekey;
        if (is_fast) {
            ekey.valid = true;
            ekey.raw = kept.raw;
            ekey.wb[0] = currWB.getTemp();
            ekey.wb[1] = currWB.getGreen();
            ekey.wb[2] = currWB.getEqual();
            if (currWB.getTemp() > 0) {
                currWB.getMultipliers(ekey.wb[3], ekey.wb[4], ekey.wb[5]);
            }
            ekey.tr = tr;
            ekey.fw = fw;
            ekey.fh = fh;
            ekey.scale = scale_factor;
            ekey.imw = fw * scale_factor + 0.5;
            ekey.imh = fh * scale_factor + 0.5;
            ekey.allow = params.resize.allowUpscaling || params.resize.dataspec == 0;
            ekey.spot = params.spot;
            if (!(params.spot.enabled && !params.spot.entries.empty())) {
                ekey.spot = procparams::SpotParams();   // (spots off: their list doesn't matter)
            }
        }
        // (reused: the request that made it read the picture out already, so ART's in-place highlight reconstruction
        // is done, as below for the steps that follow)
        bool reuse = is_fast && kept.earlyImg && ekey.same(kept.early);
        if (reuse) {
            { ART_PROFILE_SCOPE("sb.keptCopy"); img = kept.earlyImg->copy(); }
        } else {
            img = new Imagefloat(fw, fh);
            { ART_PROFILE_SCOPE("sb.getImage"); imgsrc->getImage(currWB, tr, img, pp, params.exposure, params.raw); }
        }
        img->assignColorSpace(params.icm.workingProfile);

        if (params.toneCurve.histmatching) {
            if (!params.toneCurve.fromHistMatching) {
                { ART_PROFILE_SCOPE("sb.autoToneCurve"); imgsrc->getAutoMatchedToneCurve(params.icm, params.toneCurve.curve, params.toneCurve.curve2); }
            }
        }
        if (params.logenc.enabled && params.logenc.autocompute) {
            { ART_PROFILE_SCOPE("sb.autoLog"); ipf.getAutoLog(imgsrc, params.logenc); }
        }
        if (params.sharpening.enabled && params.sharpening.deconvAutoRadius) {
            float r = 0.f;
            bool gotRadius = false;
            { ART_PROFILE_SCOPE("sb.autoDeconv"); gotRadius = imgsrc->getDeconvAutoRadius(&r); }
            if (gotRadius) {
                params.sharpening.deconvradius = r;
            }
        }

        if (reuse) {
            fw = kept.early.outW;
            fh = kept.early.outH;
            times.earlyReused = true;
            return true;
        }

        if (params.spot.enabled && !params.spot.entries.empty()) {
            { ART_PROFILE_SCOPE("sb.spots"); ipf.removeSpots(img, imgsrc, params.spot.entries, pp, currWB, nullptr, tr, nullptr); }
        }
        if (is_fast) {
            if (stopped()) {
                return false;
            }
            { ART_PROFILE_SCOPE("sb.earlyResize"); stage_early_resize(); }
            if (keep) {
                kept.early.valid = false;
                kept.early = ekey;
                kept.early.outW = fw;
                kept.early.outH = fh;
                kept.earlyImg.reset(img->copy());
            }
        }

        return true;
    }

    void sanitize(const char *where)
    {
        const size_t bad = img->sanitize();
        if (bad && settings->verbose) {
            printf("WARNING: replaced %zu NaN samples after %s\n", bad, where);
        }
    }

    void stage_denoise()
    {
        procparams::ProcParams &params = job.pparams;
        ImProcFunctions &ipf = *(ipf_p.get());

        bool converted = false;
        if (params.filmNegative.colorSpace != FilmNegativeParams::ColorSpace::INPUT) {
            { ART_PROFILE_SCOPE("sb.colourIn"); imgsrc->convertColorSpace(img, params.icm, currWB); }
            converted = true;
        }

        if (params.filmNegative.enabled) {
            FilmNegativeParams copy = params.filmNegative;
            ipf.filmNegativeProcess(img, img, copy, params.raw, imgsrc, currWB);
        }

        if (!converted) {
            { ART_PROFILE_SCOPE("sb.colourIn"); imgsrc->convertColorSpace(img, params.icm, currWB); }
        }

        sanitize("input");

        if (params.denoise.enabled) {
            { ART_PROFILE_SCOPE("sb.denoise"); ipf.denoise(imgsrc, currWB, img, params.denoise); }
        }
    }

    void stage_transform()
    {
        procparams::ProcParams &params = job.pparams;
        ImProcFunctions &ipf = *(ipf_p.get());

        sanitize("denoise");

        LUTu hist16(65536);
        { ART_PROFILE_SCOPE("sb.firstAnalysis"); ipf.firstAnalysis(img, params, hist16); }

        stop = ipf.process(ImProcFunctions::Stage::STAGE_0, img);

        if (ipf.needsTransform()) {
            ART_PROFILE_SCOPE("sb.transform");
            if (params.distortion.enabled && params.distortion.autocompute) {
                params.distortion.amount = ImProcFunctions::getAutoDistor(imgsrc->getFileName(), 400);
            }

            Imagefloat *trImg = nullptr;
            if (ipf.needsLuminanceOnly()) {
                trImg = img;
            } else {
                trImg = new Imagefloat(fw, fh, img);
            }
            ipf.transform(img, trImg, 0, 0, 0, 0, fw, fh, fw, fh, imgsrc->getMetaData(), imgsrc->getRotateDegree(),
                          true);
            if (trImg != img) {
                delete img;
                img = trImg;
            }
        }
    }

    /** 0.3: the kept picture is where the transform ended: what the steps before it left for the later ones. */
    void stage_mid_kept()
    {
        procparams::ProcParams &params = job.pparams;
        ImProcFunctions &ipf = *(ipf_p.get());
        const Imagefloat *mid = midHit->img.get();
        LUTu hist16(65536);
        { ART_PROFILE_SCOPE("sb.firstAnalysis"); ipf.firstAnalysis(mid, params, hist16); }   // (the whole picture's, as before)
        stop = midHit->stop;
        // (the automatic distortion is only read by the transform: not worked out again)
        ART_PROFILE_SCOPE("sb.keptCopy");
        // 0.5: cropped - the crop is the first of the later steps: only its part of the kept picture is copied (the same
        // pixels as copying it all and cropping; stage_finish then only tells the steps where the part sits)
        if (params.crop.enabled) {
            const int iw = mid->getWidth(), ih = mid->getHeight();
            const int cx = params.crop.x * scale_factor + 0.5;
            const int cy = params.crop.y * scale_factor + 0.5;
            const int cw = std::min(int(params.crop.w * scale_factor + 0.5), iw - cx);
            const int ch = std::min(int(params.crop.h * scale_factor + 0.5), ih - cy);
            if (cx >= 0 && cy >= 0 && cw > 0 && ch > 0) {
                img = new Imagefloat(cw, ch, mid);
#ifdef _OPENMP
#pragma omp parallel for
#endif
                for (int row = 0; row < ch; row++) {
                    for (int col = 0; col < cw; col++) {
                        img->r(row, col) = mid->r(row + cy, col + cx);
                        img->g(row, col) = mid->g(row + cy, col + cx);
                        img->b(row, col) = mid->b(row + cy, col + cx);
                    }
                }
                preCropped = true;
                preCropX = cx;
                preCropY = cy;
                preCropW = iw;
                preCropH = ih;
                return;
            }
        }
        img = mid->copy();
    }

    /** 0.3: the picture where the transform ended, kept for the next request on this photo. */
    void keep_mid()
    {
        auto &ms = kept.mids;
        for (size_t i = 0; i < ms.size(); ++i) {
            if (ms[i].key.fast == midKey.fast && ms[i].key.scale == midKey.scale) {   // (the same size: replaced)
                ms.erase(ms.begin() + i);
                break;
            }
        }
        // (0.6: the later pictures of that size came from the one replaced: gone too)
        auto &ls = kept.lates;
        ls.erase(std::remove_if(ls.begin(), ls.end(), [&](const Kept::Late &l) {
                     const LateKey &k = l.key1.valid ? l.key1 : l.key2;
                     return k.mid.fast == midKey.fast && k.mid.scale == midKey.scale;
                 }),
                 ls.end());
        while (ms.size() >= std::max<size_t>(1, memory().mids)) {
            size_t oldest = 0;
            for (size_t i = 1; i < ms.size(); ++i) {
                if (ms[i].used < ms[oldest].used) {
                    oldest = i;
                }
            }
            ms.erase(ms.begin() + oldest);
        }
        Kept::Mid m;
        img->syncCpu();   // (kept in the computer's memory: copied there, not on the graphics card)
        { ART_PROFILE_SCOPE("sb.keep"); m.img.reset(img->copy()); }
        m.fw = fw;
        m.fh = fh;
        m.stop = stop;
        m.key = midKey;
        m.used = ++kept.midClock;
        ms.push_back(std::move(m));
    }

    /** 0.6: whether a mask of a later step takes one of an earlier step's (ART passes those along while developing: the
     *  picture after stage 2 can't be the start then). */
    static bool linkedMasks(const procparams::ProcParams &params)
    {
        for (auto p : params.get_maskable()) {
            for (auto &m : p->get_masks()) {
                if (m.enabled && m.linkedMask.enabled && !m.linkedMask.toolname.empty() && !m.linkedMask.name.empty()) {
                    return true;
                }
            }
        }
        return false;
    }

    /** 0.6: the keys of the pictures after stage 1 and 2 for this request (the crop is in the settings). */
    void late_keys()
    {
        const procparams::ProcParams &params = job.pparams;
        lateKey1 = LateKey();
        lateKey1.valid = true;
        lateKey1.mid = midKey;
        lateKey1.p = LateKey::upTo(params, 1);
        lateKey2 = LateKey();
        lateKey2.valid = !linkedMasks(params);
        lateKey2.mid = midKey;
        lateKey2.p = LateKey::upTo(params, 2);
    }

    /** 0.6: the picture after stage 2 (else 1) kept from an earlier request, when what it depends on is the same: the
     *  start (true). The kept middle picture is then not copied. */
    bool find_late()
    {
        for (auto &l : kept.lates) {
            if (l.img2 && lateKey2.same(l.key2)) {
                lateHit = &l;
                lateLevel = 2;
                break;
            }
        }
        if (!lateHit) {
            for (auto &l : kept.lates) {
                if (l.img1 && lateKey1.same(l.key1)) {
                    lateHit = &l;
                    lateLevel = 1;
                    break;
                }
            }
        }
        if (!lateHit) {
            return false;
        }
        lateHit->used = ++kept.midClock;
        times.lateReused = lateLevel;
        return true;
    }

    /** 0.6: this request's slot among the kept later pictures: one per size of picture (the loupe's, the quick
     *  stand-in's, full size - its parts share one: moving the photo replaces it), past three the least used going. */
    Kept::Late &late_slot()
    {
        auto &ls = kept.lates;
        for (auto &l : ls) {
            const LateKey &k = l.key1.valid ? l.key1 : l.key2;
            if (k.mid.fast == midKey.fast && k.mid.scale == midKey.scale) {
                return l;
            }
        }
        while (!ls.empty() && ls.size() >= std::max<size_t>(1, memory().lates)) {
            size_t oldest = 0;
            for (size_t i = 1; i < ls.size(); ++i) {
                if (ls[i].used < ls[oldest].used) {
                    oldest = i;
                }
            }
            ls.erase(ls.begin() + oldest);
        }
        ls.emplace_back();
        return ls.back();
    }

    void keep_late(int level, bool viewport, int vx, int vy, int vw, int vh)
    {
        // (0.6: not for a big picture - a whole photo at full size: two more copies of it would be a lot of memory)
        if (!keepLate || memory().lates == 0 || (long long)img->getWidth() * img->getHeight() > memory().lateMaxPixels) {
            return;
        }
        Kept::Late &l = late_slot();
        l.viewport = viewport;
        l.vx = vx;
        l.vy = vy;
        l.vw = vw;
        l.vh = vh;
        l.used = ++kept.midClock;
        img->syncCpu();   // (kept in the computer's memory)
        ART_PROFILE_SCOPE("sb.keepLate");
        if (level == 1) {
            l.key1 = lateKey1;
            l.img1.reset(img->copy());
            l.stop1 = stop;
            l.key2.valid = false;   // (a stage 2 picture of other settings before it: gone - made again below)
            l.img2.reset();
        } else {
            l.key2 = lateKey2;
            l.img2.reset(img->copy());
            l.stop2 = stop;
        }
    }

    Imagefloat *stage_finish(bool is_fast)
    {
        procparams::ProcParams &params = job.pparams;
        ImProcFunctions &ipf = *(ipf_p.get());

        // 0.6: from the picture after stage 2 or 1, kept
        if (lateHit) {
            return stage_finish_late(is_fast);
        }

        int cx = 0, cy = 0, cw = img->getWidth(), ch = img->getHeight();
        bool viewport = false;
        int vx = 0, vy = 0, vw = 0, vh = 0;
        if (preCropped) {
            ipf.setViewport(preCropX, preCropY, preCropW, preCropH);   // 0.5: cropped already (stage_mid_kept)
            viewport = true;
            vx = preCropX;
            vy = preCropY;
            vw = preCropW;
            vh = preCropH;
        } else if (params.crop.enabled) {
            ART_PROFILE_SCOPE("sb.crop");
            int iw = img->getWidth();
            int ih = img->getHeight();

            cx = params.crop.x * scale_factor + 0.5;
            cy = params.crop.y * scale_factor + 0.5;
            cw = std::min(int(params.crop.w * scale_factor + 0.5), iw - cx);
            ch = std::min(int(params.crop.h * scale_factor + 0.5), ih - cy);

            ipf.setViewport(cx, cy, iw, ih);
            viewport = true;
            vx = cx;
            vy = cy;
            vw = iw;
            vh = ih;

            Imagefloat *tmpimg = new Imagefloat(cw, ch, img);
#ifdef _OPENMP
#pragma omp parallel for
#endif
            for (int row = 0; row < ch; row++) {
                for (int col = 0; col < cw; col++) {
                    tmpimg->r(row, col) = img->r(row + cy, col + cx);
                    tmpimg->g(row, col) = img->g(row + cy, col + cx);
                    tmpimg->b(row, col) = img->b(row + cy, col + cx);
                }
            }

            delete img;
            img = tmpimg;
        }

        DCPProfile::ApplyState as;
        DCPProfile *dcpProf = imgsrc->getDCP(params.icm, as);

        ipf.setDCPProfile(dcpProf, as);
        stop = stop || ipf.process(ImProcFunctions::Stage::STAGE_1, img);
        if (stopped()) {
            return nullptr;
        }
        // 0.6: kept after stage 1 and after stage 2 - only when that stage did something (else the one before serves)
        const bool any1 = params.chmixer.enabled || params.exposure.enabled || params.hsl.enabled ||
                          params.toneEqualizer.enabled || params.icm.workingProfile == "ProPhoto";
        const bool any2 = params.sharpening.enabled || params.impulseDenoise.enabled || params.defringe.enabled ||
                          params.colorcorrection.enabled || params.smoothing.enabled || params.icm.dcp_look_early;
        if (any1 || !any2) {
            keep_late(1, viewport, vx, vy, vw, vh);
        }
        stop = stop || ipf.process(ImProcFunctions::Stage::STAGE_2, img);
        if (stopped()) {
            return nullptr;
        }
        if (any2 && lateKey2.valid) {
            keep_late(2, viewport, vx, vy, vw, vh);
        }
        stop = stop || ipf.process(ImProcFunctions::Stage::STAGE_3, img);
        if (stopped()) {
            return nullptr;
        }
        return stage_output(is_fast);
    }

    /** 0.6: from a picture kept after stage 1 or 2: the stages after it, then the output. */
    Imagefloat *stage_finish_late(bool is_fast)
    {
        procparams::ProcParams &params = job.pparams;
        ImProcFunctions &ipf = *(ipf_p.get());
        {
            // (what the first analysis leaves for the later steps: the working colours' luminance - from any picture)
            ART_PROFILE_SCOPE("sb.firstAnalysis");
            Imagefloat one(1, 1);
            one.r(0, 0) = one.g(0, 0) = one.b(0, 0) = 0.f;
            LUTu hist16(65536);
            ipf.firstAnalysis(&one, params, hist16);
        }
        {
            ART_PROFILE_SCOPE("sb.keptCopy");
            img = (lateLevel == 2 ? lateHit->img2 : lateHit->img1)->copy();
        }
        stop = lateLevel == 2 ? lateHit->stop2 : lateHit->stop1;
        if (lateHit->viewport) {
            ipf.setViewport(lateHit->vx, lateHit->vy, lateHit->vw, lateHit->vh);
        }
        DCPProfile::ApplyState as;
        DCPProfile *dcpProf = imgsrc->getDCP(params.icm, as);
        ipf.setDCPProfile(dcpProf, as);
        if (lateLevel == 1) {
            stop = stop || ipf.process(ImProcFunctions::Stage::STAGE_2, img);
            if (stopped()) {
                return nullptr;
            }
            const bool any2 = params.sharpening.enabled || params.impulseDenoise.enabled || params.defringe.enabled ||
                              params.colorcorrection.enabled || params.smoothing.enabled || params.icm.dcp_look_early;
            if (any2 && lateKey2.valid) {
                Kept::Late *was = lateHit;   // (the slot it came from: the stage 2 picture beside it)
                if (keepLate) {
                    img->syncCpu();
                    ART_PROFILE_SCOPE("sb.keepLate");
                    was->key2 = lateKey2;
                    was->img2.reset(img->copy());
                    was->stop2 = stop;
                }
            }
        }
        stop = stop || ipf.process(ImProcFunctions::Stage::STAGE_3, img);
        if (stopped()) {
            return nullptr;
        }
        return stage_output(is_fast);
    }

    /** The output: resize (not on the quick route), sharpening after it, the output colours, metadata. */
    Imagefloat *stage_output(bool is_fast)
    {
        procparams::ProcParams &params = job.pparams;
        ImProcFunctions &ipf = *(ipf_p.get());

        if (params.resize.enabled) {
            if (!is_fast) {
                int imw, imh;
                double scale = ipf.resizeScale(&params, fw, fh, imw, imh);
                bool allow_upscaling = params.resize.allowUpscaling || params.resize.dataspec == 0;
                if (scale < 1.0 || (scale > 1.0 && allow_upscaling)) {
                    Imagefloat *resized = new Imagefloat(imw, imh, img);
                    { ART_PROFILE_SCOPE("sb.resize"); ipf.Lanczos(img, resized, scale); }
                    delete img;
                    img = resized;
                }
            }
        }
        if (params.prsharpening.enabled) {
            ipf.setScale(1);
            { ART_PROFILE_SCOPE("sb.prsharpening"); ipf.prsharpening(img); }
        }

        sanitize("pipeline");

        Imagefloat *readyImg = nullptr;
        { ART_PROFILE_SCOPE("sb.rgb2out"); readyImg = ipf.rgb2out(img, params.icm); }

        delete img;
        img = nullptr;

        ART_PROFILE_SCOPE("sb.metadata");   // (to the end: the metadata and the output profile)
        Exiv2Metadata info(imgsrc->getFileName());
        switch (params.metadata.mode) {
        case MetaDataParams::TUNNEL:
            readyImg->setMetadata(info);
            break;
        case MetaDataParams::EDIT: {
            auto exifpairs = params.metadata.exif;
            if (params.resize.enabled && params.resize.copyPPIToExif) {
                const Glib::ustring val = Glib::ustring::format(params.resize.ppi) + "/1";
                exifpairs["Exif.Image.XResolution"] = val;
                exifpairs["Exif.Image.YResolution"] = val;
                exifpairs["Exif.Image.ResolutionUnit"] = "2";
            }
            info.setExif(exifpairs);
            info.setIptc(params.metadata.iptc);
            if (!(params.metadata.exifKeys.size() == 1 && params.metadata.exifKeys[0] == "*")) {
                info.setExifKeys(&(params.metadata.exifKeys));
            }
            info.setOutputRating(params, art::gui::options.thumbnail_rating_mode !=
                                             art::gui::Options::ThumbnailRatingMode::PROCPARAMS);
            readyImg->setMetadata(info);
        } break;
        default:
            break;
        }

        if (params.icm.outputProfile != "" && params.icm.outputProfile != ColorManagementParams::NoICMString &&
            params.icm.outputProfile != ColorManagementParams::NoProfileString) {
            cmsHPROFILE jprof = ICCStore::getInstance()->getProfile(params.icm.outputProfile);
            if (jprof != nullptr) {
                ProfileContent pc = ICCStore::getInstance()->getContent(params.icm.outputProfile);
                readyImg->setOutputProfile(pc.getData().c_str(), pc.getData().size());
            }
        } else if (params.icm.outputProfile == ColorManagementParams::NoProfileString) {
            cmsHPROFILE wp = ICCStore::getInstance()->workingSpace(params.icm.workingProfile);
            ProfileContent wpc(wp);
            readyImg->setOutputProfile(wpc.getData().c_str(), wpc.getData().size());
        } else {
            readyImg->setOutputProfile(nullptr, 0);
        }

        return readyImg;
    }

    void stage_early_resize()
    {
        procparams::ProcParams &params = job.pparams;
        ImProcFunctions &ipf = *(ipf_p.get());

        if (scale_factor == 1.f) {
            return;
        }

        int imw = fw * scale_factor + 0.5;
        int imh = fh * scale_factor + 0.5;

        bool allow_upscaling = params.resize.allowUpscaling || params.resize.dataspec == 0;
        if (allow_upscaling || (imw <= fw && imh <= fh)) {
            Imagefloat *resized = new Imagefloat(imw, imh, img);
            ipf.Lanczos(img, resized, scale_factor);
            delete img;
            img = resized;
        }

        fw = imw;
        fh = imh;
    }

    void adjust_procparams(double scale_factor)
    {
        procparams::ProcParams &params = job.pparams;
        ImProcFunctions &ipf = *(ipf_p.get());
        ipf.setScale(1.0 / scale_factor);

        if (params.raw.xtranssensor.method == procparams::RAWParams::XTransSensor::Method::THREE_PASS) {
            params.raw.xtranssensor.method = procparams::RAWParams::XTransSensor::Method::ONE_PASS;
        }

        if (params.raw.bayersensor.method == procparams::RAWParams::BayerSensor::Method::PIXELSHIFT) {
            params.raw.bayersensor.method = procparams::RAWParams::BayerSensor::Method::AMAZE;
        }
    }

    ProcessingJobImpl &job;
    Kept &kept;
    const std::atomic<bool> &cancel;
    Times &times;
    int &errorCode;

    std::unique_ptr<ImProcFunctions> ipf_p;
    InitialImage *ii = nullptr;
    ImageSource *imgsrc = nullptr;
    int fw = 0;
    int fh = 0;
    double scale_factor = 1.0;
    int tr = 0;
    PreviewProps pp = PreviewProps(0, 0, 0, 0, 0);
    ColorTemp currWB;
    Imagefloat *img = nullptr;
    bool preCropped = false;   // 0.5: the kept picture's crop taken already (where it sat, in what size of picture)
    int preCropX = 0, preCropY = 0, preCropW = 0, preCropH = 0;
    bool stop = false;
    MidKey midKey;
    Kept::Mid *midHit = nullptr;
    bool keep = true;
    bool keepLate = true;
    LateKey lateKey1, lateKey2;   // 0.6
    Kept::Late *lateHit = nullptr;
    int lateLevel = 0;
};

} // namespace sb
}} // namespace art::engine

// ---- the photos kept, and ART-cli's arguments ----

using namespace art::gui;
namespace pp = art::engine::procparams;

namespace {

typedef std::unique_ptr<pp::PartialProfile> PartialProfile;

bool check_partial_profile(const PartialProfile &p)
{
    pp::ProcParams params;
    return p->applyTo(params);
}

Glib::ustring getExtension(const Glib::ustring &filename)
{
    Glib::ustring::size_type lastdot = filename.find_last_of('.');
    if (lastdot != Glib::ustring::npos) {
        return filename.substr(lastdot + 1);
    }
    return "";
}

/** One photo loaded (with its raw steps' result in ART's image source). */
struct KeptPhoto {
    Glib::ustring path;
    gint64 mtime = 0;
    goffset size = 0;
    bool isRaw = true;
    art::engine::InitialImage *ii = nullptr;
    art::engine::sb::Kept kept;
    Clock::time_point used;
};

class Photos {
public:
    explicit Photos(size_t keep) : keep_(std::max<size_t>(1, keep)) {}

    ~Photos()
    {
        clear();
    }

    /** The photo, loaded (or kept from before and unchanged on disk); null: it couldn't be loaded. */
    /** @param ahead 0.6: a picture made ahead ("-N"): a photo not kept is loaded into a place of its own (one photo: the
     *  last made ahead), the photos kept stay as they are; asked for normally later, it moves in among them. */
    KeptPhoto *get(const Glib::ustring &path, bool isRaw, bool &reused, int &errorCode, bool ahead = false)
    {
        gint64 mtime = -1;
        goffset size = -1;
        try {
            auto info = Gio::File::create_for_path(path)->query_info("time::modified,time::modified-usec,standard::size");
            mtime = gint64(info->get_attribute_uint64("time::modified")) * 1000000 +
                    info->get_attribute_uint32("time::modified-usec");
            size = info->get_size();
        } catch (Glib::Exception &) {
            mtime = -1;   // (not known: never taken for the same file again)
        }
        for (size_t i = 0; i < kept_.size(); ++i) {
            auto &k = kept_[i];
            if (k->path == path && k->isRaw == isRaw) {
                if (mtime >= 0 && k->mtime == mtime && k->size == size) {
                    if (!ahead) {
                        k->used = Clock::now();   // (0.6: made ahead: not counted as used - it mustn't outlast the photo being edited)
                    }
                    reused = true;
                    return k.get();
                }
                k->ii->decreaseRef();
                kept_.erase(kept_.begin() + i);
                break;
            }
        }
        // 0.6: the photo last made ahead
        std::unique_ptr<KeptPhoto> fromAhead;
        if (ahead_ && ahead_->path == path && ahead_->isRaw == isRaw) {
            if (mtime >= 0 && ahead_->mtime == mtime && ahead_->size == size) {
                if (ahead) {
                    reused = true;
                    return ahead_.get();
                }
                fromAhead = std::move(ahead_);   // (asked for now: it moves in among the photos kept)
            } else {
                dropAhead();
            }
        }
        if (fromAhead) {
            reused = true;
        } else {
            reused = false;
        }
        errorCode = 0;
        art::engine::InitialImage *ii = fromAhead ? fromAhead->ii : nullptr;
        if (!ii) {
            ii = art::engine::InitialImage::load(path, isRaw, &errorCode, nullptr);
        }
        if (!ii) {
            return nullptr;
        }
        if (ahead) {
            dropAhead();
            ahead_.reset(new KeptPhoto());
            ahead_->path = path;
            ahead_->mtime = mtime;
            ahead_->size = size;
            ahead_->isRaw = isRaw;
            ahead_->ii = ii;
            ahead_->used = Clock::now();
            return ahead_.get();
        }
        if (fromAhead) {
            while (kept_.size() >= keep_) {
                dropOldest();
            }
            fromAhead->used = Clock::now();
            kept_.push_back(std::move(fromAhead));
            return kept_.back().get();
        }
        while (kept_.size() >= keep_) {
            size_t oldest = 0;
            for (size_t i = 1; i < kept_.size(); ++i) {
                if (kept_[i]->used < kept_[oldest]->used) {
                    oldest = i;
                }
            }
            kept_[oldest]->ii->decreaseRef();
            kept_.erase(kept_.begin() + oldest);
        }
        auto k = std::unique_ptr<KeptPhoto>(new KeptPhoto());
        k->path = path;
        k->mtime = mtime;
        k->size = size;
        k->isRaw = isRaw;
        k->ii = ii;
        k->used = Clock::now();
        kept_.push_back(std::move(k));
        return kept_.back().get();
    }

    /** 0.6: whether it is the photo last made ahead (its own place). */
    bool isAhead(const KeptPhoto *p) const { return ahead_ && ahead_.get() == p; }

    /** 0.3: the middle picture kept for this photo only (it is the size of the photo: one at a time). */
    void keepMidOnly(KeptPhoto *photo)
    {
        for (auto &k : kept_) {
            if (k.get() != photo) {
                k->kept.dropMid();
            }
        }
    }

    /** Every photo let go (after an error part way: what was kept may be half changed). */
    void clear()
    {
        for (auto &k : kept_) {
            k->ii->decreaseRef();
        }
        kept_.clear();
        dropAhead();
    }

private:
    void dropAhead()
    {
        if (ahead_) {
            ahead_->ii->decreaseRef();
            ahead_.reset();
        }
    }

    void dropOldest()
    {
        size_t oldest = 0;
        for (size_t i = 1; i < kept_.size(); ++i) {
            if (kept_[i]->used < kept_[oldest]->used) {
                oldest = i;
            }
        }
        kept_[oldest]->ii->decreaseRef();
        kept_.erase(kept_.begin() + oldest);
    }

    size_t keep_;
    std::vector<std::unique_ptr<KeptPhoto>> kept_;
    std::unique_ptr<KeptPhoto> ahead_;   // 0.6: the photo last made ahead (its own place: the kept ones stay)
};

struct Result {
    int code = 0;
    std::string message;
    std::string steps;   // 0.4: ART's steps' times (see stepTimes)
    double load = 0, raw = 0, develop = 0, save = 0;
    bool photoReused = false, rawReused = false, earlyReused = false, midReused = false;
    int lateReused = 0;   // 0.6: the picture after ART's stage 1 or 2
    std::string geo;      // 0.7: the picture's geometry (see reusedText)
};

/** 0.3: a picture as three planes of 32-bit numbers (red, then green, then blue; each row after row), after a header:
 *  "SBRF", width, height, 3 (32-bit, little-endian). The values as ART's 32-bit TIFF has them. */
int saveRawPlanes(art::engine::Imagefloat *img, const Glib::ustring &fname)
{
    img->syncCpu();
    const int w = img->getWidth(), h = img->getHeight();
    std::vector<char> buf;
    std::vector<float> row;
    try {
        buf.resize(1 << 22);
        row.resize(w);
    } catch (std::bad_alloc &) {
        return 1;
    }
    FILE *f = g_fopen(fname.c_str(), "wb");
    if (!f) {
        return 1;
    }
    setvbuf(f, buf.data(), _IOFBF, buf.size());
    const int32_t head[4] = {0x46524253, w, h, 3};   // ("SBRF")
    bool ok = fwrite(head, sizeof(head), 1, f) == 1;
    for (int c = 0; c < 3 && ok; ++c) {
        for (int y = 0; y < h && ok; ++y) {
            for (int x = 0; x < w; ++x) {
                const float v = c == 0 ? img->r(y, x) : c == 1 ? img->g(y, x) : img->b(y, x);
                row[x] = (std::isnan(v) ? 0.f : v) / 65535.f;   // (as Imagefloat::getScanline for a float file)
            }
            ok = fwrite(row.data(), sizeof(float), w, f) == (size_t)w;
        }
    }
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        g_remove(fname.c_str());
    }
    return ok ? 0 : 1;
}

/** 0.5: the colour profile ART set on its picture for the output (ART keeps it to itself). */
struct ProfileOf: art::engine::ImageIO {
    static std::pair<const char *, int> get(const art::engine::ImageIO *i)
    {
        return {i->*(&ProfileOf::profileData), i->*(&ProfileOf::profileLength)};
    }
};

/** 0.5: StopBath's quick picture ("-Q"): an uncompressed 8-bit TIFF (little-endian, one strip, R G B, the output's
 *  colour profile, no other metadata), the pixels as ART-cli's 8-bit TIFF has them (as Imagefloat::getScanline makes
 *  them). ART's own saving writes the file, then reads and writes it all again to add the photo's metadata - a
 *  quarter of a second on a full-size picture; StopBath shows the picture and needs none of it. */
int saveQuickTiff(art::engine::Imagefloat *img, const Glib::ustring &fname)
{
    img->syncCpu();
    const int w = img->getWidth(), h = img->getHeight();
    const auto prof = ProfileOf::get(img);
    const uint32_t iccLen = prof.first && prof.second > 0 ? (uint32_t)prof.second : 0;
    const uint64_t bytes = (uint64_t)w * h * 3;
    if (w <= 0 || h <= 0 || bytes > 0xFFFFFFF0ull - 4096 - iccLen) {
        return 1;
    }
    std::vector<unsigned char> px;
    try {
        px.resize(bytes);
    } catch (std::bad_alloc &) {
        return 1;
    }
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (int y = 0; y < h; ++y) {
        unsigned char *o = px.data() + (size_t)y * w * 3;
        for (int x = 0; x < w; ++x) {
            *o++ = art::engine::uint16ToUint8Rounded(art::engine::CLIP(img->r(y, x)));
            *o++ = art::engine::uint16ToUint8Rounded(art::engine::CLIP(img->g(y, x)));
            *o++ = art::engine::uint16ToUint8Rounded(art::engine::CLIP(img->b(y, x)));
        }
    }
    // the header: the directory, the bits per sample, the profile, then the pixels
    const uint16_t entries = iccLen ? 12 : 11;
    const uint32_t ifdAt = 8, bitsAt = ifdAt + 2 + entries * 12 + 4, iccAt = bitsAt + 6;
    const uint32_t dataAt = iccAt + ((iccLen + 1) / 2) * 2;
    std::vector<unsigned char> head;
    auto u16 = [&](uint16_t v) { head.push_back(v & 0xff); head.push_back(v >> 8); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) head.push_back((v >> (8 * i)) & 0xff); };
    auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
        u16(tag);
        u16(type);
        u32(count);
        if (type == 3 && count == 1) {   // (a SHORT value sits in the first two bytes)
            u16((uint16_t)value);
            u16(0);
        } else {
            u32(value);
        }
    };
    head.push_back('I');
    head.push_back('I');
    u16(42);
    u32(ifdAt);
    u16(entries);
    entry(256, 4, 1, w);
    entry(257, 4, 1, h);
    entry(258, 3, 3, bitsAt);
    entry(259, 3, 1, 1);   // no compression
    entry(262, 3, 1, 2);   // RGB
    entry(273, 4, 1, dataAt);
    entry(274, 3, 1, 1);   // top left
    entry(277, 3, 1, 3);
    entry(278, 4, 1, h);
    entry(279, 4, 1, (uint32_t)bytes);
    entry(339, 3, 1, 1);   // unsigned integers
    if (iccLen) {
        entry(34675, 7, iccLen, iccAt);
    }
    u32(0);
    u16(8);
    u16(8);
    u16(8);
    if (iccLen) {
        head.insert(head.end(), prof.first, prof.first + iccLen);
        if (iccLen % 2) {
            head.push_back(0);
        }
    }
    FILE *f = g_fopen(fname.c_str(), "wb");
    if (!f) {
        return 1;
    }
    bool ok = fwrite(head.data(), 1, head.size(), f) == head.size() && fwrite(px.data(), 1, px.size(), f) == px.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        g_remove(fname.c_str());
    }
    return ok ? 0 : 1;
}

const int Unsupported = 99;
const int Stopped = 98;

/** One ART-cli run (its arguments as StopBath gives them), done here: the same settings, pipeline and files. */
Result runCli(const std::vector<Glib::ustring> &args, Photos &photos, const std::atomic<bool> &cancel,
              bool anyExtension = false)
{
    Result res;
    auto fail = [&](int code, const std::string &msg) {
        res.code = code;
        res.message = msg;
        return res;
    };

    std::vector<Glib::ustring> inputFiles;
    std::vector<PartialProfile> processingParams;
    Glib::ustring outputPath;
    bool overwriteFiles = false, copyParamsFile = false, useDefault = false, fast = false, ahead = false;
    int compression = 92, subsampling = 3, bits = -1;
    bool isFloat = false;
    std::string outputType;

    for (size_t i = 0; i < args.size(); ++i) {
        const Glib::ustring &a = args[i];
        if (a.empty()) {
            continue;
        }
        if (a[0] != '-' || a.size() < 2) {
            return fail(Unsupported, "unexpected argument: " + a);
        }
        switch (a[1]) {
        case 'O':
            copyParamsFile = true;
            // fall through
        case 'o':
            if (i + 1 >= args.size()) {
                return fail(Unsupported, "-o without a file");
            }
            outputPath = args[++i];
            if (Glib::file_test(outputPath, Glib::FILE_TEST_IS_DIR) || outputPath.substr(0, 9) == "/dev/null") {
                return fail(Unsupported, "output to a folder");
            }
            break;
        case 'p': {
            if (i + 1 >= args.size()) {
                return fail(Unsupported, "-p without a file");
            }
            Glib::ustring fname = args[++i];
            PartialProfile current(new pp::FilePartialProfile(nullptr, fname, false));
            if (!check_partial_profile(current)) {
                return fail(-3, "Error: \"" + fname + "\" not found.");
            }
            processingParams.push_back(std::move(current));
        } break;
        case 'd':
            useDefault = true;
            break;
        case 'q':
        case 'V':
            break;
        case 'Y':
            overwriteFiles = true;
            break;
        case 'j':
            if (a.size() > 2 && a[2] == 's') {
                subsampling = atoi(a.substr(3).c_str());
                if (subsampling < 1 || subsampling > 3) {
                    return fail(-3, "Error: the value accompanying the -js switch has to be in the [1-3] range!");
                }
            } else {
                outputType = "jpg";
                compression = a.size() < 3 ? 92 : atoi(a.substr(2).c_str());
                if (compression < 0 || compression > 100) {
                    return fail(-3, "Error: the value accompanying the -j switch has to be in the [0-100] range!");
                }
            }
            break;
        case 'b':
            bits = -1;
            if (a.size() >= 3 && a[2] == '8') {
                bits = 8;
            } else if (a.size() >= 4 && a.size() <= 5 && a[2] == '1' && a[3] == '6') {
                bits = 16;
                isFloat = a.size() == 5 && a[4] == 'f';
            } else if (a.size() >= 4 && a.size() <= 5 && a[2] == '3' && a[3] == '2') {
                bits = 32;
                isFloat = true;
            }
            if (bits == -1) {
                return fail(-3, "Error: specify output bit depth per channel as -b8, -b16, -b16f or -b32.");
            }
            break;
        case 't':
            outputType = "tif";
            compression = (a.size() < 3 || a[2] != 'z') ? 0 : 1;
            break;
        case 'n':
            outputType = "png";
            compression = -1;
            break;
        case 'R':   // 0.3: StopBath's own: three planes of 32-bit numbers, to the file named as it is
            outputType = "sbraw";
            bits = 32;
            isFloat = true;
            break;
        case 'Q':   // 0.5: StopBath's own: a quick 8-bit TIFF (no metadata), to the file named as it is
            outputType = "sbtif";
            bits = 8;
            isFloat = false;
            break;
        case 'f':
            fast = true;
            break;
        case 'N':   // 0.6: StopBath's own: a picture made ahead - nothing kept for it
            ahead = true;
            break;
        case 'c':
            for (++i; i < args.size(); ++i) {
                inputFiles.push_back(args[i]);
            }
            break;
        default:
            return fail(Unsupported, "not done by the helper: " + a);
        }
    }

    if (inputFiles.size() != 1) {
        return fail(Unsupported, "one photo at a time");
    }
    const Glib::ustring inputFile = inputFiles[0];
    if (!Glib::file_test(inputFile, Glib::FILE_TEST_IS_REGULAR)) {
        return fail(2, "\"" + inputFile + "\" doesn't exist!");
    }
    if (!anyExtension && !options.has_retained_extention(inputFile)) {
        return fail(2, "\"" + inputFile + "\" is not one of the selected parsed extensions. Image skipped.");
    }

    if (bits == -1) {
        bits = outputType == "jpg" || outputType == "png" ? 8 : outputType == "tif" ? 16 : 32;
    }
    if (outputType.empty()) {
        outputType = "jpg";
    }

    std::unordered_map<std::string, Glib::ustring> output_ext;
    for (auto &p : art::engine::ImageIOManager::getInstance()->getSaveFormats()) {
        output_ext[p.first] = p.second.extension;
    }
    output_ext["jpg"] = "jpg";
    output_ext["tif"] = "tif";
    output_ext["png"] = "png";
    auto oext = output_ext[outputType];
    if (oext.empty()) {
        oext = outputType;
    }

    Glib::ustring outputFile;
    if (outputType == "sbraw" || outputType == "sbtif") {
        if (outputPath.empty()) {
            return fail(Unsupported, "-R / -Q without -o");
        }
        outputFile = outputPath;
    } else if (outputPath.empty()) {
        outputFile = inputFile.substr(0, inputFile.find_last_of('.')) + "." + oext;
    } else {
        outputFile = outputPath.substr(0, outputPath.find_last_of('.')) + "." + oext;
    }
    if (inputFile == outputFile) {
        return fail(0, "cannot overwrite: " + inputFile);
    }
    if (!overwriteFiles && Glib::file_test(outputFile, Glib::FILE_TEST_EXISTS)) {
        return fail(0, outputFile + " already exists: use -Y option to overwrite. This image has been skipped.");
    }

    PartialProfile rawParams, imgParams;
    if (useDefault) {
        Glib::ustring profPath = options.findProfilePath(options.defProfRaw);
        if (profPath == Options::DEFPROFILE_INTERNAL) {
            rawParams.reset(new pp::FullPartialProfile());
        } else {
            Glib::ustring fname =
                Glib::build_filename(profPath, Glib::path_get_basename(options.defProfRaw) + paramFileExtension);
            rawParams.reset(new pp::FilePartialProfile(nullptr, fname, false));
        }
        if (options.is_defProfRawMissing() || profPath.empty() ||
            (profPath != Options::DEFPROFILE_DYNAMIC && !check_partial_profile(rawParams))) {
            return fail(-3, "Error: default raw processing profile not found.");
        }
        profPath = options.findProfilePath(options.defProfImg);
        if (profPath == Options::DEFPROFILE_INTERNAL) {
            imgParams.reset(new pp::FullPartialProfile());
        } else {
            auto fname =
                Glib::build_filename(profPath, Glib::path_get_basename(options.defProfImg) + paramFileExtension);
            imgParams.reset(new pp::FilePartialProfile(nullptr, fname, false));
        }
        if (options.is_defProfImgMissing() || profPath.empty() ||
            (profPath != Options::DEFPROFILE_DYNAMIC && !check_partial_profile(imgParams))) {
            return fail(-3, "Error: default non-raw processing profile not found.");
        }
    }

    Glib::ustring ext = getExtension(inputFile).lowercase();
    bool isRaw = !(ext == "jpg" || ext == "jpeg" || ext == "tif" || ext == "tiff" || ext == "png" ||
                   art::engine::ImageIOManager::getInstance()->canLoad(ext));

    auto t0 = Clock::now();
    int errorCode = 0;
    KeptPhoto *photo = photos.get(inputFile, isRaw, res.photoReused, errorCode, ahead);
    res.load = ms(t0, Clock::now());
    if (!photo) {
        return fail(-2, "impossible to load file: " + inputFile);
    }
    if (cancel.load()) {
        return fail(Stopped, "stopped");
    }

    pp::ProcParams currentParams;
    if (useDefault) {
        if (isRaw) {
            if (options.defProfRaw == Options::DEFPROFILE_DYNAMIC) {
                rawParams = art::engine::ProfileStore::getInstance()->loadDynamicProfile(photo->ii->getMetaData());
            }
            rawParams->applyTo(currentParams);
        } else {
            if (options.defProfImg == Options::DEFPROFILE_DYNAMIC) {
                imgParams = art::engine::ProfileStore::getInstance()->loadDynamicProfile(photo->ii->getMetaData());
            }
            imgParams->applyTo(currentParams);
        }
    }
    for (auto &p : processingParams) {
        p->applyTo(currentParams);
    }
    if (auto p = art::engine::ImageIOManager::getInstance()->getSaveProfile(outputType)) {
        p->applyTo(currentParams);
    }

    std::unique_ptr<art::engine::ProcessingJobImpl> job(
        static_cast<art::engine::ProcessingJobImpl *>(create_processing_job(photo->ii, currentParams, fast)));
    if (!job) {
        return fail(-2, "impossible to create processing job for: " + inputFile);
    }

    art::engine::sb::Times times;
    art::engine::Imagefloat *resultImage = nullptr;
    if (!ahead) {
        photos.keepMidOnly(photo);   // (0.3: before this one is developed: at most this photo's in memory)
    }
    {
        // (0.6: made ahead: the photo in its own place keeps its middle pictures - a film's second picture starts there -
        // but no later ones; a photo kept for editing keeps nothing new)
        const bool own = photos.isAhead(photo);
        art::engine::sb::Processor proc(*job, photo->kept, cancel, times, errorCode, !ahead || own, !ahead);
        resultImage = proc();
    }
    job.reset();
    res.raw = times.raw;
    res.develop = times.develop;
    res.rawReused = times.rawReused;
    res.earlyReused = times.earlyReused;
    res.midReused = times.midReused;
    res.lateReused = times.lateReused;
    if (resultImage) {
        // 0.7: full=<the full-size picture for these settings: the crop's size, else the photo's>,
        // photo=<the photo's size as turned>, crop=<x x y of the crop in it, or 0x0>, scale=<the quick route's>, out=<made>
        char g[200];
        const int fullW = times.cropOn ? times.cropW : times.fullW, fullH = times.cropOn ? times.cropH : times.fullH;
        snprintf(g, sizeof(g), "full=%dx%d;photo=%dx%d;crop=%dx%d;scale=%.9g;out=%dx%d", fullW, fullH, times.fullW, times.fullH,
                 times.cropOn ? times.cropX : 0, times.cropOn ? times.cropY : 0, times.scale, resultImage->getWidth(),
                 resultImage->getHeight());
        res.geo = g;
    }
    if (!resultImage) {
        if (errorCode == art::engine::sb::STOPPED) {
            return fail(Stopped, "stopped");
        }
        return fail(-2, "failure in processing: " + inputFile);
    }

    auto s0 = Clock::now();
    if (outputType == "sbraw") {
        errorCode = saveRawPlanes(resultImage, outputFile);
    } else if (outputType == "sbtif") {
        errorCode = saveQuickTiff(resultImage, outputFile);
    } else if (outputType == "jpg") {
        errorCode = resultImage->saveAsJPEG(outputFile, compression, subsampling);
    } else if (outputType == "tif") {
        errorCode = resultImage->saveAsTIFF(outputFile, bits, isFloat, compression == 0);
    } else if (outputType == "png") {
        errorCode = resultImage->saveAsPNG(outputFile, bits);
    } else {
        errorCode = art::engine::ImageIOManager::getInstance()->save(resultImage, outputType, outputFile, nullptr) ? 0 : 1;
    }
    if (errorCode) {
        resultImage->free();
        return fail(-2, "failure in saving to: " + outputFile);
    }
    if (copyParamsFile) {
        Glib::ustring outputProcessingParams = outputFile + paramFileExtension;
        if (!options.params_out_embed || currentParams.saveEmbedded(nullptr, outputFile) != 0) {
            currentParams.save(nullptr, outputProcessingParams);
        }
    }
    resultImage->free();
    res.save = ms(s0, Clock::now());
    return res;
}

std::string oneLine(std::string s)
{
    for (auto &c : s) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return s;
}

std::mutex sayLock;

void say(const std::string &line)
{
    std::lock_guard<std::mutex> lock(sayLock);
    std::cout.flush();
    fputs(("\n@@SB " + line + "\n").c_str(), stdout);   // (on a line of its own whatever ART printed last)
    fflush(stdout);
}

std::string reusedText(const Result &r)
{
    std::string s = r.photoReused ? "photo:kept" : "photo:loaded";
    s += r.rawReused ? ",raw:reused" : ",raw:made";
    s += r.earlyReused ? ",small:reused" : "";
    s += r.midReused ? ",mid:reused" : "";
    s += r.lateReused == 2 ? ",late:2" : r.lateReused == 1 ? ",late:1" : "";
    s += r.geo.empty() ? "" : ",geo:" + r.geo;
    char t[160];
    snprintf(t, sizeof(t), ",load:%.0f,raw:%.0f,develop:%.0f,save:%.0f", r.load, r.raw, r.develop, r.save);
    return s + t + (r.steps.empty() ? "" : ",steps:" + r.steps);
}

/** 0.4: ART's steps' times since the last call (ART's per-step timing, read from its table and emptied), slowest
 *  first, those of a millisecond or more, as name=ms;name=ms. ART's own names with ":" (the parts of noise reduction)
 *  are left out: they are inside "sb.denoise". */
std::string stepTimes()
{
    std::ostringstream table;
    std::ios format(nullptr);
    format.copyfmt(std::cerr);   // (the table's number format isn't left on standard error)
    std::streambuf *was = std::cerr.rdbuf(table.rdbuf());
    try {
        art::engine::PipelineProfile::report("");
    } catch (...) {
    }
    std::cerr.rdbuf(was);
    std::cerr.copyfmt(format);
    std::vector<std::pair<std::string, double>> v;
    std::istringstream in(table.str());
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string name;
        double msTook = 0, pct = 0, cum = 0;
        long calls = 0;
        if (!(ls >> name >> msTook >> pct >> cum >> calls) || name.find(':') != std::string::npos || msTook < 1) {
            continue;
        }
        for (auto &c : name) {
            if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
                c = '_';
            }
        }
        v.emplace_back(name, msTook);
    }
    std::sort(v.begin(), v.end(), [](const std::pair<std::string, double> &a, const std::pair<std::string, double> &b) {
        return a.second > b.second;
    });
    std::string out;
    char t[64];
    for (size_t i = 0; i < v.size() && i < 16; ++i) {
        snprintf(t, sizeof(t), "%s%s=%.0f", out.empty() ? "" : ";", v[i].first.c_str(), v[i].second);
        out += t;
    }
    return out;
}

// ---- serving StopBath ----

struct Request {
    std::string id;
    std::vector<Glib::ustring> args;
    bool quit = false;
};

std::string trimEnd(std::string s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

int serve(size_t keep)
{
    Photos photos(keep);
    std::mutex m;
    std::condition_variable cv;
    std::deque<Request> queue;
    std::string current;
    std::atomic<bool> cancel(false);

    std::thread reader([&]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            line = trimEnd(line);
            std::istringstream words(line);
            std::string verb;
            words >> verb;
            if (verb == "run") {
                Request r;
                size_t count = 0;
                words >> r.id >> count;
                bool complete = true;
                for (size_t i = 0; i < count; ++i) {
                    std::string a;
                    if (!std::getline(std::cin, a)) {
                        complete = false;
                        break;
                    }
                    r.args.emplace_back(trimEnd(a));
                }
                if (!complete) {
                    break;
                }
                std::lock_guard<std::mutex> lock(m);
                queue.push_back(std::move(r));
                cv.notify_one();
            } else if (verb == "cancel") {
                std::string id;
                words >> id;
                std::lock_guard<std::mutex> lock(m);
                if (id == current) {
                    cancel.store(true);
                }
                for (auto it = queue.begin(); it != queue.end(); ++it) {
                    if (it->id == id) {
                        say("done " + id + " " + std::to_string(Stopped) + " 0 - stopped");
                        queue.erase(it);
                        break;
                    }
                }
            } else if (verb == "quit") {
                break;
            }
        }
        // standard input closed or "quit": StopBath has gone
        std::lock_guard<std::mutex> lock(m);
        Request q;
        q.quit = true;
        queue.push_back(q);
        cv.notify_one();
    });
    reader.detach();

    say(std::string("ready ") + HelperVersion + " " + RTVERSION);

    while (true) {
        Request r;
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait(lock, [&]() { return !queue.empty(); });
            r = std::move(queue.front());
            queue.pop_front();
            if (r.quit) {
                break;
            }
            current = r.id;
            cancel.store(false);
        }
        auto t0 = Clock::now();
        Result res;
        stepTimes();   // (0.4: whatever an earlier request left in ART's table: not this one's)
        try {
            res = runCli(r.args, photos, cancel);
        } catch (std::exception &e) {
            res.code = -2;
            res.message = std::string("error: ") + e.what();
            photos.clear();
        } catch (Glib::Exception &e) {
            res.code = -2;
            res.message = "error: " + std::string(e.what());
            photos.clear();
        } catch (...) {
            res.code = -2;
            res.message = "error";
            photos.clear();
        }
        res.steps = stepTimes();
        {
            std::lock_guard<std::mutex> lock(m);
            current.clear();
        }
        char t[32];
        snprintf(t, sizeof(t), "%.0f", ms(t0, Clock::now()));
        say("done " + r.id + " " + std::to_string(res.code) + " " + t + " " + reusedText(res) + " " +
            oneLine(res.message));
    }
    std::_Exit(0);   // (the reader thread may still wait on standard input; ART's engine needs no tidying)
    return 0;
}

int render(int argc, char **argv)
{
    if (argc < 4) {
        std::cerr << "usage: stopbath-engine --render <photo> <out.jpg> [--profile <file.arp>]... [--fast] [--repeat <n>]"
                  << std::endl;
        return 2;
    }
    std::vector<Glib::ustring> args = {"-o", argv[3], "-j92", "-js3", "-Y"};
    std::vector<Glib::ustring> profiles;
    int repeat = 1;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            args.push_back("-p");
            args.push_back(argv[++i]);
        } else if (!strcmp(argv[i], "--fast")) {
            args.push_back("-f");
        } else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) {
            repeat = std::max(1, atoi(argv[++i]));
        }
    }
    args.push_back("-c");
    args.push_back(argv[2]);

    Photos photos(1);
    std::atomic<bool> cancel(false);
    auto t0 = Clock::now();
    for (int n = 1; n <= repeat; ++n) {
        auto res = runCli(args, photos, cancel, true);   // (a test: any picture ART can read)
        if (res.code != 0 || !res.message.empty()) {
            std::cerr << res.message << std::endl;
            if (res.code != 0) {
                return res.code;
            }
        }
        std::cout << "#" << n << ": " << (res.photoReused ? "photo kept" : "loaded " + std::to_string((int)res.load) + " ms")
                  << ", raw steps " << (res.rawReused ? "reused" : std::to_string((int)res.raw) + " ms")
                  << (res.earlyReused ? ", small picture reused" : "")
                  << (res.lateReused ? ", after stage " + std::to_string(res.lateReused) + " reused" : "")
                  << ", developed " << (int)res.develop << " ms, saved " << (int)res.save << " ms" << std::endl;
    }
    std::cout << "total " << (int)ms(t0, Clock::now()) << " ms" << std::endl;
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
#ifndef ART_WIN32_UCRT
    setlocale(LC_ALL, "");
#endif
    setlocale(LC_NUMERIC, "C");
    // 0.4: ART's own per-step timing on (read before anything is developed; two clock readings a step)
#ifdef _WIN32
    _putenv_s("ART_PROFILE", "1");
#else
    setenv("ART_PROFILE", "1", 1);
#endif
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    Gio::init();

#ifdef BUILD_BUNDLE
    Glib::ustring exePath = getExecutablePath(argv[0]);
    if (Glib::path_is_absolute(DATA_SEARCH_PATH)) {
        options.ART_base_dir = DATA_SEARCH_PATH;
    } else if (strcmp(DATA_SEARCH_PATH, ".") == 0) {
        options.ART_base_dir = exePath;
    } else {
        options.ART_base_dir = Glib::build_filename(exePath, DATA_SEARCH_PATH);
    }
    if (Glib::path_is_absolute(CREDITS_SEARCH_PATH)) {
        creditsPath = CREDITS_SEARCH_PATH;
    } else {
        creditsPath = Glib::build_filename(exePath, CREDITS_SEARCH_PATH);
    }
    if (Glib::path_is_absolute(LICENCE_SEARCH_PATH)) {
        licensePath = LICENCE_SEARCH_PATH;
    } else {
        licensePath = Glib::build_filename(exePath, LICENCE_SEARCH_PATH);
    }
    options.rtSettings.lensfunDbDirectory = LENSFUN_DB_PATH;
#else
    options.ART_base_dir = DATA_SEARCH_PATH;
    creditsPath = CREDITS_SEARCH_PATH;
    licensePath = LICENCE_SEARCH_PATH;
    options.rtSettings.lensfunDbDirectory = LENSFUN_DB_PATH;
#endif

    if (argc > 1 && !strcmp(argv[1], "--version")) {
        std::cout << "StopBath engine helper " << HelperVersion << " (ART " << RTVERSION << ")" << std::endl;
        return 0;
    }

    auto started = Clock::now();
    try {
        Options::load(false, 0);   // (as ART-cli: everything loaded)
    } catch (Options::Error &e) {
        std::cerr << "error: " << e.get_msg() << std::endl;
        return 1;
    }
    // as ART-cli: missing default profiles replaced
    if (options.is_defProfRawMissing()) {
        options.defProfRaw = Options::DEFPROFILE_RAW;
    }
    if (options.is_bundledDefProfRawMissing()) {
        options.defProfRaw = Options::DEFPROFILE_INTERNAL;
    }
    if (options.is_defProfImgMissing()) {
        options.defProfImg = Options::DEFPROFILE_IMG;
    }
    if (options.is_bundledDefProfImgMissing()) {
        options.defProfImg = Options::DEFPROFILE_INTERNAL;
    }
    TIFFSetWarningHandler(nullptr);

    if (argc > 1 && !strcmp(argv[1], "--serve")) {
        size_t keep = 2;
        for (int i = 2; i + 1 < argc; ++i) {
            if (!strcmp(argv[i], "--keep")) {
                keep = std::max(1, atoi(argv[i + 1]));
            } else if (!strcmp(argv[i], "--memory")) {   // 0.7
                auto &m = art::engine::sb::memory();
                if (!strcmp(argv[i + 1], "small")) {
                    m.mids = 2;
                    m.lates = 0;
                } else if (!strcmp(argv[i + 1], "large")) {
                    m.mids = 4;
                    m.lates = 4;
                    m.lateMaxPixels = 30000000;
                }
            }
        }
        return serve(keep);
    }

    std::cout << "StopBath engine helper " << HelperVersion << " (ART " << RTVERSION << "), started "
              << ms(started, Clock::now()) << " ms" << std::endl;
    if (argc > 1 && !strcmp(argv[1], "--render")) {
        return render(argc, argv);
    }
    std::cout << "usage: stopbath-engine --version | --serve [--keep <n>] [--memory small|normal|large] | --render <photo> <out.jpg> [--profile "
                 "<file.arp>]... [--fast] [--repeat <n>]"
              << std::endl;
    return 0;
}
