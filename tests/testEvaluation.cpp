#include "rigExec/rigEvaluator.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/js/json.h"
#include "pxr/usd/usd/primRange.h"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
PXR_NAMESPACE_USING_DIRECTIVE
#define CHECK(x) do { if(!(x)) throw std::runtime_error("check failed: " #x); } while(false)
int main(int argc,char **argv) {
    try {
        CHECK(argc==4 || argc==5);
        PlugRegistry::GetInstance().RegisterPlugins(argv[1]);
        PlugRegistry::GetInstance().RegisterPlugins(argv[2]);
        auto stage=UsdStage::Open(argv[3]); CHECK(stage);
        stage->SetEditTarget(stage->GetSessionLayer());
        rigExec::RigExecRigEvaluator evaluator(stage,SdfPath("/Rig"));
        std::vector<std::string> errors;
        bool compiled=evaluator.Compile(&errors);
        for(const auto &error:errors) std::cerr<<error<<'\n'; CHECK(compiled); CHECK(errors.empty());
        std::map<std::string,SdfPath> nodes,meshes;
        for(const auto &prim:stage->Traverse()) {
            auto id=prim.GetCustomDataByKey(TfToken("blender:id"));
            if(id.IsHolding<std::string>()) {
                if(prim.GetTypeName()==TfToken("Mesh")) meshes[id.Get<std::string>()]=prim.GetPath();
                else nodes[id.Get<std::string>()]=prim.GetPath();
            }
        }
        auto points=[&](const std::string &id) {
            auto pose=evaluator.Evaluate(UsdTimeCode::Default());
            for(const auto &error:pose.diagnostics) std::cerr<<error<<'\n'; CHECK(pose.valid);
            auto it=pose.movedProperties.find(meshes.at(id).AppendProperty(TfToken("points")));
            CHECK(it!=pose.movedProperties.end()); return it->second.Get<VtVec3fArray>();
        };
        if(argc==4) {
            auto rest=points("Body"); CHECK(rest.size()==4); CHECK((rest[2]-GfVec3f(1,1,0)).GetLength()<1e-5);
            auto control=stage->GetPrimAtPath(nodes.at("Driver"));
            CHECK(control.GetAttribute(TfToken("avars:tx")).Set(2.0));
            auto moved=points("Body");
            CHECK((moved[0]-GfVec3f(0,0,0)).GetLength()<1e-5);
            CHECK((moved[2]-GfVec3f(3,1,0)).GetLength()<1e-5);
            CHECK((moved[3]-GfVec3f(1,1,0)).GetLength()<1e-5);
            CHECK(control.GetAttribute(TfToken("avars:tx")).Set(0.0));
            CHECK((points("Body")[2]-rest[2]).GetLength()<1e-5);
            std::cout<<"Native control -> constraint -> linear skin -> reset passed\n";
        } else {
            std::ifstream input(argv[4]); auto reference=JsParseStream(input).GetJsObject();
            std::string mesh=reference.at("mesh").GetString();
            const double tolerance=reference.at("tolerance").GetReal();
            const bool incremental=std::getenv("USDBLENDERRIG_INCREMENTAL_POSES")!=nullptr;
            std::map<SdfPath,double> initialChannels;
            if(incremental)for(const auto &data:reference.at("poses").GetJsArray())
                for(const auto &edit:data.GetJsObject().at("edits").GetJsArray()) {
                    const auto &e=edit.GetJsObject();
                    auto attr=stage->GetPrimAtPath(nodes.at(e.at("id").GetString())).GetAttribute(TfToken("avars:"+e.at("channel").GetString()));
                    double value;CHECK(attr.Get(&value));initialChannels.emplace(attr.GetPath(),value);
                }
            for(const auto &data:reference.at("poses").GetJsArray()) {
                const auto &pose=data.GetJsObject();
                if(incremental)for(const auto &entry:initialChannels)CHECK(stage->GetAttributeAtPath(entry.first).Set(entry.second));
                else stage->GetSessionLayer()->Clear();
                for(const auto &edit:pose.at("edits").GetJsArray()) {
                    const auto &e=edit.GetJsObject();
                    auto prim=stage->GetPrimAtPath(nodes.at(e.at("id").GetString()));
                    CHECK(prim.GetAttribute(TfToken("avars:"+e.at("channel").GetString())).Set(e.at("value").GetReal()));
                }
                JsArray samples;
                if(pose.count("meshes")) samples=pose.at("meshes").GetJsArray();
                else samples.push_back(JsValue(JsObject{{"mesh",JsValue(mesh)},{"points",pose.at("points")}}));
                for(const auto &sample:samples) {
                const auto &record=sample.GetJsObject();
                auto actual=points(record.at("mesh").GetString()); const auto &expected=record.at("points").GetJsArray(); CHECK(actual.size()==expected.size());
                double maximum=0;
                for(size_t i=0;i<actual.size();++i) {
                    const auto &xyz=expected[i].GetJsArray();
                    maximum=std::max(maximum,(GfVec3d(actual[i])-GfVec3d(xyz[0].GetReal(),xyz[1].GetReal(),xyz[2].GetReal())).GetLength());
                }
                std::cout<<pose.at("name").GetString()<<" / "<<record.at("mesh").GetString()<<": max vertex error "<<maximum<<" (tolerance "<<tolerance<<")\n";
                CHECK(maximum<=tolerance);
                }
            }
            if(incremental)std::cout<<"Incremental avar edits match Blender without a layer clear or recompile\n";
            if(reference.count("native_guard_edits")) {
                stage->GetSessionLayer()->Clear();
                for(const auto &edit:reference.at("native_guard_edits").GetJsArray()) {
                    const auto &e=edit.GetJsObject();
                    auto prim=stage->GetPrimAtPath(nodes.at(e.at("id").GetString()));
                    CHECK(prim.GetAttribute(TfToken("avars:"+e.at("channel").GetString())).Set(e.at("value").GetReal()));
                }
                auto guarded=evaluator.Evaluate(UsdTimeCode::Default());CHECK(guarded.valid);
                for(const auto &entry:meshes)for(const auto &point:points(entry.first))
                    for(int axis=0;axis<3;++axis)CHECK(std::isfinite(point[axis]));
                stage->GetSessionLayer()->Clear();
                CHECK(evaluator.Evaluate(UsdTimeCode::Default()).valid);
                std::cout<<"Diagnosed collapsed-stretch guard and recovery passed\n";
            }
            stage->GetSessionLayer()->Clear();
            const auto referencePath=std::filesystem::path(argv[4]);
            const auto output=referencePath.parent_path()/(referencePath.stem()=="reference" ? "converted.usda" : referencePath.stem().string()+"-converted.usda");
            CHECK(stage->GetRootLayer()->Export(output.string()));
            auto native=UsdStage::Open(output.string()); CHECK(native);
            rigExec::RigExecRigEvaluator nativeEvaluator(native,SdfPath("/Rig"));
            CHECK(nativeEvaluator.Compile(&errors)); CHECK(nativeEvaluator.Evaluate(UsdTimeCode::Default()).valid);
            std::cout<<"Source "<<argv[3]<<" matches Blender; native USD reopening passed\n";
        }
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
