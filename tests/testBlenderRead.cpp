#include "pxr/base/plug/registry.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/usd/sdf/fileFormat.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/prim.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
PXR_NAMESPACE_USING_DIRECTIVE
#define CHECK(x) do { if(!(x)) throw std::runtime_error("check failed: " #x); } while(false)
std::string Read(const std::string &path) {
    std::ifstream input(path,std::ios::binary); CHECK(input);
    std::stringstream buffer; buffer<<input.rdbuf(); return buffer.str();
}
int main(int argc,char **argv) {
    try {
        CHECK(argc==4);
        PlugRegistry::GetInstance().RegisterPlugins(argv[1]);
        PlugRegistry::GetInstance().RegisterPlugins(argv[2]);
        auto format=SdfFileFormat::FindByExtension("blend"); CHECK(format);
        const std::string base=argv[3],source=base+"/rig.blend",unsupported=base+"/unsupported.blend";
        const auto bytes=Read(source); CHECK(format->CanRead(source));
        CHECK(SdfLayer::FindOrOpen(base+"/strict.blend",{{"strict","1"}}));
        { TfErrorMark errors; CHECK(!SdfLayer::FindOrOpen(source,{{"strict","1"}})); CHECK(!errors.IsClean()); errors.Clear(); }
        auto layer=SdfLayer::CreateAnonymous("atomic.usda");
        CHECK(format->Read(get_pointer(layer),source,false));
        std::string before; CHECK(layer->ExportToString(&before));
        auto rejected=[&](const std::string &path) {
            TfErrorMark errors;
            CHECK(!format->Read(get_pointer(layer),path,false)); CHECK(!errors.IsClean()); errors.Clear();
            std::string after; CHECK(layer->ExportToString(&after)); CHECK(before==after);
        };
        rejected(base+"/missing.blend");
        auto metadata=SdfLayer::CreateAnonymous("metadata.usda");
        CHECK(format->Read(get_pointer(metadata),source,true));
        CHECK(!metadata->GetCustomLayerData()["blenderRig:complete"].Get<bool>());
        CHECK(UsdStage::Open(metadata)->GetDefaultPrim().GetChildren().empty());
        auto incomplete=SdfLayer::FindOrOpen(unsupported); CHECK(incomplete);
        CHECK(!incomplete->GetCustomLayerData()["blenderRig:complete"].Get<bool>());
        { TfErrorMark errors; CHECK(!SdfLayer::FindOrOpen(unsupported,{{"strict","1"}})); CHECK(!errors.IsClean()); errors.Clear(); }
        CHECK(!std::filesystem::exists(base+"/script-executed"));
        const char *old=std::getenv("USDBLENDERRIG_BLENDER"); const std::string original=old ? old : "";
#ifdef _WIN32
        _putenv_s("USDBLENDERRIG_BLENDER","Z:\\missing-blender.exe");
#else
        setenv("USDBLENDERRIG_BLENDER","/missing-blender",1);
#endif
        rejected(source);
#ifdef _WIN32
        _putenv_s("USDBLENDERRIG_BLENDER",original.c_str());
#else
        if(old) setenv("USDBLENDERRIG_BLENDER",original.c_str(),1); else unsetenv("USDBLENDERRIG_BLENDER");
#endif
        CHECK(Read(source)==bytes);
        std::cout<<"Compressed header, strict read, metadata, atomic extraction failure, inert scripts and unchanged source passed\n";
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
