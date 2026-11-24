#ifndef TEA_2D_IMAGE_LIBRARY_HPP
#define TEA_2D_IMAGE_LIBRARY_HPP

#include "types.hpp"
#include <windows.h>
#include <filesystem>
#include <json.hpp>

namespace tea::vision
{
class ImageLibrary
{
public:
    explicit ImageLibrary(std::filesystem::path directory);
    const nlohmann::json& List() const { return catalog.at("images"); }
    const std::string& RecoveryNotice() const { return recoveryNotice; }
    int NextId() const { return catalog.at("nextId").get<int>(); }
    void ReserveThrough(int id);
    bool Has(int id) const;
    void Import(HWND owner);
    void Import(const std::filesystem::path& file);
    void Add(std::string name, const Image& image);
    void Rename(int id, std::string name);
    void Remove(int id);
    Image Load(int id) const;
    std::string Preview(int id) const;

private:
    std::filesystem::path File(int id) const;
    std::filesystem::path PendingDeletion(int id) const;
    void Save(nlohmann::json next);
    const std::filesystem::path directory;
    nlohmann::json catalog;
    std::string recoveryNotice;
    bool archiveIndex = false;
};
}

#endif
