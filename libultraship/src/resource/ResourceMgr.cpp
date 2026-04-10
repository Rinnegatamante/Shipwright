#include "ResourceMgr.h"
#include <spdlog/spdlog.h>
#include "OtrFile.h"
#include "Archive.h"
#include "GameVersions.h"
#include <algorithm>
#include <thread>
#include <Utils/StringHelper.h>
#include <StormLib.h>

#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include "xxhash_utils.h"

#ifdef __vita__
#include <vitasdk.h>
#endif

namespace Ship {

ResourceMgr::ResourceMgr(std::shared_ptr<Window> context, const std::string& mainPath, const std::string& patchesPath,
                         const std::unordered_set<uint32_t>& validHashes)
    : mContext(context) {
    mResourceLoader = std::make_shared<ResourceLoader>(context);
    mArchive = std::make_shared<Archive>(mainPath, patchesPath, validHashes, false);
}

ResourceMgr::ResourceMgr(std::shared_ptr<Window> context, const std::vector<std::string>& otrFiles,
                         const std::unordered_set<uint32_t>& validHashes)
    : mContext(context) {
    mResourceLoader = std::make_shared<ResourceLoader>(context);
    mArchive = std::make_shared<Archive>(otrFiles, validHashes, false);
}

ResourceMgr::~ResourceMgr() {
    SPDLOG_INFO("destruct ResourceMgr");
}

bool ResourceMgr::DidLoadSuccessfully() {
    return mArchive != nullptr && mArchive->IsMainMPQValid();
}

std::shared_ptr<OtrFile> ResourceMgr::LoadFileProcess(const std::string& fileToLoad) {
    auto file = mArchive->LoadFile(fileToLoad, true);
    if (file != nullptr) {
        SPDLOG_TRACE("Loaded File {} on ResourceMgr", file->Path);
    } else {
        SPDLOG_WARN("Could not load File {} in ResourceMgr", fileToLoad);
    }
    return file;
}

std::shared_ptr<Resource> ResourceMgr::LoadResourceProcess(const std::string& fileToLoad, uint64_t hash) {
    if (!hash) {
        if (OtrSignatureCheck(fileToLoad.c_str())) {
            auto newFilePath = fileToLoad.substr(7);
            return LoadResourceProcess(newFilePath, hash);
        }
        
        hash = XXH3_64bits(fileToLoad.c_str(), fileToLoad.size());
        auto cacheCheck = GetCachedResource(hash);
        if (cacheCheck != nullptr) {
            return cacheCheck;
        }
    }

    auto file = LoadFileProcess(fileToLoad);
    auto resource = GetResourceLoader()->LoadResource(file);
    
    mResourceCache[hash] = resource;

    if (resource != nullptr) {
        SPDLOG_TRACE("Loaded Resource {} on ResourceMgr", fileToLoad);
    } else {
        SPDLOG_WARN("Resource load FAILED {} on ResourceMgr", fileToLoad);
    }

    return resource;
}

std::vector<uint32_t> ResourceMgr::GetGameVersions() {
    return mArchive->GetGameVersions();
}

void ResourceMgr::PushGameVersion(uint32_t newGameVersion) {
    mArchive->PushGameVersion(newGameVersion);
}

std::shared_ptr<OtrFile> ResourceMgr::LoadFile(const std::string& filePath) {
    return LoadFileProcess(filePath);
}

std::shared_ptr<Resource> ResourceMgr::LoadResourceAsync(const std::string& filePath) {
    if (OtrSignatureCheck(filePath.c_str())) {
        auto newFilePath = filePath.substr(7);
        return LoadResourceAsync(newFilePath);
    }

    uint64_t hash = XXH3_64bits(filePath.c_str(), filePath.size());
    auto cacheCheck = GetCachedResource(hash);
    if (cacheCheck) {
        return cacheCheck;
    }

    return LoadResourceProcess(filePath, hash);
}

std::shared_ptr<Resource> ResourceMgr::LoadResource(const std::string& filePath) {
    return LoadResourceAsync(filePath);
}

std::shared_ptr<Resource> ResourceMgr::GetCachedResource(uint64_t hash) {
    //const std::lock_guard<std::mutex> lock(mMutex);

    auto resCacheFind = mResourceCache.find(hash);

    if (resCacheFind == mResourceCache.end()) {
        return nullptr;
    }

    if (resCacheFind->second.use_count() <= 0) {
        return nullptr;
    }

    if (resCacheFind->second->IsDirty) {
        return nullptr;
    }

    return resCacheFind->second;
}

std::shared_ptr<Resource> ResourceMgr::GetCachedResource(const std::string& filePath) {
    //const std::lock_guard<std::mutex> lock(mMutex);

    auto resCacheFind = mResourceCache.find(XXH3_64bits(filePath.c_str(), filePath.size()));

    if (resCacheFind == mResourceCache.end()) {
        return nullptr;
    }

    if (resCacheFind->second.use_count() <= 0) {
        return nullptr;
    }

    if (resCacheFind->second->IsDirty) {
        return nullptr;
    }

    return resCacheFind->second;
}

std::shared_ptr<std::vector<std::shared_ptr<Resource>>>
ResourceMgr::LoadDirectoryAsync(const std::string& searchMask) {
    auto fileList = ListFiles(searchMask);
    auto loadedList = std::make_shared<std::vector<std::shared_ptr<Resource>>>();
    for (size_t i = 0; i < fileList->size(); i++) {
        loadedList->push_back(LoadResourceAsync(fileList->operator[](i)));
    }
    return loadedList;
}

std::shared_ptr<std::vector<std::shared_ptr<Resource>>> ResourceMgr::LoadDirectory(const std::string& searchMask) {
    return LoadDirectoryAsync(searchMask);
}

size_t ResourceMgr::DirtyDirectory(const std::string& searchMask) {
    auto fileList = ListFiles(searchMask);
    size_t countDirtied = 0;

    for (size_t i = 0; i < fileList->size(); i++) {
        auto fileName = std::string(fileList->operator[](i));

        // We want to synchronously load the resource here because we don't know if it's in the thread pool queue.
        auto cacheCheck = LoadResource(fileName);
        if (cacheCheck != nullptr) {
            cacheCheck->IsDirty = true;
            countDirtied++;
        }
    }

    return countDirtied;
}

size_t ResourceMgr::UnloadDirectory(const std::string& searchMask) {
    auto fileList = ListFiles(searchMask);
    size_t countUnloaded = 0;

    for (size_t i = 0; i < fileList->size(); i++) {
        auto fileName = std::string(fileList->operator[](i));

        size_t count = UnloadResource(fileName);
        countUnloaded += count;
    }

    return countUnloaded;
}

std::shared_ptr<std::vector<std::string>> ResourceMgr::ListFiles(const std::string& searchMask) {
    auto result = std::make_shared<std::vector<std::string>>();
    auto fileList = mArchive->ListFiles(searchMask);

    for (size_t i = 0; i < fileList->size(); i++) {
        result->push_back(fileList->operator[](i).cFileName);
    }

    return result;
}

const std::string* ResourceMgr::HashToString(uint64_t hash) {
    return mArchive->HashToString(hash);
}

std::shared_ptr<Archive> ResourceMgr::GetArchive() {
    return mArchive;
}

std::shared_ptr<ResourceLoader> ResourceMgr::GetResourceLoader() {
    return mResourceLoader;
}

std::shared_ptr<Window> ResourceMgr::GetContext() {
    return mContext;
}

size_t ResourceMgr::UnloadResource(const std::string& filePath) {
    return mResourceCache.erase(XXH3_64bits(filePath.c_str(), filePath.size()));
}

void ResourceMgr::UnloadAllResources() {
    mResourceCache.clear();
}

bool ResourceMgr::OtrSignatureCheck(const char* fileName) {
    return fileName[0] == '_';
}

} // namespace Ship
