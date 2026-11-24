#ifndef TEA_2D_RECOGNIZER_HPP
#define TEA_2D_RECOGNIZER_HPP

#include <functional>
#include <memory>
#include <mutex>
#include "capture.hpp"

namespace tea::vision
{
class Recognizer
{
public:
    using Capture = std::function<Image()>;
    using OnDetection = std::function<void(const Detection&)>;

    explicit Recognizer(Capture capture = [] { return CaptureScreen(); });
    void SetTargets(std::vector<Target> targets);
    void SetOnDetection(OnDetection callback);
    // One best match per target on every scan. Callbacks run synchronously; exceptions propagate.
    std::vector<Detection> Scan();
    std::vector<Detection> Scan(const Image& frame);

private:
    Capture capture;
    std::mutex mutex;
    std::shared_ptr<const std::vector<Target>> targets = std::make_shared<const std::vector<Target>>();
    OnDetection onDetection;
};
}

#endif
