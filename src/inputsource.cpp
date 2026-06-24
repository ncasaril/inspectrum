/*
 *  Copyright (C) 2015, Mike Walters <mike@flomp.net>
 *  Copyright (C) 2015, Jared Boone <jared@sharebrained.com>
 *
 *  This file is part of inspectrum.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "inputsource.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <stdexcept>
#include <algorithm>
#include <vector>

#include <QFileInfo>

#include <QElapsedTimer>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmapCache>
#include <QRect>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QColor>
#include <QXmlStreamReader>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QCryptographicHash>

#ifdef HAVE_ZSTD
#include <zstd.h>
#endif


class ComplexF32SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(std::complex<float>);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const std::complex<float>*>(src);
        std::copy(&s[start], &s[start + length], dest);
    }
};

class ComplexF64SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(std::complex<double>);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const std::complex<double>*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const std::complex<double>& v) -> std::complex<float> {
                return { static_cast<float>(v.real()) , static_cast<float>(v.imag()) };
            }
        );
    }
};

class ComplexS32SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(std::complex<int32_t>);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const std::complex<int32_t>*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const std::complex<int32_t>& v) -> std::complex<float> {
                const float k = 1.0f / 2147483648.0f;
                return { v.real() * k, v.imag() * k };
            }
        );
    }
};

class ComplexS16SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(std::complex<int16_t>);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const std::complex<int16_t>*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const std::complex<int16_t>& v) -> std::complex<float> {
                const float k = 1.0f / 32768.0f;
                return { v.real() * k, v.imag() * k };
            }
        );
    }
};

class ComplexS8SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(std::complex<int8_t>);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const std::complex<int8_t>*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const std::complex<int8_t>& v) -> std::complex<float> {
                const float k = 1.0f / 128.0f;
                return { v.real() * k, v.imag() * k };
            }
        );
    }
};

class ComplexU8SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(std::complex<uint8_t>);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const std::complex<uint8_t>*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const std::complex<uint8_t>& v) -> std::complex<float> {
                const float k = 1.0f / 128.0f;
                return { (v.real() - 127.4f) * k, (v.imag() - 127.4f) * k };
            }
        );
    }
};

class RealF32SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(float);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const float*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const float& v) -> std::complex<float> {
                return {v, 0.0f};
            }
        );
    }
};

class RealF64SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(double);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const double*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const double& v) -> std::complex<float> {
                return {static_cast<float>(v), 0.0f};
            }
        );
    }
};

class RealS16SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(int16_t);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const int16_t*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const int16_t& v) -> std::complex<float> {
                const float k = 1.0f / 32768.0f;
                return { v * k, 0.0f };
            }
        );
    }
};

class RealS8SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(int8_t);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const int8_t*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const int8_t& v) -> std::complex<float> {
                const float k = 1.0f / 128.0f;
                return { v * k, 0.0f };
            }
        );
    }
};

class RealU8SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(uint8_t);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const uint8_t*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const uint8_t& v) -> std::complex<float> {
                const float k = 1.0f / 128.0f;
                return { (v - 127.4f) * k, 0 };
            }
        );
    }
};

namespace {

// Rohde & Schwarz .iq.tar support: a USTAR archive that bundles a raw IQ
// data file and an XML manifest. The manifest describes the format, sample
// rate, center frequency, etc. We parse the tar in-place (it is already
// mmap'd) so the sample-path reads can proceed at the data file's byte
// offset with no extra copy.

struct TarEntry {
    QString name;
    qint64 dataOffset;
    qint64 size;
};

static qint64 tarOctal(const char *buf, size_t len)
{
    // Tar numeric fields are zero- or space-padded octal ASCII, usually
    // null- or space-terminated. strtoll would choke on trailing junk, so
    // copy into a local null-terminated buffer and parse there.
    char tmp[32];
    size_t n = std::min(len, sizeof(tmp) - 1);
    memcpy(tmp, buf, n);
    tmp[n] = 0;
    return strtoll(tmp, nullptr, 8);
}

static std::vector<TarEntry> parseTar(const uchar *data, qint64 totalSize)
{
    std::vector<TarEntry> entries;
    qint64 pos = 0;
    while (pos + 512 <= totalSize) {
        const char *hdr = reinterpret_cast<const char*>(data + pos);
        bool allZero = true;
        for (int i = 0; i < 512; ++i) { if (hdr[i]) { allZero = false; break; } }
        if (allZero) break;

        // Name: 100 bytes, null-terminated; use memchr to stay within the field
        const char *nul = static_cast<const char*>(memchr(hdr, 0, 100));
        int nameLen = nul ? static_cast<int>(nul - hdr) : 100;
        QString name = QString::fromUtf8(hdr, nameLen);

        qint64 sz = tarOctal(hdr + 124, 12);
        char typeflag = hdr[156];

        qint64 dataStart = pos + 512;
        // Bound the payload against the actual mapping. A truncated or hostile
        // tar can carry a size field that runs past the mmap, and the
        // downstream QByteArray / copyRange reads would then walk off the
        // mapping (SIGSEGV/SIGBUS). dataStart <= totalSize already (loop
        // guard), so this is the overflow-safe form. Stop parsing on a bad
        // size — once the framing is wrong the rest of the stream can't be
        // trusted anyway.
        if (sz < 0 || sz > totalSize - dataStart)
            break;
        // Only regular files ('0' in ustar, '\0' in very old tar) carry data
        // we care about; skip directories, links, etc.
        if (typeflag == '0' || typeflag == '\0') {
            entries.push_back({name, dataStart, sz});
        }
        pos = dataStart + ((sz + 511) / 512) * 512;
    }
    return entries;
}

// Transparent .zst support: stream-decompress a zstd file to disk. We can't
// mmap compressed bytes (the whole sample pipeline reads via mmap), so the
// pragmatic approach is to inflate to a cache file once and mmap that. The
// decompressed output is whatever was compressed — typically a SigMF archive
// (.sigmf tarball), an iq.tar, or a plain raw IQ file — and openFile is then
// re-entered on it by suffix.

#ifdef HAVE_ZSTD
static void zstdDecompressToFile(const QString &src, const QString &dst)
{
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly))
        throw std::runtime_error(("zstd: cannot open " + src + ": " + in.errorString()).toStdString());
    QFile out(dst);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        throw std::runtime_error(("zstd: cannot write " + dst + ": " + out.errorString()).toStdString());

    const size_t inCap = ZSTD_DStreamInSize();
    const size_t outCap = ZSTD_DStreamOutSize();
    std::vector<char> inBuf(inCap), outBuf(outCap);

    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (dctx == nullptr)
        throw std::runtime_error("zstd: failed to create decompression context");

    size_t lastRet = 0;
    qint64 n;
    try {
        while ((n = in.read(inBuf.data(), inCap)) > 0) {
            ZSTD_inBuffer input = { inBuf.data(), static_cast<size_t>(n), 0 };
            while (input.pos < input.size) {
                ZSTD_outBuffer output = { outBuf.data(), outCap, 0 };
                size_t ret = ZSTD_decompressStream(dctx, &output, &input);
                if (ZSTD_isError(ret))
                    throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(ret));
                if (out.write(outBuf.data(), output.pos) != static_cast<qint64>(output.pos))
                    throw std::runtime_error(("zstd: write failed: " + out.errorString()).toStdString());
                lastRet = ret;
            }
        }
        if (n < 0)
            throw std::runtime_error(("zstd: read failed: " + in.errorString()).toStdString());
        // A non-zero final return means the last frame ended mid-block, i.e. the
        // input was truncated. Zero means we landed on a clean frame boundary.
        if (lastRet != 0)
            throw std::runtime_error("zstd: truncated or incomplete input stream");
    } catch (...) {
        ZSTD_freeDCtx(dctx);
        out.remove();
        throw;
    }
    ZSTD_freeDCtx(dctx);
    if (!out.flush())
        throw std::runtime_error(("zstd: flush failed: " + out.errorString()).toStdString());
}
#endif

// Decompress `zstPath` into a per-source cache directory and return the path of
// the inflated file. The cache key folds in size + mtime so a changed source
// re-inflates, and a complete previous inflation is reused as-is.
static QString decompressZstToCache(const QFileInfo &zstInfo)
{
#ifndef HAVE_ZSTD
    (void)zstInfo;
    throw std::runtime_error(
        "This build has no zstd support; cannot open .zst input. "
        "Install libzstd-dev (or libzstd) and rebuild.");
#else
    // Strip the .zst suffix to recover the inner filename (and thus its real
    // suffix, which openFile dispatches on): foo.sigmf.zst -> foo.sigmf.
    QString innerName = zstInfo.fileName();
    innerName.chop(QString(".").length() + QString(zstInfo.suffix()).length()); // drop ".zst"

    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty())
        base = QDir::tempPath();

    QByteArray pathKey = zstInfo.absoluteFilePath().toUtf8();
    QString digest = QString::fromLatin1(
        QCryptographicHash::hash(pathKey, QCryptographicHash::Sha1).toHex().left(16));
    QString key = QString("%1-%2-%3")
                      .arg(digest)
                      .arg(zstInfo.size())
                      .arg(zstInfo.lastModified().toMSecsSinceEpoch());

    QDir cacheDir(base + "/zst-cache/" + key);
    if (!cacheDir.exists() && !cacheDir.mkpath("."))
        throw std::runtime_error(("zstd: cannot create cache dir " + cacheDir.path()).toStdString());

    QString outPath = cacheDir.filePath(innerName);
    QString donePath = outPath + ".done";

    // Reuse a prior complete inflation. The .done marker guards against a
    // half-written output left behind by an interrupted run.
    if (QFileInfo::exists(outPath) && QFileInfo::exists(donePath))
        return outPath;

    zstdDecompressToFile(zstInfo.absoluteFilePath(), outPath);

    QFile marker(donePath);
    if (marker.open(QIODevice::WriteOnly))
        marker.close();

    return outPath;
#endif
}

} // namespace

InputSource::InputSource()
{
    frequency = 0.0;
}

InputSource::~InputSource()
{
    cleanup();
}

void InputSource::cleanup()
{
    if (mmapData != nullptr) {
        inputFile->unmap(mmapData);
        mmapData = nullptr;
    }

    if (inputFile != nullptr) {
        delete inputFile;
        inputFile = nullptr;
    }
}

QJsonObject InputSource::readMetaData(const QString &filename)
{
    QFile datafile(filename);
    if (!datafile.open(QFile::ReadOnly | QIODevice::Text)) {
        throw std::runtime_error("Error while opening meta data file: " + datafile.errorString().toStdString());
    }

    QByteArray bytes = datafile.readAll();
    datafile.close();
    return parseMetaDocument(bytes);
}

QJsonObject InputSource::parseMetaDocument(const QByteArray &bytes)
{
    QJsonDocument d = QJsonDocument::fromJson(bytes);
    auto root = d.object();

    if (!root.contains("global") || !root["global"].isObject()) {
        throw std::runtime_error("SigMF meta data is invalid (no global object found)");
    }

    auto global = root["global"].toObject();

    if (!global.contains("core:datatype") || !global["core:datatype"].isString()) {
        throw std::runtime_error("SigMF meta data does not specify a valid datatype");
    }


    auto datatype = global["core:datatype"].toString();
    if (datatype.compare("cf32_le") == 0) {
        sampleAdapter = std::make_unique<ComplexF32SampleAdapter>();
    } else if (datatype.compare("ci32_le") == 0) {
        sampleAdapter = std::make_unique<ComplexS32SampleAdapter>();
    } else if (datatype.compare("ci16_le") == 0) {
        sampleAdapter = std::make_unique<ComplexS16SampleAdapter>();
    } else if (datatype.compare("ci8") == 0) {
        sampleAdapter = std::make_unique<ComplexS8SampleAdapter>();
    } else if (datatype.compare("cu8") == 0) {
        sampleAdapter = std::make_unique<ComplexU8SampleAdapter>();
    } else if (datatype.compare("rf32_le") == 0) {
        sampleAdapter = std::make_unique<RealF32SampleAdapter>();
        _realSignal = true;
    } else if (datatype.compare("ri16_le") == 0) {
        sampleAdapter = std::make_unique<RealS16SampleAdapter>();
        _realSignal = true;
    } else if (datatype.compare("ri8") == 0) {
        sampleAdapter = std::make_unique<RealS8SampleAdapter>();
        _realSignal = true;
    } else if (datatype.compare("ru8") == 0) {
        sampleAdapter = std::make_unique<RealU8SampleAdapter>();
        _realSignal = true;
    } else {
        throw std::runtime_error("SigMF meta data specifies unsupported datatype");
    }
    _datatype = datatype;

    if (global.contains("core:sample_rate") && global["core:sample_rate"].isDouble()) {
        setSampleRate(global["core:sample_rate"].toDouble());
    }


    if (root.contains("captures") && root["captures"].isArray()) {
        auto captures = root["captures"].toArray();

        for (auto capture_ref : captures) {
            if (capture_ref.isObject()) {
                auto capture = capture_ref.toObject();
                if (capture.contains("core:frequency") && capture["core:frequency"].isDouble()) {
                    frequency = capture["core:frequency"].toDouble();
                }
            } else {
                throw std::runtime_error("SigMF meta data is invalid (invalid capture object)");
            }
        }
    }

    if(root.contains("annotations") && root["annotations"].isArray()) {

        size_t offset = 0;

        if (global.contains("core:offset")) {
            offset = global["offset"].toDouble();
        }

        auto annotations = root["annotations"].toArray();

        for (auto annotation_ref : annotations) {
            if (annotation_ref.isObject()) {
                auto sigmf_annotation = annotation_ref.toObject();

                const size_t sample_start = sigmf_annotation["core:sample_start"].toDouble();

                if (sample_start < offset)
                    continue;

                const size_t rel_sample_start = sample_start - offset;

                const size_t sample_count = sigmf_annotation["core:sample_count"].toDouble();
                auto sampleRange = range_t<size_t>{rel_sample_start, rel_sample_start + sample_count - 1};

                const double freq_lower_edge = sigmf_annotation["core:freq_lower_edge"].toDouble();
                const double freq_upper_edge = sigmf_annotation["core:freq_upper_edge"].toDouble();
                auto frequencyRange = range_t<double>{freq_lower_edge, freq_upper_edge};

                auto label = sigmf_annotation["core:label"].toString();
                auto description = sigmf_annotation["core:description"].toString();
                auto comment = sigmf_annotation["core:comment"].toString();

                auto sigmf_color = sigmf_annotation["presentation:color"].toString();
                // SigMF uses the format "#RRGGBBAA" for alpha-channel colors, QT uses "#AARRGGBB"
                if ((sigmf_color.at(0) == '#') && (sigmf_color.length()) == 9) {
                    sigmf_color = "#" + sigmf_color.mid(7,2) + sigmf_color.mid(1,6);
                }
                auto boxColor = QString::fromStdString("white");
                if (QColor::isValidColor(sigmf_color)) {
                    boxColor = sigmf_color;
                }

                annotationList.emplace_back(sampleRange, frequencyRange, label, description, comment, boxColor);
            }
        }
    }

    return root;
}

void InputSource::openIqTar(const uchar *data, qint64 size)
{
    auto entries = parseTar(data, size);

    const TarEntry *xmlEntry = nullptr;
    for (const auto &e : entries) {
        if (e.name.endsWith(".xml", Qt::CaseInsensitive)) { xmlEntry = &e; break; }
    }
    if (!xmlEntry)
        throw std::runtime_error("iq.tar: no XML manifest found in archive");

    QByteArray xmlBytes(reinterpret_cast<const char*>(data + xmlEntry->dataOffset),
                        static_cast<int>(xmlEntry->size));
    QXmlStreamReader xml(xmlBytes);

    QString dataFilename, format, dataType;
    int numChannels = 1;
    double clock = 0.0;
    double centerFreq = 0.0;
    qint64 xmlSamples = 0;

    while (!xml.atEnd() && !xml.hasError()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        auto name = xml.name().toString();
        if      (name == "DataFilename")     dataFilename = xml.readElementText();
        else if (name == "Clock")            clock = xml.readElementText().toDouble();
        else if (name == "Samples")          xmlSamples = xml.readElementText().toLongLong();
        else if (name == "Format")           format = xml.readElementText();
        else if (name == "DataType")         dataType = xml.readElementText();
        else if (name == "NumberOfChannels") numChannels = xml.readElementText().toInt();
        else if (name == "CenterFrequency") {
            bool ok;
            double cf = xml.readElementText().toDouble(&ok);
            if (ok) centerFreq = cf;
        }
    }
    if (xml.hasError())
        throw std::runtime_error(("iq.tar: XML parse error: " + xml.errorString()).toStdString());
    if (dataFilename.isEmpty())
        throw std::runtime_error("iq.tar: XML manifest is missing <DataFilename>");
    if (numChannels != 1)
        throw std::runtime_error("iq.tar: multi-channel archives are not supported");

    const TarEntry *dataEntry = nullptr;
    for (const auto &e : entries) {
        if (e.name == dataFilename) { dataEntry = &e; break; }
    }
    if (!dataEntry)
        throw std::runtime_error(("iq.tar: data file '" + dataFilename +
                                  "' referenced by manifest not found in archive").toStdString());

    const QString fmtL = format.toLower();
    const QString dtL  = dataType.toLower();
    _realSignal = false;
    if (fmtL == "complex" && dtL == "float32") {
        sampleAdapter = std::make_unique<ComplexF32SampleAdapter>();
    } else if (fmtL == "complex" && dtL == "int32") {
        sampleAdapter = std::make_unique<ComplexS32SampleAdapter>();
    } else if (fmtL == "complex" && dtL == "int16") {
        sampleAdapter = std::make_unique<ComplexS16SampleAdapter>();
    } else if (fmtL == "real" && dtL == "float32") {
        sampleAdapter = std::make_unique<RealF32SampleAdapter>();
        _realSignal = true;
    } else if (fmtL == "real" && dtL == "int16") {
        sampleAdapter = std::make_unique<RealS16SampleAdapter>();
        _realSignal = true;
    } else {
        throw std::runtime_error(("iq.tar: unsupported Format/DataType combination: " +
                                  format + "/" + dataType).toStdString());
    }

    dataOffset = static_cast<size_t>(dataEntry->dataOffset);
    size_t derivedCount = dataEntry->size / sampleAdapter->sampleSize();
    // Trust the raw tar entry size over the XML Samples count: NumberOfPreSamples
    // and NumberOfPostSamples (which we don't separate out here) can make the
    // two disagree, and the entry size is what actually sits on disk.
    sampleCount = derivedCount > 0 ? derivedCount : static_cast<size_t>(xmlSamples);
    if (clock > 0.0) sampleRate = clock;
    frequency = centerFreq;
}

bool InputSource::openSigmfArchive(const uchar *data, qint64 size)
{
    auto entries = parseTar(data, size);

    const TarEntry *metaEntry = nullptr;
    const TarEntry *dataEntry = nullptr;
    for (const auto &e : entries) {
        if (e.name.endsWith(".sigmf-meta", Qt::CaseInsensitive)) metaEntry = &e;
        else if (e.name.endsWith(".sigmf-data", Qt::CaseInsensitive)) dataEntry = &e;
    }
    // Not a SigMF archive (no meta/data members). Report that rather than
    // throwing so the caller can fall back — e.g. a plain `.tar` then opens
    // as raw IQ instead of failing. A genuinely malformed `.sigmf` is the
    // caller's error to raise.
    if (!metaEntry || !dataEntry)
        return false;

    QByteArray metaBytes(reinterpret_cast<const char*>(data + metaEntry->dataOffset),
                         static_cast<int>(metaEntry->size));
    // parseMetaDocument picks the sample adapter and sets sampleRate /
    // frequency / annotations exactly as the .sigmf-* pair path does.
    parseMetaDocument(metaBytes);

    dataOffset = static_cast<size_t>(dataEntry->dataOffset);
    sampleCount = dataEntry->size / sampleAdapter->sampleSize();
    return true;
}

void InputSource::openFile(const char *filename)
{
    QFileInfo fileInfo(filename);
    _filePath = fileInfo.absoluteFilePath();
    _wasSigmfInput = false;
    _originalSigmfRoot = QJsonObject();
    _annotationsDirty = false;
    // Reset the center frequency for each open. Container metadata (SigMF,
    // iq.tar) raises it again below when present; filename-derived values
    // (gqrx/osmocom) are applied by MainWindow *after* openFile() returns.
    // Without this reset a baseband file opened after a frequency-tagged one
    // would inherit the previous file's center and mislabel the abs-freq axis.
    frequency = 0.0;
    std::string suffix = std::string(fileInfo.suffix().toLower().toUtf8().constData());
    if (_fmt != "") { suffix = _fmt; } // allow fmt override

    // Default to no container offset; openIqTar overrides this for archives.
    dataOffset = 0;

    // Transparent zstd input: inflate to a cache file, then re-enter openFile
    // on the decompressed result so its real suffix (.sigmf, .iq.tar, .cf32, …)
    // drives the rest of the dispatch. Skipped when an explicit fmt override is
    // set, since that names the raw layout of `filename` itself.
    if (_fmt.empty() && fileInfo.suffix().compare("zst", Qt::CaseInsensitive) == 0) {
        QString decompressed = decompressZstToCache(fileInfo);
        openFile(decompressed.toUtf8().constData());
        return;
    }

    // R&S iq.tar container — delegate to openIqTar which discovers format,
    // sample rate, center frequency, and the offset of the raw data file
    // within the mmap'd archive.
    if (_fmt.empty() && fileInfo.fileName().endsWith(".iq.tar", Qt::CaseInsensitive)) {
        auto file = std::make_unique<QFile>(filename);
        if (!file->open(QFile::ReadOnly)) {
            throw std::runtime_error(file->errorString().toStdString());
        }
        qint64 size = file->size();
        uchar *data = file->map(0, size);
        if (data == nullptr)
            throw std::runtime_error("Error mmapping iq.tar file");

        annotationList.clear();
        dataOffset = 0;
        try {
            openIqTar(data, size);
        } catch (...) {
            file->unmap(data);
            throw;
        }

        cleanup();
        inputFile = file.release();
        mmapData = data;
        invalidate();
        return;
    }

    if ((suffix == "cfile") || (suffix == "cf32")  || (suffix == "fc32")) {
        sampleAdapter = std::make_unique<ComplexF32SampleAdapter>();
        _datatype = "cf32_le";
    }
    else if ((suffix == "cf64")  || (suffix == "fc64")) {
        sampleAdapter = std::make_unique<ComplexF64SampleAdapter>();
        _datatype = "cf64_le";
    }
    else if ((suffix == "cs32") || (suffix == "sc32") || (suffix == "c32")) {
        sampleAdapter = std::make_unique<ComplexS32SampleAdapter>();
        _datatype = "ci32_le";
    }
    else if ((suffix == "cs16") || (suffix == "sc16") || (suffix == "c16")) {
        sampleAdapter = std::make_unique<ComplexS16SampleAdapter>();
        _datatype = "ci16_le";
    }
    else if ((suffix == "cs8") || (suffix == "sc8") || (suffix == "c8")) {
        sampleAdapter = std::make_unique<ComplexS8SampleAdapter>();
        _datatype = "ci8";
    }
    else if ((suffix == "cu8") || (suffix == "uc8")) {
        sampleAdapter = std::make_unique<ComplexU8SampleAdapter>();
        _datatype = "cu8";
    }
    else if (suffix == "f32") {
        sampleAdapter = std::make_unique<RealF32SampleAdapter>();
        _realSignal = true;
        _datatype = "rf32_le";
    }
    else if (suffix == "f64") {
        sampleAdapter = std::make_unique<RealF64SampleAdapter>();
        _realSignal = true;
        _datatype = "rf64_le";
    }
    else if (suffix == "s16") {
        sampleAdapter = std::make_unique<RealS16SampleAdapter>();
        _realSignal = true;
        _datatype = "ri16_le";
    }
    else if (suffix == "s8") {
        sampleAdapter = std::make_unique<RealS8SampleAdapter>();
        _realSignal = true;
        _datatype = "ri8";
    }
    else if (suffix == "u8") {
        sampleAdapter = std::make_unique<RealU8SampleAdapter>();
        _realSignal = true;
        _datatype = "ru8";
    }
    else {
        sampleAdapter = std::make_unique<ComplexF32SampleAdapter>();
        _datatype = "cf32_le";
    }

    QString dataFilename;

    annotationList.clear();
    QString metaFilename;

    if (suffix == "sigmf-meta" || suffix == "sigmf-data" || suffix == "sigmf-") {
        dataFilename = fileInfo.path() + "/" + fileInfo.completeBaseName() + ".sigmf-data";
        metaFilename = fileInfo.path() + "/" + fileInfo.completeBaseName() + ".sigmf-meta";
        auto metaData = readMetaData(metaFilename);
        _wasSigmfInput = true;
        _originalSigmfRoot = metaData;
        QFile datafile(dataFilename);
        if (!datafile.open(QFile::ReadOnly | QIODevice::Text)) {
            auto global = metaData["global"].toObject();
            if (global.contains("core:dataset")) {
                auto datasetfilename = global["core:dataset"].toString();
                if(QFileInfo(datasetfilename).isAbsolute()){
                    dataFilename = datasetfilename;
                }
                else{
                    dataFilename = fileInfo.path() + "/" + datasetfilename;
                }
            }
        }
    }
    else if (suffix == "sigmf" || suffix == "tar") {
        // SigMF archive (ustar bundling a .sigmf-meta + .sigmf-data). IQEngine
        // uses .tar.zst, which decompresses to .tar, so accept both the SigMF
        // archive suffix and a plain tar suffix here. Parse it in place like
        // iq.tar — the data is read at its offset inside the mmap'd archive,
        // so there's no separate extraction copy.
        auto file = std::make_unique<QFile>(filename);
        if (!file->open(QFile::ReadOnly)) {
            throw std::runtime_error(file->errorString().toStdString());
        }
        qint64 archiveSize = file->size();
        uchar *data = file->map(0, archiveSize);
        if (data == nullptr)
            throw std::runtime_error("Error mmapping sigmf archive");

        annotationList.clear();
        dataOffset = 0;
        bool handled = false;
        try {
            handled = openSigmfArchive(data, archiveSize);
        } catch (...) {
            file->unmap(data);
            throw;
        }

        if (handled) {
            cleanup();
            inputFile = file.release();
            mmapData = data;
            invalidate();
            return;
        }

        // The archive carried no .sigmf-meta/.sigmf-data. Drop the mapping;
        // for a plain `.tar` fall through to the raw path (preserving the
        // pre-existing "open as raw IQ" behavior), but a `.sigmf` that isn't
        // a valid archive is a hard error.
        file->unmap(data);
        if (suffix != "tar")
            throw std::runtime_error("sigmf archive: no .sigmf-meta/.sigmf-data found in archive");
        dataFilename = filename;
    }
    else {
        dataFilename = filename;
    }

    auto file = std::make_unique<QFile>(dataFilename);
    if (!file->open(QFile::ReadOnly)) {
        throw std::runtime_error(file->errorString().toStdString());
    }

    auto size = file->size();
    sampleCount = size / sampleAdapter->sampleSize();

    auto data = file->map(0, size);
    if (data == nullptr)
        throw std::runtime_error("Error mmapping file");

    cleanup();

    inputFile = file.release();
    mmapData = data;

    invalidate();
}

void InputSource::setSampleRate(double rate)
{
    sampleRate = rate;
    invalidate();
}

void InputSource::setCenterFrequency(double freq)
{
    // `frequency` lives on SampleSource<T> and is the value the spectrogram's
    // frequency axis labels reference (and what derived plots read via
    // getFrequency()). Auto-init paths (gqrx filenames, etc.) call this so
    // the axis reads true Hz instead of "0 Hz" baseband.
    frequency = freq;
    invalidate();
}

double InputSource::rate()
{
    return sampleRate;
}

std::unique_ptr<std::complex<float>[]> InputSource::getSamples(size_t start, size_t length)
{
    if (inputFile == nullptr)
        return nullptr;

    if (mmapData == nullptr)
        return nullptr;

    if(start < 0 || length < 0)
        return nullptr;

    if (start + length > sampleCount)
        return nullptr;

    auto dest = std::make_unique<std::complex<float>[]>(length);
    sampleAdapter->copyRange(mmapData + dataOffset, start, length, dest.get());

    return dest;
}

void InputSource::setFormat(std::string fmt){
    _fmt = fmt;
}

namespace {

// Encode a QColor as the SigMF presentation-color string "#RRGGBBAA". Inverse
// of the parsing in readMetaData which rotates Qt's "#AARRGGBB" form.
QString sigmfColor(const QColor &c) {
    return QStringLiteral("#%1%2%3%4")
        .arg(c.red(),   2, 16, QChar('0'))
        .arg(c.green(), 2, 16, QChar('0'))
        .arg(c.blue(),  2, 16, QChar('0'))
        .arg(c.alpha(), 2, 16, QChar('0'))
        .toUpper();
}

QJsonObject annotationToJson(const Annotation &a) {
    QJsonObject ann;
    ann.insert("core:sample_start", (qint64)a.sampleRange.minimum);
    ann.insert("core:sample_count",
               (qint64)(a.sampleRange.maximum - a.sampleRange.minimum + 1));
    ann.insert("core:freq_lower_edge", a.frequencyRange.minimum);
    ann.insert("core:freq_upper_edge", a.frequencyRange.maximum);
    if (!a.label.isEmpty())       ann.insert("core:label", a.label);
    if (!a.description.isEmpty()) ann.insert("core:description", a.description);
    if (!a.comment.isEmpty())     ann.insert("core:comment", a.comment);
    if (a.boxColor.isValid() && a.boxColor != QColor("white"))
        ann.insert("presentation:color", sigmfColor(a.boxColor));
    return ann;
}

}  // namespace

void InputSource::addAnnotation(const Annotation &a)
{
    annotationList.push_back(a);
    _annotationsDirty = true;
    for (auto &cb : _annotCbs) if (cb) cb();
}

bool InputSource::updateAnnotation(int index, const Annotation &a)
{
    if (index < 0 || index >= (int)annotationList.size()) return false;
    annotationList[index] = a;
    _annotationsDirty = true;
    for (auto &cb : _annotCbs) if (cb) cb();
    return true;
}

bool InputSource::removeAnnotation(int index)
{
    if (index < 0 || index >= (int)annotationList.size()) return false;
    annotationList.erase(annotationList.begin() + index);
    _annotationsDirty = true;
    for (auto &cb : _annotCbs) if (cb) cb();
    return true;
}

bool InputSource::saveAnnotations(QString *errorOut)
{
    if (_filePath.isEmpty()) {
        if (errorOut) *errorOut = "No file is open.";
        return false;
    }

    QFileInfo fileInfo(_filePath);
    QString metaPath;
    if (_wasSigmfInput) {
        metaPath = fileInfo.path() + "/" + fileInfo.completeBaseName() + ".sigmf-meta";
    } else {
        // Sidecar next to the original data file. Keep the full original
        // filename in the meta name so a follow-up open of either still
        // resolves the right pair via core:dataset.
        metaPath = _filePath + ".sigmf-meta";
    }

    QJsonArray annotations;
    for (const auto &a : annotationList)
        annotations.append(annotationToJson(a));

    QJsonObject root;
    if (_wasSigmfInput && !_originalSigmfRoot.isEmpty()) {
        // Round-trip every other key the original meta had; only annotations
        // gets replaced. This avoids losing custom captures, hardware tags,
        // extension keys, etc.
        root = _originalSigmfRoot;
        root.insert("annotations", annotations);
    } else {
        QJsonObject global;
        global.insert("core:datatype",
                      _datatype.isEmpty() ? QStringLiteral("cf32_le") : _datatype);
        if (sampleRate > 0.0)
            global.insert("core:sample_rate", sampleRate);
        global.insert("core:version", QStringLiteral("1.0.0"));
        global.insert("core:dataset", fileInfo.fileName());
        global.insert("core:description",
                      QStringLiteral("Sidecar created by inspectrum"));
        QJsonObject capture;
        capture.insert("core:sample_start", 0);
        capture.insert("core:frequency", frequency);
        QJsonArray captures;
        captures.append(capture);
        root.insert("global", global);
        root.insert("captures", captures);
        root.insert("annotations", annotations);
    }

    QFile out(metaPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorOut) *errorOut = "Could not open " + metaPath + " for writing: " + out.errorString();
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.close();

    // After a successful save, the on-disk root *is* the new authority — so
    // a subsequent save round-trips against the version we just wrote, not
    // the older parsed copy. Marks us as a SigMF-paired input from now on.
    _originalSigmfRoot = root;
    _wasSigmfInput = true;
    _annotationsDirty = false;
    for (auto &cb : _annotCbs) if (cb) cb();
    return true;
}
