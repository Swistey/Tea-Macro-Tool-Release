#include "recognizer.hpp"
#include "matcher.hpp"
#include <set>
#include <stdexcept>
#include <utility>

namespace tea::vision
{
Recognizer::Recognizer(Capture capture) : capture(std::move(capture))
{
    if (!this->capture) throw std::invalid_argument("a recognizer needs a capture function");
}

void Recognizer::SetTargets(std::vector<Target> targets)
{
    std::set<std::string> ids;
    for (const auto& target : targets)
    {
        ValidateTarget(target);
        if (!ids.insert(target.id).second) throw std::invalid_argument("target ids must be unique");
    }
    auto replacement = std::make_shared<const std::vector<Target>>(std::move(targets));
    std::lock_guard<std::mutex> lock(mutex);
    this->targets = std::move(replacement);
}

void Recognizer::SetOnDetection(OnDetection callback)
{
    std::lock_guard<std::mutex> lock(mutex);
    onDetection = std::move(callback);
}

std::vector<Detection> Recognizer::Scan() { return Scan(capture()); }

std::vector<Detection> Recognizer::Scan(const Image& frame)
{
    if (!frame.Valid()) throw std::invalid_argument("invalid search image");
    std::shared_ptr<const std::vector<Target>> snapshot;
    OnDetection callback;
    {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot = targets;
        callback = onDetection;
    }
    std::vector<Detection> detections;
    const auto timestamp = std::chrono::steady_clock::now();
    for (const auto& target : *snapshot)
    {
        if (auto result = Match(frame, target))
        {
            result->timestamp = timestamp;
            detections.push_back(std::move(*result));
        }
    }
    // Callbacks run on the calling thread, outside the configuration lock.
    for (const auto& detection : detections) if (callback) callback(detection);
    return detections;
}
}
