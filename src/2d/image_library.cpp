#include "image_library.hpp"
#include "matcher.hpp"
#include "../tea_wraps.hpp"
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace tea::vision
{
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT MaxDimension = 8192;
constexpr unsigned MaxPixels = 16000000;

void Check(HRESULT result, const char* error)
{
    if (FAILED(result)) throw std::runtime_error(error);
}

struct ComApartment
{
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComApartment() { if (result != RPC_E_CHANGED_MODE) Check(result, "image tools could not be initialized"); }
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

ComPtr<IWICImagingFactory> Imaging()
{
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
        "image tools could not be initialized");
    return factory;
}

void ValidateSize(UINT width, UINT height)
{
    if (!width || !height || width > MaxDimension || height > MaxDimension || std::uint64_t(width) * height > MaxPixels)
        throw std::invalid_argument("choose a smaller image (up to 8192 pixels per side and 16 megapixels)");
}

ComPtr<IWICBitmapFrameDecode> OpenImage(IWICImagingFactory* factory, const fs::path& path)
{
    ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder),
        "the image file could not be read");
    ComPtr<IWICBitmapFrameDecode> frame;
    Check(decoder->GetFrame(0, &frame), "the image file is invalid");
    return frame;
}

void WritePng(IWICImagingFactory* factory, IStream* stream, IWICBitmapSource* source)
{
    const auto error = "the image could not be saved";
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), error);
    Check(encoder->Initialize(stream, WICBitmapEncoderNoCache), error);
    Check(encoder->CreateNewFrame(&frame, nullptr), error);
    Check(frame->Initialize(nullptr), error);
    Check(frame->WriteSource(source, nullptr), error);
    Check(frame->Commit(), error);
    Check(encoder->Commit(), error);
}

Image ReadImage(const fs::path& path)
{
    const ComApartment apartment;
    const auto factory = Imaging();
    const auto frame = OpenImage(factory.Get(), path);
    UINT width = 0, height = 0;
    Check(frame->GetSize(&width, &height), "the image size could not be read");
    ValidateSize(width, height);
    ComPtr<IWICFormatConverter> converter;
    Check(factory->CreateFormatConverter(&converter), "the image format could not be read");
    Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
        nullptr, 0, WICBitmapPaletteTypeCustom), "the image format could not be read");
    Image image{static_cast<int>(width), static_cast<int>(height), std::vector<Pixel>(width * height)};
    Check(converter->CopyPixels(nullptr, width * sizeof(Pixel), width * height * sizeof(Pixel),
        reinterpret_cast<BYTE*>(image.pixels.data())), "the image pixels could not be read");
    return image;
}

void ValidateName(const std::string& name)
{
    if (name.size() > 1024 || name.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::invalid_argument("enter an image name");
}

bool WithId(const Json& image, int id) { return image.at("id") == id; }

bool CatalogContains(const Json& images, int id)
{
    const auto entry = std::lower_bound(images.begin(), images.end(), id,
        [](const Json& image, int candidate) { return image.at("id").get<int>() < candidate; });
    return entry != images.end() && WithId(*entry, id);
}

std::optional<int> StoredImageId(const fs::path& file)
{
    if (_wcsicmp(file.extension().c_str(), L".png") != 0) return std::nullopt;
    const auto stem = file.stem().u8string();
    int id = 0;
    const auto result = std::from_chars(stem.data(), stem.data() + stem.size(), id);
    if (result.ec != std::errc{} || id <= 0 || stem != std::to_string(id)) return std::nullopt;
    return id;
}

Json ReadCatalog(const fs::path& file)
{
    std::ifstream input(file);
    auto catalog = Json::parse(input);
    const int next = catalog.at("nextId").get<int>();
    const auto& images = catalog.at("images");
    if (!images.is_array() || next <= 0) throw std::invalid_argument("invalid image catalog");
    int last = 0;
    for (const auto& image : images)
    {
        const int id = image.at("id").get<int>();
        ValidateName(image.at("name").get<std::string>());
        ValidateSize(image.at("width").get<UINT>(), image.at("height").get<UINT>());
        if (id <= last || id >= next) throw std::invalid_argument("invalid image id");
        last = id;
    }
    for (int id : catalog.value("pendingRecovery", std::vector<int>{}))
        if (id <= 0 || id >= next || CatalogContains(images, id))
            throw std::invalid_argument("invalid image recovery id");
    return catalog;
}
}

ImageLibrary::ImageLibrary(fs::path directory)
    : directory(std::move(directory)), catalog({{"images", Json::array()}, {"nextId", 1}})
{
    const auto file = this->directory / L"index.json";
    std::error_code accessError;
    const bool hasIndex = fs::exists(file, accessError);
    bool rebuild = !hasIndex;
    if (hasIndex)
    {
        try { catalog = ReadCatalog(file); }
        catch (const Json::exception&) { rebuild = true; }
        catch (const std::invalid_argument&) { rebuild = true; }
    }
    archiveIndex = (hasIndex || accessError) && rebuild;

    const int firstUnusedId = NextId();
    auto files = catalog.value("pendingRecovery", std::vector<int>{});
    try
    {
        if (fs::exists(this->directory))
        {
            for (const auto& entry : fs::directory_iterator(this->directory))
            {
                const auto& path = entry.path();
                const bool deleting = path.extension() == L".deleting";
                const auto id = StoredImageId(deleting ? path.stem() : path.filename());
                if (!id || entry.is_directory()) continue;
                ReserveThrough(*id);
                if (deleting)
                {
                    const bool restore = rebuild || std::find(files.begin(), files.end(), *id) != files.end() ||
                        CatalogContains(List(), *id);
                    const bool resolved = restore
                        ? MoveFileExW(path.c_str(), File(*id).c_str(), MOVEFILE_WRITE_THROUGH)
                        : DeleteFileW(path.c_str());
                    if (!resolved)
                        recoveryNotice = "Tea could not finish a previous image deletion. Check the image folder for the file.";
                    if (!restore) continue;
                }
                // interrupted imports publish the png using an id the saved catalog has yet to allocate
                if ((rebuild || *id >= firstUnusedId) && *id < (std::numeric_limits<int>::max)()) files.push_back(*id);
            }
        }
    }
    catch (const fs::filesystem_error&)
    {
        recoveryNotice = "Tea could not read part of the image folder. Check its permissions before changing the library.";
    }
    if (!files.empty())
    {
        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());
        std::vector<int> pending;
        try
        {
            const ComApartment apartment;
            const auto factory = Imaging();
            for (int id : files)
            {
                try
                {
                    const auto frame = OpenImage(factory.Get(), File(id));
                    UINT width = 0, height = 0;
                    Check(frame->GetSize(&width, &height), "the image size could not be read");
                    ValidateSize(width, height);
                    catalog["images"].push_back({{"id", id}, {"name", "recovered image " + std::to_string(id)},
                        {"width", width}, {"height", height}});
                }
                catch (const std::runtime_error&) { pending.push_back(id); }
                catch (const std::invalid_argument&) { pending.push_back(id); }
            }
        }
        catch (const std::runtime_error&) { pending = files; }
        auto& images = catalog.at("images");
        std::sort(images.begin(), images.end(), [](const Json& a, const Json& b) { return a.at("id") < b.at("id"); });
        if (pending.empty()) catalog.erase("pendingRecovery");
        else catalog["pendingRecovery"] = std::move(pending);
    }
    if (recoveryNotice.empty() && (archiveIndex || !files.empty()))
        recoveryNotice = catalog.contains("pendingRecovery") || List().empty()
            ? "Some images could not be recovered. The files are still in the image folder. Tea retries recovery at startup."
            : "Tea recovered files that were missing from the image library. The recovered images have new names.";
}

void ImageLibrary::ReserveThrough(int id)
{
    if (id >= NextId()) catalog["nextId"] = id < INT_MAX ? id + 1 : INT_MAX;
}

bool ImageLibrary::Has(int id) const
{
    std::error_code error;
    return CatalogContains(List(), id) && fs::is_regular_file(File(id), error);
}

fs::path ImageLibrary::File(int id) const { return directory / (std::to_wstring(id) + L".png"); }
fs::path ImageLibrary::PendingDeletion(int id) const { return File(id).wstring() + L".deleting"; }

void ImageLibrary::Save(Json next)
{
    const auto temporary = directory / L"index.tmp";
    const auto file = directory / L"index.json";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << next.dump(2);
    output.close();
    if (!output) throw std::runtime_error("the image library could not be saved");
    if (archiveIndex) tea::PreserveUnreadableFile(file);
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("the image library could not be saved");
    archiveIndex = false;
    catalog = std::move(next);
}

void ImageLibrary::Import(HWND owner)
{
    ComPtr<IFileOpenDialog> dialog;
    const auto error = "the image file picker could not be opened";
    Check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)), error);
    const COMDLG_FILTERSPEC types[] = {{L"images", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff"}};
    Check(dialog->SetFileTypes(1, types), error);
    Check(dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST), error);
    const HRESULT result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    Check(result, error);
    ComPtr<IShellItem> item;
    Check(dialog->GetResult(&item), error);
    PWSTR path = nullptr;
    Check(item->GetDisplayName(SIGDN_FILESYSPATH, &path), "the selected file could not be opened");
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> filename(path, CoTaskMemFree);
    Import(fs::path(filename.get()));
}

void ImageLibrary::Import(const fs::path& file) { Add(file.stem().u8string(), ReadImage(file)); }

void ImageLibrary::Add(std::string name, const Image& image)
{
    ValidateName(name);
    ValidateSize(image.width, image.height);
    ValidateTemplate(image);
    const int id = catalog.at("nextId").get<int>();
    if (id == (std::numeric_limits<int>::max)()) throw std::runtime_error("the image library is full");
    auto next = catalog;
    next["nextId"] = id + 1;
    next["images"].push_back({{"id", id}, {"name", std::move(name)}, {"width", image.width}, {"height", image.height}});
    fs::create_directories(directory);
    const auto path = File(id);
    const auto temporary = directory / L"image.tmp";
    bool published = false;
    try
    {
        const ComApartment apartment;
        const auto factory = Imaging();
        ComPtr<IWICBitmap> bitmap;
        auto* data = reinterpret_cast<BYTE*>(const_cast<Pixel*>(image.pixels.data()));
        Check(factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppBGRA,
            image.width * sizeof(Pixel), static_cast<UINT>(image.pixels.size() * sizeof(Pixel)), data, &bitmap),
            "the image could not be prepared");
        ComPtr<IWICStream> stream;
        Check(factory->CreateStream(&stream), "the image could not be saved");
        Check(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE), "the image file could not be opened for writing");
        WritePng(factory.Get(), stream.Get(), bitmap.Get());
        stream.Reset();
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("the image could not be published without replacing an existing file");
        published = true;
        Save(std::move(next));
    }
    catch (...)
    {
        std::error_code ignored;
        if (published) fs::remove(path, ignored);
        else fs::remove(temporary, ignored);
        throw;
    }
}

void ImageLibrary::Rename(int id, std::string name)
{
    ValidateName(name);
    auto next = catalog;
    auto& entries = next.at("images");
    const auto entry = std::find_if(entries.begin(), entries.end(), [id](const Json& image) { return WithId(image, id); });
    if (entry == entries.end()) throw std::invalid_argument("the image could not be found");
    (*entry)["name"] = std::move(name);
    Save(std::move(next));
}

void ImageLibrary::Remove(int id)
{
    auto next = catalog;
    auto& entries = next.at("images");
    const auto entry = std::find_if(entries.begin(), entries.end(), [id](const Json& image) { return WithId(image, id); });
    if (entry == entries.end()) throw std::invalid_argument("the image could not be found");
    entries.erase(entry);
    const auto path = File(id), pending = PendingDeletion(id);
    const auto appearance = GetFileAttributesW(path.c_str());
    const auto error = "the image file could not be deleted; check its permissions and close any app using it";
    if (appearance != INVALID_FILE_ATTRIBUTES && (appearance & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)))
        throw std::runtime_error(error);
    bool staged = MoveFileExW(path.c_str(), pending.c_str(), MOVEFILE_WRITE_THROUGH);
    if (!staged)
    {
        const auto failure = GetLastError();
        if (failure != ERROR_FILE_NOT_FOUND && failure != ERROR_PATH_NOT_FOUND) throw std::runtime_error(error);
        const auto attributes = GetFileAttributesW(pending.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            if (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error(error);
            staged = true;
        }
        else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
            throw std::runtime_error(error);
    }

    const auto previous = catalog;
    try { Save(std::move(next)); }
    catch (...)
    {
        if (staged) MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
        throw;
    }
    if (!staged || DeleteFileW(pending.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return;
    Save(previous);
    MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
    throw std::runtime_error(error);
}

Image ImageLibrary::Load(int id) const { return ReadImage(File(id)); }

std::string ImageLibrary::Preview(int id) const
{
    if (!Has(id)) throw std::invalid_argument("the image could not be found");
    const ComApartment apartment;
    const auto factory = Imaging();
    const auto frame = OpenImage(factory.Get(), File(id));
    UINT width = 0, height = 0;
    const auto error = "the image preview could not be opened";
    Check(frame->GetSize(&width, &height), error);
    ValidateSize(width, height);
    const UINT side = std::max({width, height, 160u});
    ComPtr<IWICBitmapScaler> scaled;
    Check(factory->CreateBitmapScaler(&scaled), error);
    Check(scaled->Initialize(frame.Get(), std::max(1u, width * 160 / side), std::max(1u, height * 160 / side),
        WICBitmapInterpolationModeFant), error);
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), error);
    WritePng(factory.Get(), stream.Get(), scaled.Get());
    STATSTG info{};
    Check(stream->Stat(&info, STATFLAG_NONAME), error);
    Check(stream->Seek({}, STREAM_SEEK_SET, nullptr), error);
    std::string bytes(info.cbSize.LowPart, '\0');
    ULONG read = 0;
    Check(stream->Read(bytes.data(), info.cbSize.LowPart, &read), error);
    if (read != bytes.size()) throw std::runtime_error(error);
    return bytes;
}
}
