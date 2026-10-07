#include "pxr/base/plug/registry.h"
#include "pxr/base/js/json.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/usd/sdf/fileFormat.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdShade/materialBindingAPI.h"
#include "pxr/usd/usdShade/shader.h"
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>
PXR_NAMESPACE_USING_DIRECTIVE
#define CHECK(x) do { if(!(x)) throw std::runtime_error("check failed: " #x); } while(false)
int main(int argc,char **argv) {
    try {
        CHECK(argc==4);
        PlugRegistry::GetInstance().RegisterPlugins(argv[1]);
        PlugRegistry::GetInstance().RegisterPlugins(argv[2]);
        auto format=SdfFileFormat::FindByExtension("blend"); CHECK(format);
        CHECK(SdfFileFormat::FindByExtension("blendrig")==format);
        CHECK(!format->SupportsWriting()); CHECK(!format->SupportsEditing());
        const std::string fixture=std::string(argv[3])+"/rig.blendrig";
        CHECK(format->CanRead(fixture));
        auto layer=SdfLayer::FindOrOpen(fixture); CHECK(layer);
        auto stage=UsdStage::Open(layer); CHECK(stage);
        CHECK(stage->GetDefaultPrim().GetTypeName()==TfToken("RigExecRoot"));
        CHECK(stage->GetTimeCodesPerSecond()==30);
        CHECK(layer->GetCustomLayerData()["blenderRig:complete"].Get<bool>());
        int joints=0,skins=0,constraints=0,meshes=0;
        for(const auto &prim:stage->Traverse()) {
            joints+=prim.GetTypeName()==TfToken("RigExecJoint");
            skins+=prim.GetTypeName()==TfToken("RigExecSkinMover");
            constraints+=prim.GetTypeName()==TfToken("RigExecPositionConstraint");
            meshes+=prim.GetTypeName()==TfToken("Mesh");
            if(prim.GetTypeName()==TfToken("Mesh")) {
                auto uv=UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken("st")); CHECK(uv);
                CHECK(uv.GetInterpolation()==UsdGeomTokens->faceVarying);
                CHECK(UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial());
            }
        }
        CHECK(joints==2); CHECK(skins==1); CHECK(constraints==1); CHECK(meshes==1);
        CHECK(SdfLayer::FindOrOpen(fixture,{{"strict","1"}}));
        std::ifstream input(fixture); std::stringstream buffer; buffer<<input.rdbuf();
        auto json=JsParseString(buffer.str());
        auto anonymous=SdfLayer::CreateAnonymous("rig.blendrig");
        CHECK(anonymous->ImportFromString(buffer.str()));
        std::string original; CHECK(anonymous->ExportToString(&original)); CHECK(original.find("#usda 1.0")==0);
        auto fail=[&](const JsValue &bad) {
            TfErrorMark errors;
            CHECK(!anonymous->ImportFromString(JsWriteToString(bad)));
            CHECK(!errors.IsClean()); errors.Clear();
            std::string after; CHECK(anonymous->ExportToString(&after)); CHECK(after==original);
        };
        auto bad=json.GetJsObject(); bad["version"]=JsValue(2); fail(JsValue(bad));
        bad=json.GetJsObject(); auto nodes=bad["nodes"].GetJsArray(); nodes.push_back(nodes[0]); bad["nodes"]=JsValue(nodes); fail(JsValue(bad));
        bad=json.GetJsObject(); nodes=bad["nodes"].GetJsArray(); auto first=nodes[0].GetJsObject(); first["parent"]=first["id"]; nodes[0]=JsValue(first); bad["nodes"]=JsValue(nodes); fail(JsValue(bad));
        bad=json.GetJsObject(); auto meshesData=bad["meshes"].GetJsArray(); auto mesh=meshesData[0].GetJsObject(); mesh["indices"]=JsValue(JsArray{JsValue(999)}); meshesData[0]=JsValue(mesh); bad["meshes"]=JsValue(meshesData); fail(JsValue(bad));
        bad=json.GetJsObject(); auto cons=bad["constraints"].GetJsArray(); auto con=cons[0].GetJsObject(); con["source"]=con["owner"]; cons[0]=JsValue(con); bad["constraints"]=JsValue(cons); fail(JsValue(bad));
        // Picker links obey the same transactional import contract as graph data.
        const auto controlId=json.GetJsObject().at("nodes").GetJsArray()[0].GetJsObject().at("id");
        JsObject button{{"label",JsValue("Root")},{"controls",JsValue(JsArray{controlId})}};
        JsObject page{{"name",JsValue("Main")},{"buttons",JsValue(JsArray{JsValue(button)})}};
        JsObject picker{{"owner",controlId},{"name",JsValue("Character")},
                        {"source_kind",JsValue("Rigify control collections")},
                        {"pages",JsValue(JsArray{JsValue(page)})}};
        auto withPicker=json.GetJsObject(); withPicker["pickers"]=JsValue(JsArray{JsValue(picker)});
        auto pickerLayer=SdfLayer::CreateAnonymous("picker.blendrig",{{"strict","1"}});
        CHECK(pickerLayer->ImportFromString(JsWriteToString(JsValue(withPicker))));
        auto pickerStage=UsdStage::Open(pickerLayer); int pickerButtons=0;
        for(const auto &prim:pickerStage->Traverse()) {
            if(prim.GetTypeName()!=TfToken("RigExecPickerButton"))continue;
            ++pickerButtons; SdfPathVector targets;
            CHECK(prim.GetRelationship(TfToken("rigExec:picker:controls")).GetTargets(&targets));
            CHECK(targets.size()==1); CHECK(pickerStage->GetPrimAtPath(targets[0]));
        }
        CHECK(pickerButtons==1);
        // Full source identities remain metadata, never inflated prim names.
        // Sanitization, truncation, reserved helper names and duplicate labels
        // must stay distinct and independent of source-array ordering.
        auto named=json.GetJsObject();auto namedNodes=named["nodes"].GetJsArray();
        auto templateNode=namedNodes[0].GetJsObject();
        for(const auto &label:std::vector<std::string>{"root.L","root-L","root_L","EditorFrame","Display",std::string(100,'x'),std::string(100,'x')+"y","雪"}) {
            auto node=templateNode;
            node["id"]=JsValue("long-source-identity/"+label);
            node["name"]=JsValue(label);node["parent"]=JsValue("");
            namedNodes.push_back(JsValue(node));
        }
        named["nodes"]=JsValue(namedNodes);
        auto readNames=[&](const JsObject &data) {
            auto layer=SdfLayer::CreateAnonymous("names.blendrig");
            CHECK(layer->ImportFromString(JsWriteToString(JsValue(data))));
            auto stage=UsdStage::Open(layer);std::map<std::string,SdfPath> paths;
            for(const auto &prim:stage->Traverse()) {
                auto id=prim.GetCustomDataByKey(TfToken("blender:id"));
                if(prim.GetTypeName()!=TfToken("RigExecControl") || !id.IsHolding<std::string>())continue;
                CHECK(prim.GetName().GetString().size()<=45);
                CHECK(paths.emplace(id.Get<std::string>(),prim.GetPath()).second);
            }
            return paths;
        };
        const auto compact=readNames(named);
        CHECK(compact.size()>=8);
        std::reverse(namedNodes.begin(),namedNodes.end());named["nodes"]=JsValue(namedNodes);
        CHECK(readNames(named)==compact);
        bad=withPicker; bad["pickers"]=JsValue(JsArray{JsValue(picker),JsValue(picker)}); fail(JsValue(bad));
        auto missingOwner=picker; missingOwner["owner"]=JsValue("missing");
        bad=withPicker; bad["pickers"]=JsValue(JsArray{JsValue(missingOwner)}); fail(JsValue(bad));
        for(const auto &targets:JsArray{JsValue(JsArray{JsValue("missing")}),JsValue(JsArray{controlId,controlId})}) {
            auto invalidButton=button; invalidButton["controls"]=targets;
            auto invalidPage=page; invalidPage["buttons"]=JsValue(JsArray{JsValue(invalidButton)});
            auto invalidPicker=picker; invalidPicker["pages"]=JsValue(JsArray{JsValue(invalidPage)});
            bad=withPicker; bad["pickers"]=JsValue(JsArray{JsValue(invalidPicker)}); fail(JsValue(bad));
        }
        auto unsupported=json.GetJsObject(); cons=unsupported["constraints"].GetJsArray(); con=cons[0].GetJsObject(); con["type"]=JsValue("SCRIPTED"); cons[0]=JsValue(con); unsupported["constraints"]=JsValue(cons);
        CHECK(anonymous->ImportFromString(JsWriteToString(JsValue(unsupported))));
        CHECK(!anonymous->GetCustomLayerData()["blenderRig:complete"].Get<bool>());
        auto strict=SdfLayer::CreateAnonymous("strict.blendrig",{{"strict","1"}});
        { TfErrorMark errors; CHECK(!strict->ImportFromString(JsWriteToString(JsValue(unsupported)))); errors.Clear(); CHECK(strict->IsEmpty()); }
        auto unknown=SdfLayer::CreateAnonymous("bad.blendrig",{{"invalid","1"}});
        { TfErrorMark errors; CHECK(!unknown->ImportFromString(buffer.str())); errors.Clear(); }
        // Anonymous USDA serialization can be reopened without the source reader.
        auto usd=SdfLayer::CreateAnonymous("native.usda"); CHECK(usd->ImportFromString(original));
        CHECK(UsdStage::Open(usd)->GetDefaultPrim().GetTypeName()==TfToken("RigExecRoot"));
        auto textured=json.GetJsObject();
        textured["source"]=JsValue(std::filesystem::absolute(fixture).string());
        auto mats=textured["materials"].GetJsArray(); auto mat=mats[0].GetJsObject();
        mat["texture"]=JsValue("textures/checker.ppm"); mats[0]=JsValue(mat); textured["materials"]=JsValue(mats);
        CHECK(strict->ImportFromString(JsWriteToString(JsValue(textured))));
        CHECK(strict->GetExternalAssetDependencies().size()==1);
        auto textureStage=UsdStage::Open(strict); CHECK(textureStage);
        auto texture=UsdShadeShader(textureStage->GetPrimAtPath(SdfPath("/Rig/Materials/Blue/Texture")));
        SdfAssetPath asset; CHECK(texture.GetInput(TfToken("file")).Get(&asset));
        CHECK(std::filesystem::is_regular_file(asset.GetAssetPath()));
        CHECK(texture.GetInput(TfToken("st")).HasConnectedSource());
        { TfErrorMark mark;
            CHECK(!format->WriteToFile(*layer,fixture,"",{})); CHECK(mark.IsClean());
        }
        std::cout<<"Discovery, strict diagnostics, cycles, atomic failure and USDA serialization passed\n";
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
