/*
 * Copyright 2018 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkGlyphRun.h"

#include <algorithm>
#include <new>
#include <tuple>

#include "SkDevice.h"
#include "SkDraw.h"
#include "SkGlyphCache.h"
#include "SkMSAN.h"
#include "SkMakeUnique.h"
#include "SkPaint.h"
#include "SkPaintPriv.h"
#include "SkStrikeCache.h"
#include "SkTextBlob.h"
#include "SkTextBlobRunIterator.h"
#include "SkTo.h"
#include "SkUtils.h"

namespace {
static SkTypeface::Encoding convert_encoding(SkPaint::TextEncoding encoding) {
    switch (encoding) {
        case  SkPaint::kUTF8_TextEncoding: return SkTypeface::kUTF8_Encoding;
        case SkPaint::kUTF16_TextEncoding: return SkTypeface::kUTF16_Encoding;
        case SkPaint::kUTF32_TextEncoding: return SkTypeface::kUTF32_Encoding;
        default: return SkTypeface::kUTF32_Encoding;
    }
}
}  // namespace

// -- SkGlyphRun -----------------------------------------------------------------------------------
SkGlyphRun::SkGlyphRun(SkPaint&& runPaint,
                       SkSpan<const uint16_t> denseIndices,
                       SkSpan<const SkPoint> positions,
                       SkSpan<const SkGlyphID> glyphIDs,
                       SkSpan<const SkGlyphID> uniqueGlyphIDs,
                       SkSpan<const char> text,
                       SkSpan<const uint32_t> clusters)
        : fUniqueGlyphIDIndices{denseIndices}
        , fPositions{positions}
        , fTemporaryShuntGlyphIDs{glyphIDs}
        , fUniqueGlyphIDs{uniqueGlyphIDs}
        , fText{text}
        , fClusters{clusters}
        , fRunPaint{std::move(runPaint)} {}

void SkGlyphRun::temporaryShuntToDrawPosText(SkBaseDevice* device) {

    auto pos = (const SkScalar*) this->positions().data();
    auto origin = SkPoint::Make(0, 0);

    if (!fTemporaryShuntGlyphIDs.empty()) {
        device->drawPosText(
                fTemporaryShuntGlyphIDs.data(), fTemporaryShuntGlyphIDs.size() * sizeof(SkGlyphID),
                pos, 2, origin, fRunPaint);
    }
}

void SkGlyphRun::temporaryShuntToCallback(TemporaryShuntCallback callback) {
    auto bytes = (const char *)fTemporaryShuntGlyphIDs.data();
    auto pos = (const SkScalar*) this->positions().data();
    callback(fTemporaryShuntGlyphIDs.size(), bytes, pos);
}

// -- SkGlyphIDSet ---------------------------------------------------------------------------------
// A faster set implementation that does not need any initialization, and reading the set items
// is order the number of items, and not the size of the universe.
// This implementation is based on the paper by Briggs and Torczon, "An Efficient Representation
// for Sparse Sets"
//
// This implementation assumes that the unique glyphs added are appended to a vector that may
// already have unique glyph from a previous computation. This allows the packing of multiple
// UniqueID sequences in a single vector.
SkSpan<const SkGlyphID> SkGlyphIDSet::uniquifyGlyphIDs(
        uint32_t universeSize,
        SkSpan<const SkGlyphID> glyphIDs,
        SkGlyphID* uniqueGlyphIDs,
        uint16_t* denseIndices) {
    static constexpr SkGlyphID  kUndefGlyph{0};

    if (universeSize > fUniverseToUniqueSize) {
        fUniverseToUnique.reset(universeSize);
        fUniverseToUniqueSize = universeSize;
        // If the following bzero becomes a performance problem, the memory can be marked as
        // initialized for valgrind and msan.
        // valgrind = VALGRIND_MAKE_MEM_DEFINED(fUniverseToUnique, universeSize * sizeof(SkGlyphID))
        // msan = sk_msan_mark_initialized(fUniverseToUnique, universeSize * sizeof(SkGlyphID))
        sk_bzero(fUniverseToUnique, universeSize * sizeof(SkGlyphID));
    }

    // No need to clear fUniverseToUnique here... the set insertion algorithm is designed to work
    // correctly even when the fUniverseToUnique buffer is uninitialized!

    size_t uniqueSize = 0;
    size_t denseIndicesCursor = 0;
    for (auto glyphID : glyphIDs) {

        // If the glyphID is not in range then it is the undefined glyph.
        if (glyphID >= universeSize) {
            glyphID = kUndefGlyph;
        }

        // The index into the unique ID vector.
        auto uniqueIndex = fUniverseToUnique[glyphID];

        if (uniqueIndex >= uniqueSize || uniqueGlyphIDs[uniqueIndex] != glyphID) {
            uniqueIndex = SkTo<uint16_t>(uniqueSize);
            uniqueGlyphIDs[uniqueSize] = glyphID;
            fUniverseToUnique[glyphID] = uniqueIndex;
            uniqueSize += 1;
        }

        denseIndices[denseIndicesCursor++] = uniqueIndex;
    }

    // If we're hanging onto these arrays for a long time, we don't want their size to drift
    // endlessly upwards. It's unusual to see a typeface with more than 4096 possible glyphs.
    if (fUniverseToUniqueSize > 4096) {
        fUniverseToUnique.reset(4096);
        sk_bzero(fUniverseToUnique, 4096 * sizeof(SkGlyphID));
        fUniverseToUniqueSize = 4096;
    }

    return SkSpan<const SkGlyphID>(uniqueGlyphIDs, uniqueSize);
}

// -- SkGlyphRunBuilder ----------------------------------------------------------------------------
void SkGlyphRunBuilder::prepareDrawText(
        const SkPaint& paint, const void* bytes, size_t byteLength, SkPoint origin) {
    auto glyphIDs = textToGlyphIDs(paint, bytes, byteLength);
    if (!glyphIDs.empty()) {
        this->initialize(glyphIDs.size());
        this->drawText(paint, glyphIDs, origin, SkSpan<const char>(), SkSpan<const uint32_t>());
    }
}

void SkGlyphRunBuilder::prepareDrawPosTextH(const SkPaint& paint, const void* bytes,
                                            size_t byteLength, const SkScalar* xpos,
                                            SkScalar constY) {
    auto glyphIDs = textToGlyphIDs(paint, bytes, byteLength);
    if (!glyphIDs.empty()) {
        this->initialize(glyphIDs.size());
        this->drawPosTextH(
                paint, glyphIDs, xpos, constY, SkSpan<const char>(), SkSpan<const uint32_t>());
    }
}

void SkGlyphRunBuilder::prepareDrawPosText(const SkPaint& paint, const void* bytes,
                                           size_t byteLength, const SkPoint* pos) {
    auto glyphIDs = textToGlyphIDs(paint, bytes, byteLength);
    if (!glyphIDs.empty()) {
        this->initialize(glyphIDs.size());
        this->drawPosText(paint, glyphIDs, pos, SkSpan<const char>(), SkSpan<const uint32_t>());
    }
}

SkGlyphRun* SkGlyphRunBuilder::useGlyphRun() {
    return &fScratchGlyphRun;
}

void SkGlyphRunBuilder::initialize(size_t totalRunSize) {
    fUniqueID = 0;

    if (totalRunSize > fMaxTotalRunSize) {
        fMaxTotalRunSize = totalRunSize;
        fUniqueGlyphIDIndices.reset(fMaxTotalRunSize);
        fPositions.reset(fMaxTotalRunSize);
        fUniqueGlyphIDs.reset(fMaxTotalRunSize);
    }

    // Be sure to clean up the last run before we reuse it.
    fScratchGlyphRun.~SkGlyphRun();
}

SkSpan<const SkGlyphID> SkGlyphRunBuilder::textToGlyphIDs(
        const SkPaint& paint, const void* bytes, size_t byteLength) {
    auto encoding = paint.getTextEncoding();
    if (encoding != SkPaint::kGlyphID_TextEncoding) {
        auto tfEncoding = convert_encoding(encoding);
        int utfSize = SkUTFN_CountUnichars(tfEncoding, bytes, byteLength);
        if (utfSize > 0) {
            size_t runSize = SkTo<size_t>(utfSize);
            fScratchGlyphIDs.resize(runSize);
            auto typeface = SkPaintPriv::GetTypefaceOrDefault(paint);
            typeface->charsToGlyphs(bytes, tfEncoding, fScratchGlyphIDs.data(), runSize);
            return SkSpan<const SkGlyphID>{fScratchGlyphIDs};
        } else {
            return SkSpan<const SkGlyphID>();
        }
    } else {
        return SkSpan<const SkGlyphID>((const SkGlyphID*)bytes, byteLength / 2);
    }
}

SkSpan<const SkGlyphID> SkGlyphRunBuilder::addDenseAndUnique(
        const SkPaint& paint,
        SkSpan<const SkGlyphID> glyphIDs) {
    SkSpan<const SkGlyphID> uniquifiedGlyphIDs;
    if (!glyphIDs.empty()) {
        auto typeface = SkPaintPriv::GetTypefaceOrDefault(paint);
        auto glyphUniverseSize = typeface->countGlyphs();
        // There better be glyphs in the font if we want to uniqify.
        if (glyphUniverseSize > 0) {
            uniquifiedGlyphIDs = fGlyphIDSet.uniquifyGlyphIDs(
                    glyphUniverseSize, glyphIDs, fUniqueGlyphIDs, fUniqueGlyphIDIndices);
        }
    }

    return uniquifiedGlyphIDs;
}

void SkGlyphRunBuilder::makeGlyphRun(
        const SkPaint& runPaint,
        SkSpan<const SkGlyphID> glyphIDs,
        SkSpan<const SkPoint> positions,
        SkSpan<const uint16_t> uniqueGlyphIDIndices,
        SkSpan<const SkGlyphID> uniqueGlyphIDs,
        SkSpan<const char> text,
        SkSpan<const uint32_t> clusters) {

    // Ignore empty runs.
    if (!glyphIDs.empty()) {
        SkPaint glyphRunPaint{runPaint};
        glyphRunPaint.setTextEncoding(SkPaint::kGlyphID_TextEncoding);
        glyphRunPaint.setTextAlign(SkPaint::kLeft_Align);

        new ((void*)&fScratchGlyphRun) SkGlyphRun{
                std::move(glyphRunPaint),
                uniqueGlyphIDIndices,
                positions,
                glyphIDs,
                uniqueGlyphIDs,
                text,
                clusters
        };
    }
}

void SkGlyphRunBuilder::drawText(
        const SkPaint& paint, SkSpan<const SkGlyphID> glyphIDs, SkPoint origin,
        SkSpan<const char> text, SkSpan<const uint32_t> clusters) {
    SkASSERT(!glyphIDs.empty());

    auto runSize = glyphIDs.size();

    auto unqiueGlyphIDs = this->addDenseAndUnique(paint, glyphIDs);

    if (!unqiueGlyphIDs.empty()) {
        fScratchAdvances.resize(runSize);
        {
            auto cache = SkStrikeCache::FindOrCreateStrikeExclusive(paint);
            cache->getAdvances(unqiueGlyphIDs, fScratchAdvances.data());
        }

        SkPoint endOfLastGlyph = origin;

        for (size_t i = 0; i < runSize; i++) {
            fPositions[i] = endOfLastGlyph;
            endOfLastGlyph += fScratchAdvances[fUniqueGlyphIDIndices[i]];
        }

        if (paint.getTextAlign() != SkPaint::kLeft_Align) {
            SkVector len = endOfLastGlyph - origin;
            if (paint.getTextAlign() == SkPaint::kCenter_Align) {
                len.scale(SK_ScalarHalf);
            }
            for (auto& pt : SkSpan<SkPoint>{fPositions, runSize}) {
                pt -= len;
            }
        }


        this->makeGlyphRun(
                paint,
                glyphIDs,
                SkSpan<const SkPoint>{fPositions, runSize},
                SkSpan<const uint16_t>{fUniqueGlyphIDIndices, runSize},
                unqiueGlyphIDs,
                text,
                clusters);
    }
}

void SkGlyphRunBuilder::drawPosTextH(const SkPaint& paint, SkSpan<const SkGlyphID> glyphIDs,
                                     const SkScalar* xpos, SkScalar constY,
                                     SkSpan<const char> text, SkSpan<const uint32_t> clusters) {
    SkASSERT(!glyphIDs.empty());
    auto runSize = glyphIDs.size();

    // The dense indices are not used by the rest of the stack yet.
    #ifdef SK_DEBUG
        this->addDenseAndUnique(paint, glyphIDs);
    #endif

    // TODO: when using the unique glyph system have a guard that there are actually glyphs like
    // drawText above.
    auto posCursor = fPositions.get();
    for (auto x : SkSpan<const SkScalar>{xpos, runSize}) {
        *posCursor++ = SkPoint::Make(x, constY);
    }

    this->makeGlyphRun(
            paint,
            glyphIDs,
            SkSpan<const SkPoint>{fPositions, runSize},
            SkSpan<const uint16_t>{},
            SkSpan<const SkGlyphID>{},
            text,
            clusters);
}

void SkGlyphRunBuilder::drawPosText(const SkPaint& paint, SkSpan<const SkGlyphID> glyphIDs,
                                    const SkPoint* pos,
                                    SkSpan<const char> text, SkSpan<const uint32_t> clusters) {
    SkASSERT(!glyphIDs.empty());
    auto runSize = glyphIDs.size();

    // The dense indices are not used by the rest of the stack yet.
    #ifdef SK_DEBUG
        this->addDenseAndUnique(paint, glyphIDs);
    #endif

    // TODO: when using the unique glyph system have a guard that there are actually glyphs like
    // drawText above.
    this->makeGlyphRun(
            paint,
            glyphIDs,
            SkSpan<const SkPoint>{pos, runSize},
            SkSpan<const uint16_t>{},
            SkSpan<const SkGlyphID>{},
            text,
            clusters);
}


