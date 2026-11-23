#ifndef TEA_2D_MATCHER_HPP
#define TEA_2D_MATCHER_HPP

#include "types.hpp"
#include <functional>

namespace tea::vision
{
void ValidateTemplate(const Image& image);
void ValidateTarget(const Target& target);
std::optional<Detection> Match(const Image& frame, const Target& target,
    const std::function<bool()>& keepSearching = {}, bool firstMatch = false);
}

#endif
