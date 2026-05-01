// Standalone offline test harness for the FM post-demod LPF.
//
// Reads a Rohde & Schwarz .iq.tar archive directly (mmap'd, same parser as
// inspectrum), applies the same NCO + Kaiser-FIR tuner + freqdem chain that
// the GUI uses, then runs a series of post-demod LPF candidates over the
// freqdem output. Each candidate is timed and its samples saved as raw
// float32 so we can plot them with the bundled Python script
// (tools/fm_filter_plot.py) and compare without bouncing through the GUI.
//
// Build:
//   cmake --build build --target fm_filter_compare
// Run:
//   ./build/tools/fm_filter_compare <iq.tar> <center_hz> <bw_hz> <out_dir>
//
// All intermediate float32 files are written to <out_dir> with names that
// describe the filter (raw, butter6, ellip8, kaiser_fir, …). The first 1M
// samples are processed by default — enough to evaluate any audio-rate
// modulating signal at 10 MHz Fs.

#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <liquid/liquid.h>

namespace {

struct TarEntry {
    std::string name;
    size_t off;
    size_t sz;
};

static std::vector<TarEntry> parseTar(const uint8_t *data, size_t total)
{
    std::vector<TarEntry> entries;
    size_t pos = 0;
    while (pos + 512 <= total) {
        const char *hdr = reinterpret_cast<const char*>(data + pos);
        bool allZero = true;
        for (int i = 0; i < 512; ++i) { if (hdr[i]) { allZero = false; break; } }
        if (allZero) break;
        const char *nul = static_cast<const char*>(memchr(hdr, 0, 100));
        size_t nameLen = nul ? static_cast<size_t>(nul - hdr) : 100;
        std::string name(hdr, nameLen);
        char szBuf[13];
        memcpy(szBuf, hdr + 124, 12);
        szBuf[12] = 0;
        size_t sz = strtoull(szBuf, nullptr, 8);
        char typeflag = hdr[156];
        size_t dataStart = pos + 512;
        if (typeflag == '0' || typeflag == '\0') {
            entries.push_back({name, dataStart, sz});
        }
        pos = dataStart + ((sz + 511) / 512) * 512;
    }
    return entries;
}

static std::string xmlTag(const std::string &xml, const std::string &tag)
{
    size_t s = xml.find("<" + tag);
    if (s == std::string::npos) return "";
    s = xml.find('>', s) + 1;
    size_t e = xml.find("</" + tag, s);
    if (e == std::string::npos) return "";
    return xml.substr(s, e - s);
}

static void saveF32(const std::string &path, const std::vector<float> &v)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(v.data()),
            static_cast<std::streamsize>(v.size() * sizeof(float)));
    fprintf(stderr, "  wrote %s (%zu samples, %.1f MB)\n",
            path.c_str(), v.size(), v.size() * sizeof(float) / (1024.0 * 1024.0));
}

template <typename Apply>
static void runIirCandidate(const std::string &label,
                            const std::vector<float> &demod,
                            const std::string &outDir,
                            iirfilt_rrrf filt,
                            Apply &&)
{
    std::vector<float> y(demod.size());
    iirfilt_rrrf_reset(filt);
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < demod.size(); ++i) {
        iirfilt_rrrf_execute(filt, demod[i], &y[i]);
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    fprintf(stderr, "  %-22s %7.1f ms\n", label.c_str(), ms);
    saveF32(outDir + "/" + label + ".f32", y);
    iirfilt_rrrf_destroy(filt);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s <iq.tar> <tuner_center_hz> <tuner_bw_hz> <out_dir> "
                "[lpf_cutoff_hz=5000] [n_samples=1000000]\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    const double centerHz = std::stod(argv[2]);
    const double bwHz = std::stod(argv[3]);
    const std::string outDir = argv[4];
    const double lpfHz = (argc >= 6) ? std::stod(argv[5]) : 5000.0;
    const size_t nWanted = (argc >= 7) ? std::stoull(argv[6]) : 1'000'000ULL;

    int fd = ::open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st{};
    if (::fstat(fd, &st) != 0) { perror("fstat"); return 1; }
    void *mm = ::mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mm == MAP_FAILED) { perror("mmap"); return 1; }
    const uint8_t *base = static_cast<const uint8_t*>(mm);

    auto entries = parseTar(base, st.st_size);
    const TarEntry *xmlE = nullptr, *dataE = nullptr;
    for (const auto &e : entries) {
        if (e.name.size() >= 4 && e.name.compare(e.name.size() - 4, 4, ".xml") == 0) xmlE = &e;
    }
    if (!xmlE) { fprintf(stderr, "iq.tar: no xml found\n"); return 1; }

    std::string xml(reinterpret_cast<const char*>(base + xmlE->off), xmlE->sz);
    const std::string clockStr = xmlTag(xml, "Clock");
    const std::string dataFn = xmlTag(xml, "DataFilename");
    const std::string fmtStr = xmlTag(xml, "Format");
    const std::string dtypeStr = xmlTag(xml, "DataType");
    const double Fs = std::stod(clockStr);
    fprintf(stderr, "Fs=%.0f Hz, data=%s, fmt=%s/%s\n",
            Fs, dataFn.c_str(), fmtStr.c_str(), dtypeStr.c_str());

    for (const auto &e : entries) if (e.name == dataFn) dataE = &e;
    if (!dataE) { fprintf(stderr, "iq.tar: data file '%s' missing\n", dataFn.c_str()); return 1; }
    if (fmtStr != "complex" || dtypeStr != "float32") {
        fprintf(stderr, "Only complex/float32 supported in this tool\n"); return 1;
    }

    const auto *iq = reinterpret_cast<const std::complex<float>*>(base + dataE->off);
    const size_t nTotal = dataE->sz / sizeof(std::complex<float>);
    const size_t N = std::min(nWanted, nTotal);
    fprintf(stderr, "processing N=%zu of %zu samples (%.3f s)\n",
            N, nTotal, static_cast<double>(N) / Fs);

    // Tuner: NCO mix down by centerHz, then Kaiser FIR LPF at bwHz/2.
    nco_crcf nco = nco_crcf_create(LIQUID_NCO);
    nco_crcf_set_frequency(nco, 2.0f * static_cast<float>(M_PI) *
                                  static_cast<float>(centerHz / Fs));
    std::vector<std::complex<float>> mixed(N);
    nco_crcf_mix_block_down(nco, const_cast<std::complex<float>*>(iq),
                            mixed.data(), N);
    nco_crcf_destroy(nco);

    const float tunerCutoff = static_cast<float>((bwHz / 2) / Fs);
    const float atten = 60.0f;
    unsigned int tunerLen = estimate_req_filter_len(std::min(tunerCutoff, 0.05f), atten);
    if (tunerLen < 3) tunerLen = 3;
    std::vector<float> tunerTaps(tunerLen);
    liquid_firdes_kaiser(tunerLen, tunerCutoff, atten, 0.0f, tunerTaps.data());

    firfilt_crcf tunerFilt = firfilt_crcf_create(tunerTaps.data(), tunerLen);
    std::vector<std::complex<float>> tuned(N);
    for (size_t i = 0; i < N; ++i) {
        firfilt_crcf_push(tunerFilt, mixed[i]);
        firfilt_crcf_execute(tunerFilt, &tuned[i]);
    }
    firfilt_crcf_destroy(tunerFilt);

    // freqdem: kf matches tuner relative bandwidth.
    const float relBw = static_cast<float>(bwHz / Fs);
    freqdem fdem = freqdem_create(relBw / 2.0f);
    std::vector<float> demod(N);
    for (size_t i = 0; i < N; ++i) {
        freqdem_demodulate(fdem, *reinterpret_cast<liquid_float_complex*>(&tuned[i]),
                           &demod[i]);
    }
    freqdem_destroy(fdem);

    fprintf(stderr, "Saving raw demod and filter candidates @ Fc=%.0f Hz:\n", lpfHz);
    saveF32(outDir + "/raw_demod.f32", demod);

    const float fcNorm = static_cast<float>(lpfHz / Fs);

    // 1) Butterworth IIR (cascaded SOS) — the previous default.
    runIirCandidate("butter_o6_iir", demod, outDir,
        iirfilt_rrrf_create_prototype(LIQUID_IIRDES_BUTTER, LIQUID_IIRDES_LOWPASS,
                                      LIQUID_IIRDES_SOS, 6, fcNorm, 0.0f, 0.1f, 60.0f),
        nullptr);
    runIirCandidate("butter_o10_iir", demod, outDir,
        iirfilt_rrrf_create_prototype(LIQUID_IIRDES_BUTTER, LIQUID_IIRDES_LOWPASS,
                                      LIQUID_IIRDES_SOS, 10, fcNorm, 0.0f, 0.1f, 60.0f),
        nullptr);

    // 2) Chebyshev type 2 — flat passband, ripple in stopband (better
    //    for visualization than type 1 which ripples the passband).
    runIirCandidate("cheby2_o8_iir", demod, outDir,
        iirfilt_rrrf_create_prototype(LIQUID_IIRDES_CHEBY2, LIQUID_IIRDES_LOWPASS,
                                      LIQUID_IIRDES_SOS, 8, fcNorm, 0.0f, 0.1f, 60.0f),
        nullptr);

    // 3) Elliptic — sharpest transition for a given order; current
    //    inspectrum default at order 8.
    runIirCandidate("ellip_o8_iir", demod, outDir,
        iirfilt_rrrf_create_prototype(LIQUID_IIRDES_ELLIP, LIQUID_IIRDES_LOWPASS,
                                      LIQUID_IIRDES_SOS, 8, fcNorm, 0.0f, 0.1f, 60.0f),
        nullptr);
    runIirCandidate("ellip_o12_iir", demod, outDir,
        iirfilt_rrrf_create_prototype(LIQUID_IIRDES_ELLIP, LIQUID_IIRDES_LOWPASS,
                                      LIQUID_IIRDES_SOS, 12, fcNorm, 0.0f, 0.1f, 60.0f),
        nullptr);

    // Helper to run an FIR over the demod buffer and time it.
    auto runFir = [&](const std::string &label, unsigned int len) {
        if (len < 3) len = 3;
        std::vector<float> taps(len);
        liquid_firdes_kaiser(len, fcNorm, 60.0f, 0.0f, taps.data());
        // Normalize to unity DC gain.
        double dc = 0.0;
        for (auto t : taps) dc += t;
        if (dc != 0.0) for (auto &t : taps) t = static_cast<float>(t / dc);
        firfilt_rrrf fir = firfilt_rrrf_create(taps.data(), len);
        std::vector<float> y(N);
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < N; ++i) {
            firfilt_rrrf_push(fir, demod[i]);
            firfilt_rrrf_execute(fir, &y[i]);
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        firfilt_rrrf_destroy(fir);
        fprintf(stderr, "  %-22s %7.1f ms (taps=%u, dc_norm)\n", label.c_str(), ms, len);
        saveF32(outDir + "/" + label + ".f32", y);
    };

    // 4) Kaiser FIR — full reference (linear-phase, expensive).
    {
        unsigned int firLen = estimate_req_filter_len(std::max(fcNorm, 1e-4f), 60.0f);
        runFir("kaiser_fir", firLen);
    }
    // 5) Capped Kaiser FIRs — linear phase but cost bounded, useful for narrow Fc.
    runFir("kaiser_fir_1024", 1024);
    runFir("kaiser_fir_4096", 4096);

    // 6) Filtfilt-style zero-phase via forward+reversed IIR. Effective order
    //    doubles, group delay cancels, magnitude response squared. Cheaper
    //    than a comparable-length FIR and keeps linear-phase look.
    {
        iirfilt_rrrf filt = iirfilt_rrrf_create_prototype(
            LIQUID_IIRDES_BUTTER, LIQUID_IIRDES_LOWPASS, LIQUID_IIRDES_SOS,
            6, fcNorm, 0.0f, 0.1f, 60.0f);
        std::vector<float> y(N);
        auto t0 = std::chrono::steady_clock::now();
        // Forward
        iirfilt_rrrf_reset(filt);
        for (size_t i = 0; i < N; ++i) iirfilt_rrrf_execute(filt, demod[i], &y[i]);
        // Reverse pass
        iirfilt_rrrf_reset(filt);
        std::vector<float> z(N);
        for (size_t i = 0; i < N; ++i) {
            float in = y[N - 1 - i];
            iirfilt_rrrf_execute(filt, in, &z[N - 1 - i]);
        }
        auto t1 = std::chrono::steady_clock::now();
        iirfilt_rrrf_destroy(filt);
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        fprintf(stderr, "  %-22s %7.1f ms (zero-phase)\n", "butter_o6_filtfilt", ms);
        saveF32(outDir + "/butter_o6_filtfilt.f32", z);
    }

    fprintf(stderr, "done. Plot with:\n"
            "  python3 tools/fm_filter_plot.py %s/*.f32\n", outDir.c_str());

    ::munmap(mm, st.st_size);
    ::close(fd);
    return 0;
}
