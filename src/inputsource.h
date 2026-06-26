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

#pragma once

#include <complex>
#include <functional>
#include <QFile>
#include <QJsonObject>
#include "samplesource.h"

class SampleAdapter {
public:
    virtual size_t sampleSize() = 0;
    virtual void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) = 0;
    virtual ~SampleAdapter() { };
};

class InputSource : public SampleSource<std::complex<float>>
{
private:
    QFile *inputFile = nullptr;
    size_t sampleCount = 0;
    double sampleRate = 0.0;
    uchar *mmapData = nullptr;
    // Byte offset of the first IQ sample within the mmap. Non-zero for
    // container formats like Rohde & Schwarz .iq.tar where the raw data
    // sits after a 512-byte tar header inside the archive.
    size_t dataOffset = 0;
    std::unique_ptr<SampleAdapter> sampleAdapter;
    std::string _fmt;
    bool _realSignal = false;
    QString _filePath;
    // SigMF datatype string ("cf32_le", "ci16_le", ...) inferred from the
    // sample adapter that openFile picked. Used when synthesising a sidecar
    // .sigmf-meta for a non-SigMF input.
    QString _datatype;
    // Original parsed meta JSON when the input was opened from a .sigmf-*
    // pair. Empty for raw inputs. saveAnnotations replaces only the
    // "annotations" array so unrelated keys are round-tripped.
    QJsonObject _originalSigmfRoot;
    bool _wasSigmfInput = false;
    bool _annotationsDirty = false;
    // SigMF-archive (tar / tar+zstd) tracking, so saveAnnotations can append an
    // updated .sigmf-meta member back into the container instead of orphaning a
    // sidecar in the decompression cache. _containerPath is the ORIGINAL file
    // the user opened (the .sigmf.zst / .sigmf / .tar), not the decompressed
    // temp that _filePath points at. _archiveMetaName is the tar member name of
    // the meta entry, reused for the appended member so it reads as an update.
    bool _isArchive = false;
    bool _archiveZstd = false;
    QString _containerPath;
    QString _archiveMetaName;
    using AnnotationCallback = std::function<void()>;
    std::vector<AnnotationCallback> _annotCbs;

    QJsonObject readMetaData(const QString &filename);
    // Body of openFile, separated so the public openFile can reset the
    // container-tracking state once and then recurse (for transparent zstd)
    // without that reset clobbering what an outer frame discovered.
    void openFileImpl(const char *filename);
    // Append an updated .sigmf-meta to the open SigMF archive: build a tar
    // member (+ EOF blocks), zstd-compress it as one frame when the container
    // is compressed, and append it to _containerPath. The original bytes are
    // never modified — a failed write is rolled back by truncating to the
    // pre-append size. The newest meta wins on the next open. Returns false and
    // sets *errorOut on failure.
    bool appendMetaToArchive(const QJsonObject &root, QString *errorOut);
    // Parse a SigMF meta document (the JSON bytes of a .sigmf-meta) and apply
    // it: pick the sampleAdapter from core:datatype and populate sampleRate /
    // frequency / annotations. Shared by the .sigmf-* pair path and the
    // in-archive path. Throws on invalid/unsupported meta.
    QJsonObject parseMetaDocument(const QByteArray &bytes);
    // Populate sampleAdapter / sampleRate / sampleCount / frequency /
    // dataOffset from a memory-mapped R&S iq.tar archive. Throws on error.
    void openIqTar(const uchar *data, qint64 size);
    // Populate the same fields from a memory-mapped SigMF archive (a ustar
    // tarball bundling a .sigmf-meta + .sigmf-data, optionally produced by
    // decompressing a .sigmf.zst). The data is read in place at its byte
    // offset within the archive, so no separate extraction copy is made.
    // Returns true if it was a SigMF archive and the fields were populated;
    // false if it carried no .sigmf-meta/.sigmf-data (so the caller can fall
    // back, e.g. open a plain `.tar` as raw IQ).
    bool openSigmfArchive(const uchar *data, qint64 size);

public:
    InputSource();
    ~InputSource();
    void cleanup();
    void openFile(const char *filename);
    std::unique_ptr<std::complex<float>[]> getSamples(size_t start, size_t length);
    size_t count() {
        return sampleCount;
    };
    void setSampleRate(double rate);
    void setCenterFrequency(double freq);
    void setFormat(std::string fmt);
    double rate();
    bool realSignal() {
        return _realSignal;
    };
    float relativeBandwidth() {
        return 1;
    }
    QString filePath() const { return _filePath; }

    // Mutate annotations through these so the dirty flag and change callback
    // fire consistently. Direct vector access still works for the read path.
    void addAnnotation(const Annotation &a);
    bool updateAnnotation(int index, const Annotation &a);
    bool removeAnnotation(int index);
    bool annotationsDirty() const { return _annotationsDirty; }
    // Returns true iff the file was opened from a .sigmf-* pair (i.e. there
    // is an existing sidecar to round-trip). Saving for a non-SigMF input
    // synthesises a fresh meta with core:dataset pointing back to the file.
    bool wasSigmfInput() const { return _wasSigmfInput; }
    // Persists annotations to a .sigmf-meta beside the data file. Returns
    // true on success; on failure populates *errorOut if non-null.
    bool saveAnnotations(QString *errorOut = nullptr);
    void addAnnotationCallback(AnnotationCallback cb) { _annotCbs.push_back(std::move(cb)); }
    QString sigmfDatatype() const { return _datatype; }
};
