#pragma once

#include <assert.h>
#include <cstddef>
#include <cstring>
#include <string>

namespace actaeid::pipewire_video_contract {

inline bool StartsWith(const std::string &value, const char *prefix) {
    const auto prefix_len = std::strlen(prefix);
    return value.size() >= prefix_len && value.compare(0, prefix_len, prefix) == 0;
}

template <typename TProducerFamily, typename TConsumerFamily, std::size_t SourceCount,
          std::size_t SinkCount>
struct SingleSourceSingleSinkContract {
    static_assert(SourceCount == 1,
                  "Actaeid VTuber PipeWire video path must expose exactly one source.");
    static_assert(SinkCount == 1,
                  "Actaeid VTuber PipeWire video path must expose exactly one sink.");

    using ProducerFamily = TProducerFamily;
    using ConsumerFamily = TConsumerFamily;
    static constexpr std::size_t kSourceCount = SourceCount;
    static constexpr std::size_t kSinkCount = SinkCount;
};

struct WarudoSpoutProducerFamily {
    static const char *Name() { return "Warudo -> Spout2PW producer"; }

    static const char *SpoutSenderPrefix() { return "Warudo"; }

    static const char *SelectedSenderEnvVar() { return "SPOUT2PW_SENDER_NAME"; }

    static bool MatchesSpoutSenderName(const std::string &sender_name) {
        return StartsWith(sender_name, SpoutSenderPrefix());
    }
};

struct ObsPipewireConsumerFamily {
    static const char *Name() { return "OBS pipewire-video-source consumer"; }

    static const char *PipeWireNodePrefix() { return "obs_pwvideo."; }
};

using Contract = SingleSourceSingleSinkContract<WarudoSpoutProducerFamily,
                                                ObsPipewireConsumerFamily, 1, 1>;

inline bool AllowsSourceName(const std::string &sender_name) {
    return Contract::ProducerFamily::MatchesSpoutSenderName(sender_name);
}

inline const char *SourceFamilyName() {
    return Contract::ProducerFamily::Name();
}

inline const char *SourceSenderPrefix() {
    return Contract::ProducerFamily::SpoutSenderPrefix();
}

inline const char *SourceSelectedSenderEnvVar() {
    return Contract::ProducerFamily::SelectedSenderEnvVar();
}

inline const char *SinkFamilyName() {
    return Contract::ConsumerFamily::Name();
}

inline const char *SinkNodePrefix() {
    return Contract::ConsumerFamily::PipeWireNodePrefix();
}

inline const char *Warning() {
    return "WARNING: Actaeid VTuber PipeWire video contract is exactly one "
           "source and one sink: select exactly one Warudo Spout sender for "
           "bridging into exactly one Spout2PW PipeWire producer node, and "
           "consume it with exactly one OBS pipewire-video-source node. Do "
           "not add extra injection points, fan-out, or fallback capture "
           "paths here.";
}

}  // namespace actaeid::pipewire_video_contract
