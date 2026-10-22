/*
 *  StopBath engine helper - step 1.
 *
 *  A small program built from ART's own processing engine (ART by Alberto Griggio, a fork of RawTherapee), for
 *  StopBath. This first step only proves that ART's engine builds and starts here, and times one photo:
 *
 *    stopbath-engine --version
 *    stopbath-engine --render <photo> <out.jpg> [--profile <file.arp>]... [--fast] [--repeat <n>]
 *
 *  --repeat develops the same photo n times from one loading (how much a helper that stays open saves on loading).
 *  Later steps keep the photo and the unchanged steps in memory and take changes from StopBath through a pipe.
 *
 *  Copyright (c) 2026 the StopBath engine helper's authors. Based on ART's main-cli.cc.
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version (see LICENSE).
 */

#include "../engine/imgiomanager.h"
#include "../engine/profilestore.h"
#include "../engine/settings.h"
#include "../engine/procparams.h"
#include "../engine/rtengine.h"
#include "../utils/config.h"
#include "../utils/pathutils.h"
#include "../utils/version.h"
#include "fastexport.h"
#include "options.h"
#include <chrono>
#include <cstring>
#include <giomm.h>
#include <iostream>
#include <locale.h>
#include <tiffio.h>
#include <vector>

namespace art { namespace gui {
using namespace utils;
extern Options options;
// (ART's other command-line sources expect these)
Glib::ustring creditsPath;
Glib::ustring licensePath;
Glib::ustring argv1;
bool progress = false;
} } // namespace art::gui

using namespace art::gui;
using Clock = std::chrono::steady_clock;

namespace {

const char *HelperVersion = "0.1";

double ms(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

Glib::ustring extensionOf(const Glib::ustring &f)
{
    auto dot = f.find_last_of('.');
    return dot == Glib::ustring::npos ? "" : f.substr(dot + 1).lowercase();
}

bool isPicture(const Glib::ustring &f)
{
    auto ext = extensionOf(f);
    return ext == "jpg" || ext == "jpeg" || ext == "tif" || ext == "tiff" || ext == "png" ||
           art::engine::ImageIOManager::getInstance()->canLoad(ext);
}

int render(int argc, char **argv)
{
    if (argc < 4) {
        std::cerr << "usage: stopbath-engine --render <photo> <out.jpg> [--profile <file.arp>]... [--fast] [--repeat <n>]" << std::endl;
        return 2;
    }
    Glib::ustring photo = argv[2], out = argv[3];
    std::vector<Glib::ustring> profiles;
    bool fast = false;
    int repeat = 1;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--profile") && i + 1 < argc) profiles.push_back(argv[++i]);
        else if (!strcmp(argv[i], "--fast")) fast = true;
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = std::max(1, atoi(argv[++i]));
    }

    auto t0 = Clock::now();
    bool raw = !isPicture(photo);
    int error = 0;
    art::engine::InitialImage *ii = art::engine::InitialImage::load(photo, raw, &error, nullptr);
    if (!ii) {
        std::cerr << "error: the photo couldn't be loaded (" << error << ")" << std::endl;
        return 3;
    }
    auto t1 = Clock::now();
    std::cout << "loaded " << ms(t0, t1) << " ms" << std::endl;

    art::engine::procparams::ProcParams params;
    for (auto &p : profiles) {
        if (params.load(nullptr, p) != 0) {
            std::cerr << "error: the profile couldn't be read: " << p << std::endl;
            ii->decreaseRef();
            return 4;
        }
    }
    if (auto sp = art::engine::ImageIOManager::getInstance()->getSaveProfile("jpg")) sp->applyTo(params);

    for (int n = 1; n <= repeat; ++n) {
        auto a = Clock::now();
        auto job = create_processing_job(ii, params, fast);
        if (!job) {
            std::cerr << "error: no processing job" << std::endl;
            ii->decreaseRef();
            return 5;
        }
        art::engine::IImagefloat *img = art::engine::processImage(job, error, nullptr);
        if (!img) {
            std::cerr << "error: the photo couldn't be developed (" << error << ")" << std::endl;
            ii->decreaseRef();
            return 6;
        }
        auto b = Clock::now();
        int saved = n == repeat ? img->saveAsJPEG(out, 92, 3) : 0;
        auto c = Clock::now();
        std::cout << "developed #" << n << " " << ms(a, b) << " ms" << (n == repeat ? ", saved " + std::to_string((int)ms(b, c)) + " ms" : "") << std::endl;
        img->free();
        if (saved != 0) {
            std::cerr << "error: the picture couldn't be saved" << std::endl;
            ii->decreaseRef();
            return 7;
        }
    }
    ii->decreaseRef();
    std::cout << "total " << ms(t0, Clock::now()) << " ms" << std::endl;
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
#ifndef ART_WIN32_UCRT
    setlocale(LC_ALL, "");
#endif
    setlocale(LC_NUMERIC, "C");

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
        Options::load(true, 0);
    } catch (Options::Error &e) {
        std::cerr << "error: " << e.get_msg() << std::endl;
        return 1;
    }
    TIFFSetWarningHandler(nullptr);
    std::cout << "StopBath engine helper " << HelperVersion << " (ART " << RTVERSION << "), started " << ms(started, Clock::now()) << " ms" << std::endl;

    if (argc > 1 && !strcmp(argv[1], "--render")) return render(argc, argv);
    std::cout << "usage: stopbath-engine --version | --render <photo> <out.jpg> [--profile <file.arp>]... [--fast] [--repeat <n>]" << std::endl;
    return 0;
}
