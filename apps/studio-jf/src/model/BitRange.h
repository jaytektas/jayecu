#pragma once

// BitRange — a value packed inside a word: bits [lo..hi] of the word at some field's offset.
//
// Its own header because BOTH sides of the data model need it and neither owns it: a TunerStudio ini packs
// flags several to a word in [Constants] (config, read AND written) and [OutputChannels] (telemetry, read
// only) alike, so ConfigField and TelemField carry the same thing. Sharing only the RANGE and the
// EXTRACTION is deliberate — each path keeps its own decode, because telemetry decodes in float to match
// the firmware's SignalBus precision while config reads raw doubles, and only config can write.

#include <cstdint>

    // A value packed inside a word: bits [lo..hi] of the word at the field's offset, -1 = the whole word.
// Config fields and telemetry channels share this because the QUESTION is the same — TunerStudio packs
// flags several to a word in [Constants] and [OutputChannels] alike. Only EXTRACTION is shared: each
// path keeps its own decode (config reads raw doubles; telemetry decodes in float to match the
// firmware's SignalBus precision) and only config can write, so insert() has one caller by nature.
struct BitRange {
    int lo = -1, hi = -1;
    bool packed() const { return lo >= 0 && hi >= lo; }
    uint32_t mask() const {
        const int n = hi - lo + 1;
        return (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    }
    // The storage word itself: U08 / U16 / U32 all carry packed fields, so the word must be masked to ITS
    // width before any bit is read. A 16-bit word decoded as SIGNED arrives as a negative double, which
    // sign-extends to 1-bits above bit 15 — without this, a range near the top of a small word would read
    // those, and a write would try to store them back.
    static uint32_t widthMask(int sizeBytes) {
        return (sizeBytes >= 4 || sizeBytes <= 0) ? 0xFFFFFFFFu : ((1u << (8 * sizeBytes)) - 1u);
    }
    static uint32_t rawWord(double word, int sizeBytes) {
        return static_cast<uint32_t>(static_cast<int64_t>(word)) & widthMask(sizeBytes);
    }

    // Pull this field's bits out of a decoded word. Meaningless for F32 (a packed field is always an
    // integer type) — the caller passes size 0 / leaves the range unset for those.
    double extract(double word, int sizeBytes = 4) const {
        if (!packed()) return word;
        return static_cast<double>((rawWord(word, sizeBytes) >> lo) & mask());
    }
    // Put `value` back, preserving every other bit of the word.
    double insert(double word, double value, int sizeBytes = 4) const {
        if (!packed()) return value;
        const uint32_t w = rawWord(word, sizeBytes);
        const uint32_t v = static_cast<uint32_t>(static_cast<int64_t>(value)) & mask();
        return static_cast<double>((w & ~(mask() << lo)) | (v << lo));
    }
};
