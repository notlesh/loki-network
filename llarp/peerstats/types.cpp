#include <peerstats/types.hpp>

namespace llarp
{
  PeerStats::PeerStats() = default;

  PeerStats::PeerStats(const RouterID& routerId_)
  {
    routerId = routerId_.ToString();
  }

  PeerStats&
  PeerStats::operator+=(const PeerStats& other)
  {
    numConnectionAttempts += other.numConnectionAttempts;
    numConnectionSuccesses += other.numConnectionSuccesses;
    numConnectionRejections += other.numConnectionRejections;
    numConnectionTimeouts += other.numConnectionTimeouts;

    numPathBuilds += other.numPathBuilds;
    numPacketsAttempted += other.numPacketsAttempted;
    numPacketsSent += other.numPacketsSent;
    numPacketsDropped += other.numPacketsDropped;
    numPacketsResent += other.numPacketsResent;

    numDistinctRCsReceived += other.numDistinctRCsReceived;
    numLateRCs += other.numLateRCs;

    peakBandwidthBytesPerSec = std::max(peakBandwidthBytesPerSec, other.peakBandwidthBytesPerSec);
    longestRCReceiveIntervalMs =
        std::max(longestRCReceiveIntervalMs, other.longestRCReceiveIntervalMs);
    mostExpiredRCMs = std::max(mostExpiredRCMs, other.mostExpiredRCMs);

    return *this;
  }

  bool
  PeerStats::operator==(const PeerStats& other)
  {
    return routerId == other.routerId and numConnectionAttempts == other.numConnectionAttempts
        and numConnectionSuccesses == other.numConnectionSuccesses
        and numConnectionRejections == other.numConnectionRejections
        and numConnectionTimeouts == other.numConnectionTimeouts

        and numPathBuilds == other.numPathBuilds
        and numPacketsAttempted == other.numPacketsAttempted
        and numPacketsSent == other.numPacketsSent and numPacketsDropped == other.numPacketsDropped
        and numPacketsResent == other.numPacketsResent

        and numDistinctRCsReceived == other.numDistinctRCsReceived
        and numLateRCs == other.numLateRCs

        and peakBandwidthBytesPerSec == other.peakBandwidthBytesPerSec
        and longestRCReceiveIntervalMs == other.longestRCReceiveIntervalMs
        and mostExpiredRCMs == other.mostExpiredRCMs;
  }

};  // namespace llarp