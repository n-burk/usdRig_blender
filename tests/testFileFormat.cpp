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
        // The converter is a file format, never a runtime schema/mover plugin.
        std::ifstream metadata(std::string(argv[1])+"/plugInfo.json"); CHECK(metadata);
        const auto pluginData=JsParseStream(metadata).GetJsObject();
        const auto info=pluginData.at("Plugins").GetJsArray().front().GetJsObject().at("Info").GetJsObject();
        CHECK(info.size()==1); CHECK(info.count("Types"));
        const auto types=info.at("Types").GetJsObject();
        CHECK(types.size()==1); CHECK(types.count("UsdBlenderRigFileFormat"));
        CHECK(!std::filesystem::exists(std::string(argv[1])+"/generatedSchema.usda"));
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
        bool baked=false;
        CHECK(stage->GetDefaultPrim().GetAttribute(TfToken("rigExec:baked")).Get(&baked));
        CHECK(baked);
        CHECK(stage->GetTimeCodesPerSecond()==30);
        CHECK(layer->GetCustomLayerData()["blenderRig:complete"].Get<bool>());
        int joints=0,skins=0,constraints=0,meshes=0;
        for(const auto &prim:stage->Traverse()) {
            CHECK(!prim.HasRelationship(TfToken("blender:editorFrame")));
            CHECK(!prim.HasRelationship(TfToken("blender:control")));
            CHECK(!prim.HasCustomDataKey(TfToken("blender:editableChannels")));
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
        const auto controlId=json.GetJsObject().at("nodes").GetJsArray()[2].GetJsObject().at("id");
        JsObject button{{"label",JsValue("Root")},{"controls",JsValue(JsArray{controlId})}};
        JsObject page{{"name",JsValue("Main")},{"buttons",JsValue(JsArray{JsValue(button)})}};
        JsObject picker{{"owner",controlId},{"name",JsValue("Character")},
                        {"source_kind",JsValue("Rigify control collections")},
                        {"pages",JsValue(JsArray{JsValue(page)})}};
        auto withPicker=json.GetJsObject(); withPicker["pickers"]=JsValue(JsArray{JsValue(picker)});
        auto pickerNodes=withPicker["nodes"].GetJsArray();
        for(size_t i:{size_t(2),size_t(3)}) {
            auto bone=pickerNodes[i].GetJsObject(); bone["armature"]=JsValue("Armature");
            pickerNodes[i]=JsValue(bone);
        }
        withPicker["nodes"]=JsValue(pickerNodes);
        auto pickerLayer=SdfLayer::CreateAnonymous("picker.blendrig",{{"strict","1"}});
        CHECK(pickerLayer->ImportFromString(JsWriteToString(JsValue(withPicker))));
        auto withIk=withPicker;
        auto ikConstraints=withIk["constraints"].GetJsArray();
        JsObject ik{{"owner",JsValue("Tip")},{"source",JsValue("Driver")},
                    {"name",JsValue("Arm IK")},{"type",JsValue("IK")},
                    {"enabled",JsValue(true)},{"influence",JsValue(1.0)},
                    {"owner_space",JsValue("WORLD")},{"target_space",JsValue("WORLD")},
                    {"chain_count",JsValue(2)},{"use_stretch",JsValue(true)},
                    {"pole",JsValue("Driver")}};
        ikConstraints.push_back(JsValue(ik));withIk["constraints"]=JsValue(ikConstraints);
        auto ikLayer=SdfLayer::CreateAnonymous("ik.blendrig");
        CHECK(ikLayer->ImportFromString(JsWriteToString(JsValue(withIk))));
        auto ikStage=UsdStage::Open(ikLayer);int hiddenTips=0,ikSolvers=0;
        for(const auto &prim:ikStage->Traverse()) {
            if(prim.GetTypeName()==TfToken("RigExecSingleChainIkConstraint")) {
                SdfPathVector ends;
                CHECK(prim.GetRelationship(TfToken("rigExec:endJoint")).GetTargets(&ends));
                CHECK(ends.size()==1);
                if(ends.size()==1) {
                    const auto endpoint=ikStage->GetPrimAtPath(ends.front());
                    CHECK(endpoint);
                    double radius=-1;float opacity=-1;
                    CHECK(endpoint.GetAttribute(TfToken("guide:radius")).Get(&radius));CHECK(radius==0);
                    CHECK(endpoint.GetAttribute(TfToken("guide:displayOpacity")).Get(&opacity));CHECK(opacity==0);
                    ++hiddenTips;
                }
                TfToken mode;float stretch=-1;
                CHECK(prim.GetAttribute(TfToken("rigExec:poleVectorMode")).Get(&mode));CHECK(mode==TfToken("object"));
                CHECK(prim.GetAttribute(TfToken("inputs:stretch")).Get(&stretch));CHECK(stretch==0);
                ++ikSolvers;
            }
        }
        CHECK(hiddenTips==1);CHECK(ikSolvers==1);
        auto pickerStage=UsdStage::Open(pickerLayer); int pickerButtons=0;
        int editableBones=0;
        for(const auto &prim:pickerStage->Traverse()) {
            if(prim.GetTypeName()!=TfToken("RigExecJoint"))continue;
            SdfPathVector targets;
            CHECK(prim.GetRelationship(TfToken("blender:channelControl")).GetTargets(&targets));
            CHECK(targets.size()==1);
            auto control=pickerStage->GetPrimAtPath(targets.front()); CHECK(control);
            CHECK(control.GetTypeName()==TfToken("RigExecControl"));
            CHECK(control.GetCustomDataByKey(TfToken("blender:sourceId"))==prim.GetCustomDataByKey(TfToken("blender:id")));
            CHECK(!control.GetAttribute(TfToken("posed:space")).HasAuthoredConnections());
            TfToken order; CHECK(control.GetAttribute(TfToken("avars:rotationOrder")).Get(&order));
            CHECK(order==TfToken("XYZ"));
            for(const auto *name:{"default:space"}) {
                SdfPathVector connections;
                CHECK(control.GetAttribute(TfToken(name)).GetConnections(&connections));
                CHECK(connections.size()==1);
                CHECK(connections.front().GetPrimPath().HasPrefix(SdfPath("/Rig/ControlSpaces")));
            }
            for(const auto *channel:{"tx","ty","tz","rx","ry","rz","sx","sy","sz"}) {
                SdfPathVector connections;
                CHECK(prim.GetAttribute(TfToken(std::string("avars:")+channel)).GetConnections(&connections));
                CHECK(connections.size()==1);
                CHECK(connections.front()==targets.front().AppendProperty(TfToken(std::string("avars:")+channel)));
            }
            ++editableBones;
        }
        CHECK(editableBones==2);
        for(const auto &prim:pickerStage->Traverse()) {
            if(prim.GetTypeName()!=TfToken("RigExecPickerButton"))continue;
            ++pickerButtons; SdfPathVector targets;
            CHECK(prim.GetRelationship(TfToken("rigExec:picker:controls")).GetTargets(&targets));
            CHECK(targets.size()==1); CHECK(pickerStage->GetPrimAtPath(targets[0]));
            CHECK(pickerStage->GetPrimAtPath(targets[0]).GetTypeName()==TfToken("RigExecControl"));
            CHECK(pickerStage->GetPrimAtPath(targets[0]).GetCustomDataByKey(TfToken("blender:sourceId")).Get<std::string>()=="Root");
            CHECK(!pickerStage->GetPrimAtPath(targets[0]).GetAttribute(TfToken("posed:space")).HasAuthoredConnections());
        }
        CHECK(pickerButtons==1);
        auto genericPicker=json.GetJsObject(); genericPicker["pickers"]=JsValue(JsArray{JsValue(picker)});
        auto genericLayer=SdfLayer::CreateAnonymous("generic-picker.blendrig",{{"strict","1"}});
        CHECK(genericLayer->ImportFromString(JsWriteToString(JsValue(genericPicker))));
        auto genericStage=UsdStage::Open(genericLayer);
        auto genericButton=genericStage->GetPrimAtPath(SdfPath("/Rig/Pickers/Root/page_0/button_0"));
        CHECK(genericButton);
        SdfPathVector genericTargets;
        CHECK(genericButton.GetRelationship(TfToken("rigExec:picker:controls")).GetTargets(&genericTargets));
        CHECK(genericTargets.size()==1);
        CHECK(genericStage->GetPrimAtPath(genericTargets.front()).GetTypeName()==TfToken("RigExecJoint"));
        auto constrainedGeneric=genericPicker;
        auto constrainedButton=button; constrainedButton["controls"]=JsValue(JsArray{JsValue("Tip")});
        auto constrainedPage=page; constrainedPage["buttons"]=JsValue(JsArray{JsValue(constrainedButton)});
        auto constrainedPicker=picker; constrainedPicker["pages"]=JsValue(JsArray{JsValue(constrainedPage)});
        constrainedGeneric["pickers"]=JsValue(JsArray{JsValue(constrainedPicker)});
        auto constrainedLayer=SdfLayer::CreateAnonymous("constrained-generic.blendrig");
        CHECK(constrainedLayer->ImportFromString(JsWriteToString(JsValue(constrainedGeneric))));
        auto constrainedStage=UsdStage::Open(constrainedLayer);
        CHECK(constrainedStage->GetPrimAtPath(SdfPath("/Rig/Pickers/Root/page_0/button_0"))
              .GetCustomDataByKey(TfToken("blender:unavailable")).Get<bool>());
        auto unavailable=withPicker;
        auto unavailableNodes=unavailable["nodes"].GetJsArray();
        auto connected=unavailableNodes[2].GetJsObject(); connected["connected"]=JsValue(true);
        unavailableNodes[2]=JsValue(connected); unavailable["nodes"]=JsValue(unavailableNodes);
        auto unavailableButton=button; unavailableButton["controls"]=JsValue(JsArray{controlId,JsValue("Tip")});
        auto unavailablePage=page; unavailablePage["buttons"]=JsValue(JsArray{JsValue(unavailableButton)});
        auto unavailablePicker=picker; unavailablePicker["pages"]=JsValue(JsArray{JsValue(unavailablePage)});
        unavailable["pickers"]=JsValue(JsArray{JsValue(unavailablePicker)});
        auto unavailableLayer=SdfLayer::CreateAnonymous("unavailable.blendrig");
        CHECK(unavailableLayer->ImportFromString(JsWriteToString(JsValue(unavailable))));
        auto unavailableStage=UsdStage::Open(unavailableLayer);
        auto inactive=unavailableStage->GetPrimAtPath(SdfPath("/Rig/Pickers/Root/page_0/button_0"));
        CHECK(inactive);
        SdfPathVector inactiveTargets;
        auto inactiveRelation=inactive.GetRelationship(TfToken("rigExec:picker:controls"));
        if(inactiveRelation) inactiveRelation.GetTargets(&inactiveTargets);
        CHECK(inactiveTargets.size()==2);
        auto connectedControl=unavailableStage->GetPrimAtPath(inactiveTargets.front());
        SdfPathVector spaces;
        CHECK(connectedControl.GetRelationship(TfToken("rigExec:channelSpaces")).GetTargets(&spaces));
        CHECK(spaces.size()==2);
        bool translationEnabled=true;
        CHECK(connectedControl.GetAttribute(TfToken("rigExec:translationEnabled")).Get(&translationEnabled));
        CHECK(!translationEnabled);
        CHECK(connectedControl.GetAttribute(TfToken("posed:space")).HasAuthoredConnections());
        std::string inactiveLabel; CHECK(inactive.GetAttribute(TfToken("ui:text")).Get(&inactiveLabel));
        CHECK(inactiveLabel=="Root");
        auto inertBinding=withPicker;
        auto bindingButton=button; bindingButton["controls"]=JsValue(JsArray{});
        bindingButton["source"]=JsValue(JsObject{{"binding",JsValue(JsObject{{"operator",JsValue("toggle")}})}});
        auto bindingPage=page; bindingPage["buttons"]=JsValue(JsArray{JsValue(bindingButton)});
        auto bindingPicker=picker; bindingPicker["pages"]=JsValue(JsArray{JsValue(bindingPage)});
        inertBinding["pickers"]=JsValue(JsArray{JsValue(bindingPicker)});
        auto bindingLayer=SdfLayer::CreateAnonymous("binding.blendrig");
        CHECK(bindingLayer->ImportFromString(JsWriteToString(JsValue(inertBinding))));
        auto bindingStage=UsdStage::Open(bindingLayer);
        auto bindingPrim=bindingStage->GetPrimAtPath(SdfPath("/Rig/Pickers/Root/page_0/button_0"));
        CHECK(bindingPrim.GetCustomDataByKey(TfToken("blender:unavailable")).Get<bool>());
        std::string bindingLabel; CHECK(bindingPrim.GetAttribute(TfToken("ui:text")).Get(&bindingLabel));
        CHECK(bindingLabel=="Root (unavailable)");
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
        baked=false;
        CHECK(UsdStage::Open(usd)->GetDefaultPrim().GetAttribute(TfToken("rigExec:baked")).Get(&baked));
        CHECK(baked);
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
