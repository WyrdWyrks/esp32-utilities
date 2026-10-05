#pragma once

#include <FastLED.h>
#include <vector>
#include <cstdint>
#include <algorithm>

namespace UxModule
{
    // A logical run of LEDs inside a strip buffer: either a contiguous slice
    // (offset + length) or an arbitrary list of physical indices. Patterns draw
    // through operator[] using logical positions; physicalIndex() maps a logical
    // position back to its index in the strip, which is how UxModule::LedUtilities
    // composites pattern layers by priority.
    class LedSegment {
    public:
        // Default — empty/null segment
        LedSegment()
            : _length(0), _indexed(false), _offset(0), _contiguous(nullptr), _master(nullptr)
        {}

        // Contiguous segment — offset + length into master buffer
        LedSegment(CRGB* master, size_t offset, size_t length)
            : _length(length), _indexed(false), _offset(offset), _contiguous(master + offset), _master(master)
        {}

        // Indexed segment — arbitrary indices into master buffer
        LedSegment(CRGB* master, std::vector<size_t> indices)
            : _length(indices.size()), _indexed(true), _offset(0), _contiguous(nullptr), _master(master),
              _indices(std::move(indices))
        {}

        // Union of several segments on the same buffer, in the order given.
        // A physical LED that appears in more than one part is kept once (first
        // occurrence), so a composite never draws the same LED twice.
        static LedSegment Composite(const std::vector<LedSegment> &parts)
        {
            CRGB *master = nullptr;
            std::vector<size_t> indices;

            for (const auto &part : parts)
            {
                if (master == nullptr)
                {
                    master = part._master;
                }

                for (size_t i = 0; i < part.length(); i++)
                {
                    size_t idx = part.physicalIndex(i);
                    if (std::find(indices.begin(), indices.end(), idx) == indices.end())
                    {
                        indices.push_back(idx);
                    }
                }
            }

            return LedSegment(master, std::move(indices));
        }

        size_t length() const { return _length; }

        // Index in the strip buffer of logical position i.
        size_t physicalIndex(size_t i) const
        {
            if (_indexed)
            {
                return _indices[i];
            }
            return _offset + i;
        }

        // Highest physical index this segment touches, or 0 when empty.
        size_t maxPhysicalIndex() const
        {
            size_t maxIdx = 0;
            for (size_t i = 0; i < _length; i++)
            {
                maxIdx = std::max(maxIdx, physicalIndex(i));
            }
            return maxIdx;
        }

        // Points the segment at another buffer with the same layout. Used to give
        // every registered pattern its own layer the size of the strip.
        void rebase(CRGB* master)
        {
            _master = master;
            if (!_indexed)
            {
                _contiguous = (master != nullptr) ? master + _offset : nullptr;
            }
        }

        CRGB& operator[](size_t i)
        {
            if (_indexed)
            {
                return _master[_indices[i]];
            }
            return _contiguous[i];
        }

        const CRGB& operator[](size_t i) const
        {
            if (_indexed)
            {
                return _master[_indices[i]];
            }
            return _contiguous[i];
        }

        void fill(CRGB color)
        {
            for (size_t i = 0; i < _length; i++)
            {
                (*this)[i] = color;
            }
        }

        void clear()
        {
            fill(CRGB::Black);
        }

        // Iterator support so patterns can use range-for
        struct Iterator {
            LedSegment* seg;
            size_t idx;

            CRGB& operator*() { return (*seg)[idx]; }
            Iterator& operator++() { ++idx; return *this; }
            bool operator!=(const Iterator& other) const { return idx != other.idx; }
        };

        Iterator begin() { return { this, 0 }; }
        Iterator end()   { return { this, _length }; }

    private:
        size_t _length;
        bool _indexed;

        // Contiguous mode
        size_t _offset;
        CRGB* _contiguous;

        // Buffer the segment draws into (both modes)
        CRGB* _master;

        // Indexed mode
        std::vector<size_t> _indices;
    };
}
