// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_PCM_PACKET_H
#define USB_AUDIO360_PCM_PACKET_H
#include <limits.h>
namespace uac {
typedef char PcmRequires32BitUnsigned[UINT_MAX == 0xffffffffu ? 1 : -1];
// Input: signed PCM represented as MSB-aligned 32-bit bits. Validated format.
inline void WritePcm(unsigned char* p, unsigned sample, unsigned bytes,
                     unsigned bits) {
  if (bits < 32) sample &= 0xffffffffu << (32 - bits);
  sample >>= (4 - bytes) * 8;
  for (unsigned i = 0; i < bytes; ++i) p[i] = (unsigned char)(sample >> (8 * i));
}
// One batch, contiguous packets; no padding to endpoint maximum size.
class PcmPacketBuilder {
 public:
  PcmPacketBuilder(unsigned char* buffer, unsigned capacity, unsigned bytes,
                   unsigned bits, unsigned maximum)
      : buffer_(buffer), capacity_(capacity), bytes_(bytes), bits_(bits),
        maximum_(maximum), total_(0) {}
  unsigned char* Append(unsigned frames, unsigned short* length) {
    if (!buffer_ || !length || bytes_ < 2 || bytes_ > 4 || !bits_ ||
        bits_ > bytes_ * 8 || !frames || frames > maximum_ / (2 * bytes_))
      return 0;
    unsigned size = frames * 2 * bytes_;
    if (size > 65535 || size > capacity_ - total_) return 0;
    unsigned char* p = buffer_ + total_;
    total_ += size; *length = (unsigned short)size;
    return p;
  }
  void Stereo(unsigned char* p, unsigned left, unsigned right) const {
    WritePcm(p, left, bytes_, bits_);
    WritePcm(p + bytes_, right, bytes_, bits_);
  }
  unsigned total() const { return total_; }
 private:
  unsigned char* buffer_;
  unsigned capacity_, bytes_, bits_, maximum_, total_;
};
}
#endif
