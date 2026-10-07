#include "fileFormat.h"
#include "translator.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/registryManager.h"
#include "pxr/usd/ar/asset.h"
#include "pxr/usd/ar/resolvedPath.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/sdf/layer.h"
#include <filesystem>
#include <stdexcept>

PXR_NAMESPACE_OPEN_SCOPE
TF_REGISTRY_FUNCTION(TfType) { SDF_DEFINE_FILE_FORMAT(UsdBlenderRigFileFormat, SdfFileFormat); }
UsdBlenderRigFileFormat::UsdBlenderRigFileFormat()
    : SdfFileFormat(TfToken("blenderRig"), TfToken("1.0"), TfToken("usd"), TfToken("blend")) {}
bool UsdBlenderRigFileFormat::CanRead(const std::string &path) const {
    auto asset=ArGetResolver().OpenAsset(ArResolvedPath(path));
    if(!asset) return false;
    unsigned char header[16]={}; const size_t n=asset->Read(header,sizeof(header),0);
    if(std::filesystem::path(path).extension()==".blendrig")
        return n && (header[0]=='{' || header[0]==' ' || header[0]=='\n');
    // Blender reads its own current/legacy headers and compressed streams.
    return (n>=7 && std::string(reinterpret_cast<char *>(header),7)=="BLENDER") ||
        (n>=2 && header[0]==0x1f && header[1]==0x8b) ||
        (n>=4 && header[0]==0x28 && header[1]==0xb5 && header[2]==0x2f && header[3]==0xfd);
}
bool UsdBlenderRigFileFormat::_Read(SdfLayer *layer,const std::string &data,
                                   const std::string &source,bool metadataOnly) const {
    try {
        bool strict=false;
        for(const auto &arg:layer->GetFileFormatArguments()) {
            if(arg.first!="strict" || (arg.second!="0" && arg.second!="1"))
                throw std::runtime_error("supported argument: strict=0|1");
            strict=arg.second=="1";
        }
        auto result=usdBlenderRig::Translate(data,source,metadataOnly,strict);
        layer->TransferContent(result);
        return true;
    } catch(const std::exception &e) {
        TF_RUNTIME_ERROR("Blender rig translation failed for %s: %s",source.c_str(),e.what());
        return false;
    }
}
bool UsdBlenderRigFileFormat::Read(SdfLayer *layer,const std::string &path,bool metadataOnly) const {
    try {
        if(std::filesystem::path(path).extension()==".blend")
            return _Read(layer,usdBlenderRig::Extract(path),path,metadataOnly);
        auto asset=ArGetResolver().OpenAsset(ArResolvedPath(path));
        if(!asset) throw std::runtime_error("cannot open snapshot");
        if(asset->GetSize()>512*1024*1024) throw std::runtime_error("snapshot exceeds 512 MiB");
        auto data=asset->GetBuffer();
        if(!data) throw std::runtime_error("cannot buffer snapshot");
        return _Read(layer,std::string(data.get(),asset->GetSize()),path,metadataOnly);
    } catch(const std::exception &e) {
        TF_RUNTIME_ERROR("Cannot read Blender rig %s: %s",path.c_str(),e.what()); return false;
    }
}
bool UsdBlenderRigFileFormat::ReadFromString(SdfLayer *layer,const std::string &data) const {
    return _Read(layer,data,{},false);
}
bool UsdBlenderRigFileFormat::WriteToString(const SdfLayer &layer,std::string *text,const std::string &comment) const {
    return SdfFileFormat::FindById(TfToken("usda"))->WriteToString(layer,text,comment);
}
bool UsdBlenderRigFileFormat::WriteToStream(const SdfSpecHandle &spec,std::ostream &out,size_t indent) const {
    return SdfFileFormat::FindById(TfToken("usda"))->WriteToStream(spec,out,indent);
}
std::set<std::string> UsdBlenderRigFileFormat::GetExternalAssetDependencies(const SdfLayer &layer) const {
    std::set<std::string> result; const auto data=layer.GetCustomLayerData();
    const auto it=data.find("blenderRig:dependencies");
    if(it!=data.end() && it->second.IsHolding<VtStringArray>())
        for(const auto &id:it->second.UncheckedGet<VtStringArray>()) result.insert(id);
    return result;
}
PXR_NAMESPACE_CLOSE_SCOPE
