// Platforms without encoders (the macOS/Linux test build): exporting needs an explicit SinkFactory.
#include "exporter.h"

namespace vbnx {

std::unique_ptr<FrameSink> makeSink(const Options&, const fs::path&, uint32_t, uint32_t) {
    throw Error(Error::Kind::encoderFailed, "No encoders on this platform.");
}

}  // namespace vbnx
