#include "rigExec/rigEvaluator.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/js/json.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/usd/primRange.h"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <tuple>
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
        std::map<std::string,SdfPath> channels=nodes;
        for(const auto &entry:nodes) {
            SdfPathVector targets;
            if(stage->GetPrimAtPath(entry.second).GetRelationship(TfToken("blender:channelControl")).GetTargets(&targets) && targets.size()==1)
                channels[entry.first]=targets.front();
        }
        auto points=[&](const std::string &id) {
            auto pose=evaluator.Evaluate(UsdTimeCode::Default());
            for(const auto &error:pose.diagnostics) std::cerr<<error<<'\n'; CHECK(pose.valid);
            auto it=pose.movedProperties.find(meshes.at(id).AppendProperty(TfToken("points")));
            CHECK(it!=pose.movedProperties.end()); return it->second.Get<VtVec3fArray>();
        };
        if(argc==4) {
            auto rest=points("Body"); CHECK(rest.size()==4); CHECK((rest[2]-GfVec3f(1,1,0)).GetLength()<1e-5);
            auto control=stage->GetPrimAtPath(channels.at("Driver"));
            CHECK(control.GetAttribute(TfToken("avars:tx")).Set(2.0));
            auto moved=points("Body");
            CHECK((moved[0]-GfVec3f(0,0,0)).GetLength()<1e-5);
            CHECK((moved[2]-GfVec3f(3,1,0)).GetLength()<1e-5);
            CHECK((moved[3]-GfVec3f(1,1,0)).GetLength()<1e-5);
            CHECK(control.GetAttribute(TfToken("avars:tx")).Set(0.0));
            CHECK((points("Body")[2]-rest[2]).GetLength()<1e-5);
            // A synthetic Blender armature exercises the converter's native
            // SpaceSwitch controls independently of the skin fixture's plain
            // RigExec joints. Repeated edits keep one evaluator compiled.
            std::ifstream input(argv[3]); CHECK(input);
            auto synthetic=JsParseStream(input).GetJsObject();
            auto sourceNodes=synthetic.at("nodes").GetJsArray();
            for(auto &item:sourceNodes) {
                auto node=item.GetJsObject();
                if(node.at("kind").GetString()=="joint") node["armature"]=JsValue("Armature");
                item=JsValue(node);
            }
            synthetic["nodes"]=JsValue(sourceNodes);
            const auto originalConstraints=synthetic.at("constraints").GetJsArray();
            synthetic["constraints"]=JsValue(JsArray{});
            auto nativeLayer=SdfLayer::CreateAnonymous("native-controls.blendrig");
            CHECK(nativeLayer->ImportFromString(JsWriteToString(JsValue(synthetic))));
            auto nativeStage=UsdStage::Open(nativeLayer); CHECK(nativeStage);
            nativeStage->SetEditTarget(nativeStage->GetSessionLayer());
            std::map<std::string,SdfPath> bonePaths,controlPaths;
            for(const auto &prim:nativeStage->Traverse()) {
                if(prim.GetTypeName()!=TfToken("RigExecJoint"))continue;
                auto id=prim.GetCustomDataByKey(TfToken("blender:id"));
                if(!id.IsHolding<std::string>())continue;
                SdfPathVector targets;
                CHECK(prim.GetRelationship(TfToken("blender:channelControl")).GetTargets(&targets));
                CHECK(targets.size()==1);
                bonePaths[id.Get<std::string>()]=prim.GetPath();
                controlPaths[id.Get<std::string>()]=targets.front();
            }
            CHECK(bonePaths.size()==2);
            rigExec::RigExecRigEvaluator nativeRig(nativeStage,SdfPath("/Rig"));
            errors.clear(); CHECK(nativeRig.Compile(&errors)); CHECK(errors.empty());
            const auto sameFrame=[&](const rigExec::RigExecPointFrame &a,const rigExec::RigExecPointFrame &b) {
                for(int i=0;i<4;++i)CHECK((a.points[i]-b.points[i]).GetLength()<1e-6);
            };
            for(const auto &edit:std::vector<std::tuple<std::string,std::string,double>>{
                {"Armature","tx",1.25},{"Armature","rz",25.0},{"Root","tx",2.0},
                {"Root","ry",-20.0},{"Root","sx",1.3},{"Tip","tz",0.4},
                {"Tip","rx",35.0},{"Tip","sy",0.8}}) {
                SdfPath path;
                if(std::get<0>(edit)=="Armature") {
                    for(const auto &prim:nativeStage->Traverse())
                        if(prim.GetCustomDataByKey(TfToken("blender:id"))==VtValue(std::string("Armature"))) path=prim.GetPath();
                } else path=controlPaths.at(std::get<0>(edit));
                CHECK(nativeStage->GetPrimAtPath(path).GetAttribute(TfToken("avars:"+std::get<1>(edit))).Set(std::get<2>(edit)));
                auto pose=nativeRig.Evaluate(UsdTimeCode::Default()); CHECK(pose.valid);
                for(const auto &name:{"Root","Tip"})
                    sameFrame(pose.controlFrames.at(controlPaths.at(name)),pose.jointFramesBase.at(bonePaths.at(name)));
            }
            auto constrained=synthetic;
            auto follow=originalConstraints.front().GetJsObject();
            follow["owner"]=JsValue("Root");
            constrained["constraints"]=JsValue(JsArray{JsValue(follow)});
            auto constrainedLayer=SdfLayer::CreateAnonymous("constrained-parent.blendrig");
            CHECK(constrainedLayer->ImportFromString(JsWriteToString(JsValue(constrained))));
            auto constrainedStage=UsdStage::Open(constrainedLayer); CHECK(constrainedStage);
            constrainedStage->SetEditTarget(constrainedStage->GetSessionLayer());
            rigExec::RigExecRigEvaluator constrainedRig(constrainedStage,SdfPath("/Rig"));
            errors.clear(); CHECK(constrainedRig.Compile(&errors)); CHECK(errors.empty());
            SdfPath driverPath;
            for(const auto &prim:constrainedStage->Traverse())
                if(prim.GetCustomDataByKey(TfToken("blender:id"))==VtValue(std::string("Driver")))driverPath=prim.GetPath();
            CHECK(!driverPath.IsEmpty());
            CHECK(constrainedStage->GetPrimAtPath(driverPath).GetAttribute(TfToken("avars:tx")).Set(5.0));
            CHECK(constrainedStage->GetPrimAtPath(controlPaths.at("Tip")).GetAttribute(TfToken("avars:ry")).Set(20.0));
            auto constrainedPose=constrainedRig.Evaluate(UsdTimeCode::Default()); CHECK(constrainedPose.valid);
            sameFrame(constrainedPose.controlFrames.at(controlPaths.at("Tip")),constrainedPose.jointFramesBase.at(bonePaths.at("Tip")));
            CHECK((constrainedPose.controlFrames.at(controlPaths.at("Root")).Origin()-
                   constrainedPose.jointFramesBase.at(bonePaths.at("Root")).Origin()).GetLength()>1.0);
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
                    auto attr=stage->GetPrimAtPath(channels.at(e.at("id").GetString())).GetAttribute(TfToken("avars:"+e.at("channel").GetString()));
                    double value;CHECK(attr.Get(&value));initialChannels.emplace(attr.GetPath(),value);
                }
            for(const auto &data:reference.at("poses").GetJsArray()) {
                const auto &pose=data.GetJsObject();
                if(incremental)for(const auto &entry:initialChannels)CHECK(stage->GetAttributeAtPath(entry.first).Set(entry.second));
                else stage->GetSessionLayer()->Clear();
                for(const auto &edit:pose.at("edits").GetJsArray()) {
                    const auto &e=edit.GetJsObject();
                    auto prim=stage->GetPrimAtPath(channels.at(e.at("id").GetString()));
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
                    auto prim=stage->GetPrimAtPath(channels.at(e.at("id").GetString()));
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
