#pragma once

#include <string>

// The imaginary machine Mattrix OS runs on. One place, so that neofetch, free,
// df, top, nproc and /proc all tell the same story.
namespace fakeos {
namespace hardware {

// Mattrix OS "booted" on 1987-02-27 00:00:00 UTC and has not been restarted
// since.
constexpr long long kBootUnix = 541382400;

constexpr const char *kCpuModel = "Mattrix Quantum Threadripper 9999X";
constexpr int kCpuCores = 128;
constexpr const char *kCpuClock = "6.20GHz";
constexpr int kCpuMegahertz = 6200;
constexpr const char *kGpu = "Mattrix Photon-X 9090 Ti Mega Hyper, 96 GB HBM9";
constexpr const char *kDisplay = "2048x1536 @ 85Hz, 21-inch CRT";
constexpr const char *kDiskModel = "Mattrix HyperNVMe 64 TB";

constexpr long long kGiB = 1024LL * 1024L;     // In KiB.
constexpr long long kRamTotalKiB = 512 * kGiB; // 512 GiB.
constexpr long long kRamUsedKiB = 38 * kGiB;   // 38 GiB.
constexpr long long kRamCacheKiB = 61 * kGiB;  // 61 GiB buffers and cache.
constexpr long long kRamSharedKiB = 1 * kGiB;
constexpr long long kRamFreeKiB = kRamTotalKiB - kRamUsedKiB - kRamCacheKiB;
constexpr long long kRamAvailableKiB = kRamTotalKiB - kRamUsedKiB;

constexpr long long kDiskTotalKiB = 64LL * 1024 * kGiB; // 64 TiB.
constexpr long long kDiskBaseUsedKiB =
    8LL * 1024 * kGiB; // 8 TiB before the visitor's files.
constexpr long long kTmpTotalKiB = 256 * kGiB;

// A size in KiB as "38.0GiB" or "8.0TiB".
inline std::string size(long long kib) {
    const double gib = double(kib) / double(kGiB);
    const bool tib = gib >= 1024.0;
    const long tenths = long((tib ? gib / 1024.0 : gib) * 10.0 + 0.5);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) +
           (tib ? "TiB" : "GiB");
}
// MiB with one decimal, as top prints them.
inline std::string mebibytes(long long kib) {
    const long tenths = long(double(kib) / 1024.0 * 10.0 + 0.5);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

} // namespace hardware
} // namespace fakeos
