#include "pxr/base/plug/registry.h"
#include "pxr/base/js/json.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/gf/vec4f.h"
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
#include <set>
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
        // Authoring a property the installed schema lacks posts a coding error
        // that Python hosts receive as an exception on open.
        TfErrorMark opening;
        auto layer=SdfLayer::FindOrOpen(fixture); CHECK(layer);
        auto stage=UsdStage::Open(layer); CHECK(stage);
        CHECK(opening.IsClean());
        CHECK(stage->GetDefaultPrim().GetTypeName()==TfToken("RigExecRoot"));
        // usdRig has one compiled evaluator; the root carries no evaluation-mode opt-in.
        CHECK(!stage->GetDefaultPrim().GetAttribute(TfToken("rigExec:baked")).IsAuthored());
        CHECK(stage->GetDefaultPrim().GetCustomDataByKey(TfToken("rigExec:connectedPoseSeedReuse")).IsEmpty());
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
        int editableBones=0,nativeSpaces=0,literalSpaces=0;
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
            {
                // A rigid eligible bone switches to its Dag parent and holds its
                // raw rest over that parent's default space, which RigExec builds
                // from orthonormalized rests; others keep a neutral provider.
                // The default is a literal on the schema attribute. An identity
                // one equals no opinion, so it connects to the identity source:
                // either way stock Pivot editing sees an authored default space.
                SdfPathVector connections,sources;
                const auto defaultSpace=control.GetAttribute(TfToken("default:space"));
                defaultSpace.GetConnections(&connections);
                auto spaceSwitch=pickerStage->GetPrimAtPath(SdfPath("/Rig/Mechanisms").AppendChild(TfToken("controlSpace_"+targets.front().GetParentPath().GetName())));
                CHECK(spaceSwitch); CHECK(spaceSwitch.GetRelationship(TfToken("rigExec:sources")).GetTargets(&sources)); CHECK(sources.size()==1);
                // The switch carries schema properties only.
                for(const auto &name:spaceSwitch.GetAuthoredPropertyNames()) CHECK(spaceSwitch.GetPrimDefinition().GetPropertyDefinition(name));
                if(sources.front()==prim.GetPath().GetParentPath()) {
                    GfMatrix4d held(0.0),expected(1.0);
                    CHECK(prim.GetAttribute(TfToken("rest:space")).Get(&expected));
                    for(auto parent=prim.GetParent();parent.GetPath()!=SdfPath("/Rig/Dag");parent=parent.GetParent()) {
                        GfMatrix4d rest(1); CHECK(parent.GetAttribute(TfToken("rest:space")).Get(&rest));
                        rest.Orthonormalize(false); expected*=rest;
                    }
                    if(connections.empty()) {
                        CHECK(defaultSpace.HasAuthoredValue()); CHECK(defaultSpace.Get(&held)); CHECK(held!=GfMatrix4d(1));
                        ++literalSpaces;
                    } else {
                        CHECK(connections==SdfPathVector{SdfPath("/Rig/Mechanisms/identityParent.outputs:matrix")});
                        CHECK(!defaultSpace.HasAuthoredValue()); CHECK(pickerStage->GetAttributeAtPath(connections.front()).Get(&held));
                        CHECK(held==GfMatrix4d(1));
                    }
                    CHECK(GfIsClose(held,expected,1e-14));
                    ++nativeSpaces;
                }
                else {
                    CHECK(connections.size()==1);
                    const auto space=connections.front().GetPrimPath();
                    CHECK(connections.front().GetNameToken()==TfToken("default:space"));
                    CHECK(space.GetParentPath()==SdfPath("/Rig/ControlSpaces")); CHECK(pickerStage->GetPrimAtPath(space)); CHECK(sources.front()==space);
                }
                // The saved pose stays readable on the bone beside its relay connections.
                for(const auto *channel:{"tx","ty","tz","rx","ry","rz","sx","sy","sz"}) {
                    double bone=0,value=1; const TfToken avar(std::string("avars:")+channel);
                    CHECK(prim.GetAttribute(avar).Get(&bone)); CHECK(control.GetAttribute(avar).Get(&value)); CHECK(bone==value);
                }
            }
            for(const auto *channel:{"tx","ty","tz","rx","ry","rz","sx","sy","sz"}) {
                SdfPathVector connections;
                CHECK(prim.GetAttribute(TfToken(std::string("avars:")+channel)).GetConnections(&connections));
                CHECK(connections.size()==1);
                CHECK(connections.front()==targets.front().AppendProperty(TfToken(std::string("avars:")+channel)));
            }
            ++editableBones;
        }
        CHECK(editableBones==2); CHECK(nativeSpaces==2); CHECK(literalSpaces==1);
        // A scope that only collects providers is one static, hidden provider,
        // so that those have an ancestor frame; the Dag hierarchy is not.
        for(const char *name:{"Controls","Mechanisms"}) {
            const auto holder=pickerStage->GetPrimAtPath(SdfPath("/Rig").AppendChild(TfToken(name)));
            CHECK(holder.GetTypeName()==TfToken("RigExecControl"));
            float opacity=1; CHECK(holder.GetAttribute(TfToken("guide:displayOpacity")).Get(&opacity)); CHECK(opacity==0);
            for(const auto &property:holder.GetAuthoredProperties()) CHECK(property.GetName()==TfToken("guide:displayOpacity") ||
                property.GetName()==UsdGeomTokens->purpose || property.GetName()==TfToken("default:space"));
            // Skin providers bind through the holder's rest, so like a Control
            // it authors its default space: stock Pivot editing is refused.
            SdfPathVector holderDefault; holder.GetAttribute(TfToken("default:space")).GetConnections(&holderDefault);
            CHECK(holderDefault==SdfPathVector{SdfPath("/Rig/Mechanisms/identityParent.outputs:matrix")});
            CHECK(!holder.GetAttribute(TfToken("default:space")).HasAuthoredValue());
            // One extent carries one purpose, and the Controls beneath are guides.
            CHECK(UsdGeomImageable(holder).GetPurposeAttr().HasAuthoredValue()==(std::string(name)=="Controls"));
            CHECK(!holder.HasCustomDataKey(TfToken("blender:id")) && !holder.HasCustomDataKey(TfToken("blender:sourceId")));
        }
        CHECK(UsdGeomImageable(pickerStage->GetPrimAtPath(SdfPath("/Rig/Controls"))).ComputePurpose()==UsdGeomTokens->guide);
        CHECK(pickerStage->GetPrimAtPath(SdfPath("/Rig/Dag")).GetTypeName()==TfToken("Scope"));
        // Without armature bones nothing is collected under /Rig/Controls.
        CHECK(stage->GetPrimAtPath(SdfPath("/Rig/Controls")).GetTypeName()==TfToken("Scope"));
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
        // A read-only selection is picker metadata with its reason, never a dead button.
        auto pickerPrims=[](const UsdStageRefPtr &s,const char *type) {
            int count=0;
            for(const auto &prim:s->Traverse())count+=prim.GetTypeName()==TfToken(type);
            return count;
        };
        auto unavailableEntries=[](const UsdStageRefPtr &s,const char *key) {
            return s->GetPrimAtPath(SdfPath("/Rig/Pickers/Root")).GetCustomDataByKey(TfToken(key)).GetWithDefault<VtStringArray>();
        };
        auto warned=[](const UsdStageRefPtr &s,const std::string &text) {
            VtStringArray messages; s->GetDefaultPrim().GetAttribute(TfToken("blender:diagnostics")).Get(&messages);
            return std::count(messages.begin(),messages.end(),text)==1;
        };
        CHECK(pickerPrims(constrainedStage,"RigExecPicker")==1);
        CHECK(pickerPrims(constrainedStage,"RigExecPickerPanel")==0 && pickerPrims(constrainedStage,"RigExecPickerButton")==0);
        CHECK(unavailableEntries(constrainedStage,"blender:unavailable")==VtStringArray{"Main: Root"});
        CHECK(unavailableEntries(constrainedStage,"blender:unsupportedReason")==VtStringArray{"Read-only target in standard usdRig interaction"});
        CHECK(warned(constrainedStage,"picker target is read-only in standard usdRig interaction: Tip"));
        // A source button with a read-only member never selects a subset: Root
        // is not selectable through it, only through a button that lists it alone.
        auto partialButton=button; partialButton["controls"]=JsValue(JsArray{controlId,JsValue("Tip")});
        for(const bool alone:{false,true}) {
            JsArray partialButtons{JsValue(partialButton)};
            if(alone)partialButtons.push_back(JsValue(button));
            auto partialPage=page; partialPage["buttons"]=JsValue(partialButtons);
            auto partialPicker=picker; partialPicker["pages"]=JsValue(JsArray{JsValue(partialPage)});
            auto partial=genericPicker; partial["pickers"]=JsValue(JsArray{JsValue(partialPicker)});
            auto partialLayer=SdfLayer::CreateAnonymous("partial.blendrig");
            CHECK(partialLayer->ImportFromString(JsWriteToString(JsValue(partial))));
            auto partialStage=UsdStage::Open(partialLayer);
            CHECK(pickerPrims(partialStage,"RigExecPickerButton")==(alone ? 1 : 0));
            CHECK(unavailableEntries(partialStage,"blender:unavailable").size()==1);
            CHECK(partialStage->GetPrimAtPath(SdfPath("/Rig/Pickers/Root")).GetCustomDataByKey(TfToken("blender:groupCount")).Get<int>()==0);   // never a partial group
            CHECK(partialStage->GetPrimAtPath(SdfPath("/Rig/Pickers/Root")).GetCustomDataByKey(TfToken("blender:controlCount")).Get<int>()==(alone ? 1 : 0));
            if(alone) {
                SdfPathVector only;
                CHECK(partialStage->GetPrimAtPath(SdfPath("/Rig/Pickers/Root/page_0/button_0"))
                      .GetRelationship(TfToken("rigExec:picker:controls")).GetTargets(&only));
                CHECK(only.size()==1 && partialStage->GetPrimAtPath(only.front()).GetCustomDataByKey(TfToken("blender:id")).Get<std::string>()=="Root");
            }
        }
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
        // A multi-control source button whose members are all selectable is
        // kept as one group button with the source label and the whole source
        // set, ahead of one overflow entry per control in source order.
        auto sourceGroup=unavailableStage->GetPrimAtPath(SdfPath("/Rig/Pickers/Root/page_0/button_0"));
        CHECK(sourceGroup);
        SdfPathVector groupTargets,inactiveTargets,secondTargets;
        CHECK(sourceGroup.GetRelationship(TfToken("rigExec:picker:controls")).GetTargets(&groupTargets));
        auto inactive=unavailableStage->GetPrimAtPath(SdfPath("/Rig/Pickers/Root/page_0/button_1"));
        CHECK(inactive);
        auto inactiveRelation=inactive.GetRelationship(TfToken("rigExec:picker:controls"));
        if(inactiveRelation) inactiveRelation.GetTargets(&inactiveTargets);
        CHECK(inactiveTargets.size()==1);
        auto second=unavailableStage->GetPrimAtPath(SdfPath("/Rig/Pickers/Root/page_0/button_2"));
        CHECK(second && second.GetRelationship(TfToken("rigExec:picker:controls")).GetTargets(&secondTargets));
        CHECK(secondTargets.size()==1 && secondTargets!=inactiveTargets);
        CHECK((groupTargets==SdfPathVector{inactiveTargets.front(),secondTargets.front()}));
        CHECK(unavailableStage->GetPrimAtPath(secondTargets.front()).GetCustomDataByKey(TfToken("blender:sourceId")).Get<std::string>()=="Tip");
        CHECK(!unavailableStage->GetPrimAtPath(SdfPath("/Rig/Pickers/Root/page_0/button_3")));
        CHECK(pickerPrims(unavailableStage,"RigExecPickerButton")==3);
        CHECK(unavailableEntries(unavailableStage,"blender:unavailable").empty());
        const auto counted=[&](const char *key) { return unavailableStage->GetPrimAtPath(SdfPath("/Rig/Pickers/Root")).GetCustomDataByKey(TfToken(key)).Get<int>(); };
        CHECK(counted("blender:controlCount")==2 && counted("blender:overflowCount")==2 && counted("blender:groupCount")==1);
        std::string groupLabel; CHECK(sourceGroup.GetAttribute(TfToken("ui:text")).Get(&groupLabel) && groupLabel=="Root");
        GfVec4f groupFill(0),singleFill(0);
        CHECK(sourceGroup.GetAttribute(TfToken("ui:fill")).Get(&groupFill) && inactive.GetAttribute(TfToken("ui:fill")).Get(&singleFill) && groupFill!=singleFill);
        // Values equal to the schema fallback and source JSON are not authored.
        CHECK(!sourceGroup.GetAttribute(TfToken("ui:position")).HasAuthoredValue());
        CHECK(inactive.GetAttribute(TfToken("ui:position")).HasAuthoredValue() && second.GetAttribute(TfToken("ui:position")).HasAuthoredValue());
        for(const auto &prim:unavailableStage->GetPrimAtPath(SdfPath("/Rig/Pickers")).GetAllDescendants())
            CHECK(!prim.HasCustomDataKey(TfToken("blender:source")) && !prim.HasCustomDataKey(TfToken("blender:controlInventory")));
        auto connectedControl=unavailableStage->GetPrimAtPath(inactiveTargets.front());
        SdfPathVector spaces;
        CHECK(connectedControl.GetRelationship(TfToken("rigExec:channelSpaces")).GetTargets(&spaces));
        CHECK(spaces.size()==2);
        // Rotation and translation bases are siblings in two scopes: neither
        // has a namespace parent frame, so neither needs a parent:space boundary.
        CHECK(spaces[0]==SdfPath("/Rig/ControlSpaces").AppendChild(connectedControl.GetParent().GetName()));
        CHECK(spaces[1]==SdfPath("/Rig/TranslationSpaces").AppendChild(connectedControl.GetParent().GetName()));
        for(const auto &space:spaces) {
            const auto provider=unavailableStage->GetPrimAtPath(space);
            CHECK(provider.GetTypeName()==TfToken("RigExecControl")); CHECK(provider.GetChildren().empty());
            CHECK(provider.GetParent().GetTypeName()==TfToken("RigExecControl"));
            CHECK(provider.GetAttribute(TfToken("posed:space")).HasAuthoredConnections());
            CHECK(!provider.GetAttribute(TfToken("parent:space")).HasAuthoredConnections());
        }
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
        // Settings/operator bindings are compact metadata plus the unchanged diagnostic.
        CHECK(pickerPrims(bindingStage,"RigExecPickerPanel")==0 && pickerPrims(bindingStage,"RigExecPickerButton")==0);
        CHECK(unavailableEntries(bindingStage,"blender:unavailable")==VtStringArray{"Main: Root"});
        CHECK(unavailableEntries(bindingStage,"blender:unsupportedReason")==VtStringArray{"Source settings and operators are not translated"});
        CHECK(warned(bindingStage,"picker binding is selection-only; settings and operators are not translated: Root"));
        // A binding that selects nothing and states why it is unsupported is a
        // source setting and keeps that wording and reason, with or without the
        // layout and inventory fields of older snapshots, which are accepted
        // and ignored. One that selects controls stays a selection.
        auto legacyButton=bindingButton;
        legacyButton["source"]=JsValue(JsObject{{"binding",JsValue(JsObject{{"property",JsValue("ik")}})},{"unsupported_reason",JsValue("Not native")}});
        auto legacyPage=page; legacyPage["buttons"]=JsValue(JsArray{JsValue(button),JsValue(legacyButton)});
        for(const bool formerLayout:{true,false}) {
            auto legacyPicker=picker; legacyPicker["pages"]=JsValue(JsArray{JsValue(legacyPage)});
            if(formerLayout) { legacyPicker["layout"]=JsValue("biped"); legacyPicker["inventory"]=JsValue(JsObject{{"controls",JsValue(JsArray{})}}); }
            auto legacy=withPicker; legacy["pickers"]=JsValue(JsArray{JsValue(legacyPicker)});
            auto legacyLayer=SdfLayer::CreateAnonymous("legacy.blendrig");
            CHECK(legacyLayer->ImportFromString(JsWriteToString(JsValue(legacy))));
            auto legacyStage=UsdStage::Open(legacyLayer);
            CHECK(pickerPrims(legacyStage,"RigExecPickerButton")==1);
            CHECK(unavailableEntries(legacyStage,"blender:unsupportedReason")==VtStringArray{"Not native"});
            CHECK(warned(legacyStage,"picker setting unavailable; source behavior is not translated: Root"));
            CHECK(!warned(legacyStage,"picker binding is selection-only; settings and operators are not translated: Root"));
        }
        {
            auto selecting=legacyButton; selecting["controls"]=JsValue(JsArray{controlId});
            auto selectingPage=page; selectingPage["buttons"]=JsValue(JsArray{JsValue(selecting)});
            auto selectingPicker=picker; selectingPicker["pages"]=JsValue(JsArray{JsValue(selectingPage)});
            auto data=withPicker; data["pickers"]=JsValue(JsArray{JsValue(selectingPicker)});
            auto layer=SdfLayer::CreateAnonymous("selecting.blendrig");
            CHECK(layer->ImportFromString(JsWriteToString(JsValue(data))));
            auto selectingStage=UsdStage::Open(layer);
            CHECK(pickerPrims(selectingStage,"RigExecPickerButton")==1);
            CHECK(warned(selectingStage,"picker binding is selection-only; settings and operators are not translated: Root"));
            CHECK(!warned(selectingStage,"picker setting unavailable; source behavior is not translated: Root"));
        }
        // A page name and a button label are required even where no panel shows them.
        for(const bool dropLabel:{false,true}) {
            auto bareButton=button; auto barePage=page;
            if(dropLabel)bareButton.erase("label"); else barePage.erase("name");
            barePage["buttons"]=JsValue(JsArray{JsValue(bareButton)});
            auto barePicker=picker; barePicker["pages"]=JsValue(JsArray{JsValue(barePage)});
            bad=withPicker; bad["pickers"]=JsValue(JsArray{JsValue(barePicker)}); fail(JsValue(bad));
        }
        // Every rig maps onto the biped template by role: side parse plus the
        // rig-generator name table. Unresolved slots are not authored.
        const auto boneTemplate=withPicker["nodes"].GetJsArray()[2].GetJsObject();
        struct Bone { const char *name; bool visible=true,shaped=false; };
        const auto roleStage=[&](const std::vector<Bone> &bones,const JsArray &extraButtons=JsArray{}) {
            auto data=withPicker; auto roleNodes=data["nodes"].GetJsArray();
            JsArray roleButtons{JsValue(button)};
            for(const Bone &bone:bones) {
                auto node=boneTemplate; node["id"]=JsValue(bone.name); node["name"]=JsValue(bone.name);
                node["parent"]=JsValue("Root"); node["visible"]=JsValue(bone.visible);
                if(bone.shaped) {
                    node["guide_points"]=JsValue(JsArray{JsValue(JsArray{JsValue(0.0),JsValue(0.0),JsValue(0.0)}),JsValue(JsArray{JsValue(0.0),JsValue(1.0),JsValue(0.0)})});
                    node["guide_counts"]=JsValue(JsArray{JsValue(2)});
                }
                roleNodes.push_back(JsValue(node));
                roleButtons.push_back(JsValue(JsObject{{"label",JsValue(bone.name)},{"controls",JsValue(JsArray{JsValue(bone.name)})}}));
            }
            for(const auto &extra:extraButtons)roleButtons.push_back(extra);
            data["nodes"]=JsValue(roleNodes);
            auto rolePage=page; rolePage["buttons"]=JsValue(roleButtons);
            auto rolePicker=picker; rolePicker["owner"]=JsValue("Armature"); rolePicker["pages"]=JsValue(JsArray{JsValue(rolePage)});
            data["pickers"]=JsValue(JsArray{JsValue(rolePicker)});
            auto roleLayer=SdfLayer::CreateAnonymous("biped.blendrig");
            CHECK(roleLayer->ImportFromString(JsWriteToString(JsValue(data))));
            return UsdStage::Open(roleLayer);
        };
        auto bipedStage=roleStage({{"hand_ik.L"},{"hand_ik.R"},{"IK-Wrist.R"},{"foot_ik.L",false,true},{"IK-Foot.L"},{"IK-Foot.R"},
                                   {"hand_fk.L"},{"FK-Wrist.L"},{"upper_arm_fk.L"},{"FK-UpperArm.R"},{"forearm_fk.L"},{"torso"},{"Left Whisker"},
                                   {"foot_fk.L"},{"thigh_fk.L"},{"thigh_ik_target.L"},{"shin_fk.R",false,false}});
        const SdfPath bipedPath("/Rig/Pickers/Armature");
        auto selected=[&](const std::string &relative) {
            std::vector<std::string> names; SdfPathVector found;
            auto prim=bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath(relative)));
            if(prim)prim.GetRelationship(TfToken("rigExec:picker:controls")).GetTargets(&found);
            for(const auto &path:found) {
                auto control=bipedStage->GetPrimAtPath(path);
                CHECK(control.GetTypeName()==TfToken("RigExecControl"));
                names.push_back(control.GetCustomDataByKey(TfToken("blender:sourceId")).Get<std::string>());
            }
            return names;
        };
        using Names=std::vector<std::string>;
        CHECK(selected("Body/b_M_Main")==Names{"Root"});                  // case-insensitive centre name
        CHECK(selected("Body/b_M_Body")==Names{"torso"});
        CHECK(selected("Body/b_L_ArmIK")==Names{"hand_ik.L"});            // Rigify, side suffix
        CHECK(selected("Body/b_R_ArmIK")==Names{"hand_ik.R"});            // table order: Rigify name before CloudRig
        CHECK(selected("Body/b_L_LegIK")==Names{"IK-Foot.L"});            // visible control before a hidden shaped one
        CHECK(selected("Body/b_R_LegIK")==Names{"IK-Foot.R"});            // CloudRig prefix
        CHECK(selected("Body/b_L_Hand")==Names{"hand_fk.L"});             // the template holds no names: table order again
        CHECK(selected("Body/b_L_UpArm")==Names{"upper_arm_fk.L"});
        CHECK(selected("Body/b_R_UpArm")==Names{"FK-UpperArm.R"});
        CHECK(selected("Body/b_L_Shldr_x15")==(Names{"hand_fk.L","upper_arm_fk.L","forearm_fk.L","hand_ik.L"}));  // group keeps resolved roles only
        CHECK(selected("Body/b_R_Shldr_x16")==(Names{"FK-UpperArm.R","hand_ik.R"}));
        CHECK(!bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath("Body/b_L_IndexTip_x21"))));   // group with no resolved role dropped
        CHECK(selected("Body/b_L_Foot")==Names{"foot_fk.L"});
        CHECK(selected("Body/b_L_UpLeg")==Names{"thigh_fk.L"});
        CHECK(selected("Body/b_L_LegPV")==Names{"thigh_ik_target.L"});
        CHECK(!bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath("Body/b_R_UpLeg"))));   // unresolved slot dropped
        CHECK(!bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath("Body/b_R_LoLeg"))));   // shin_fk.R is hidden and unshaped: a mechanism, no slot
        CHECK(!bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath("Body/t_Hand"))));      // native attribute slot never copied
        CHECK(!bipedStage->GetPrimAtPath(bipedPath.AppendChild(TfToken("Facial"))));          // panel without a live slot
        // The stock picker hit-tests bounding boxes and gives a click to the
        // topmost button: widget controls first, then the larger ui:depth, then
        // the later prim. Natively an IK/FK dial hides one half of each stacked
        // pair (hand, foot, knee pole inside the thigh); without the dial every
        // selecting button must keep a clickable share of its own rectangle.
        struct Box { std::string name; double x,y,w,h,z; bool overlay; };
        const auto box=[](const UsdPrim &prim) {
            GfVec2f position(0),size(10); float depth=0; TfToken shape; bool alternate=false;
            prim.GetAttribute(TfToken("ui:position")).Get(&position); prim.GetAttribute(TfToken("ui:size")).Get(&size);
            prim.GetAttribute(TfToken("ui:depth")).Get(&depth); prim.GetAttribute(TfToken("ui:shape")).Get(&shape);
            prim.GetAttribute(TfToken("ui:alternate")).Get(&alternate);
            CHECK(!alternate && shape!=TfToken("slider"));   // never drawn by the stock picker
            return Box{prim.GetName().GetString(),position[0],position[1],size[0],size[1],depth,shape==TfToken("widgetControl")};
        };
        const auto clickable=[](const std::vector<Box> &boxes) {
            std::set<double> xs,ys; std::map<std::string,double> share;
            for(const auto &b:boxes) { xs.insert(b.x); xs.insert(b.x+b.w); ys.insert(b.y); ys.insert(b.y+b.h); share[b.name]=0; }
            if(boxes.empty())return share;
            for(auto x=xs.begin();std::next(x)!=xs.end();++x) for(auto y=ys.begin();std::next(y)!=ys.end();++y) {
                const double cx=(*x+*std::next(x))/2,cy=(*y+*std::next(y))/2; const Box *top=nullptr;
                for(const auto &b:boxes)
                    if(b.x<=cx && cx<=b.x+b.w && b.y<=cy && cy<=b.y+b.h &&
                       (!top || std::make_pair(b.overlay,b.z)>=std::make_pair(top->overlay,top->z)))top=&b;
                if(top)share[top->name]+=(*std::next(x)-*x)*(*std::next(y)-*y)/(top->w*top->h);
            }
            return share;
        };
        // Worst case first: every role slot of the packaged template live at once.
        // The template carries role keys only, never a source bone name.
        auto templateStage=UsdStage::Open(std::string(argv[1])+"/biped_picker.usda"); CHECK(templateStage);
        int templateSlots=0;
        std::map<std::string,std::string> slotRole;
        for(const auto &panel:templateStage->GetPrimAtPath(SdfPath("/Template")).GetChildren()) {
            std::vector<Box> slots;
            for(const auto &slot:panel.GetChildren()) {
                const auto custom=slot.GetCustomDataByKey(TfToken("blender"));
                if(custom.IsEmpty())continue;
                CHECK(custom.IsHolding<VtDictionary>());
                const auto &keys=custom.UncheckedGet<VtDictionary>();
                CHECK(keys.size()==1 && keys.begin()->first=="roles" && keys.begin()->second.IsHolding<VtStringArray>());
                const auto &roles=keys.begin()->second.UncheckedGet<VtStringArray>();
                CHECK(!roles.empty());
                if(roles.size()==1)slotRole[panel.GetName().GetString()+"/"+slot.GetName().GetString()]=roles[0];
                slots.push_back(box(slot));
            }
            for(const auto &entry:clickable(slots)) { CHECK(entry.second>=0.25); ++templateSlots; }
        }
        CHECK(templateSlots==326);
        for(const auto &panel:bipedStage->GetPrimAtPath(bipedPath).GetChildren()) {
            std::vector<Box> live;
            for(const auto &slot:panel.GetChildren()) if(slot.GetRelationship(TfToken("rigExec:picker:controls")).HasAuthoredTargets())live.push_back(box(slot));
            CHECK(!live.empty());
            for(const auto &entry:clickable(live))CHECK(entry.second>=0.25);
        }
        // The split pairs do not overlap at all.
        for(const auto &pair:{std::make_pair("Body/b_L_Hand","Body/b_L_ArmIK"),std::make_pair("Body/b_L_Foot","Body/b_L_LegIK")}) {
            const auto fk=box(bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath(pair.first)))),ik=box(bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath(pair.second))));
            CHECK(fk.w>0 && ik.w>0 && fk.y==ik.y && fk.h==ik.h && fk.x+fk.w==ik.x);
        }
        const auto share=clickable({box(bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath("Body/b_L_UpLeg")))),
                                    box(bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath("Body/b_L_LegPV"))))});
        CHECK(share.at("b_L_LegPV")>0.999 && share.at("b_L_UpLeg")>0.8);
        std::map<std::string,int> overflowCount; std::set<std::string> bipedTargets; int mirrorLinks=0,artwork=0;
        for(const auto &prim:bipedStage->GetPrimAtPath(bipedPath).GetAllDescendants()) {
            CHECK(!prim.HasCustomDataKey(TfToken("blender:source")) && !prim.HasCustomDataKey(TfToken("blender:unavailable")));
            if(prim.GetTypeName()!=TfToken("RigExecPickerButton"))continue;
            CHECK(!prim.HasAuthoredCustomData());   // template role keys are not copied
            bool alternate=false; prim.GetAttribute(TfToken("ui:alternate")).Get(&alternate);
            CHECK(!alternate);   // the stock picker hides an alternate that has no mode dial
            SdfPathVector peers,found;
            prim.GetRelationship(TfToken("rigExec:picker:mirror")).GetTargets(&peers);
            prim.GetRelationship(TfToken("rigExec:picker:controls")).GetTargets(&found);
            for(const auto &peer:peers) {
                // A link is authored both ways and joins two different selections (or two artwork shapes).
                SdfPathVector back,other;
                CHECK(peer.HasPrefix(bipedPath) && bipedStage->GetPrimAtPath(peer)); ++mirrorLinks;
                CHECK(bipedStage->GetPrimAtPath(peer).GetRelationship(TfToken("rigExec:picker:mirror")).GetTargets(&back) && back==SdfPathVector{prim.GetPath()});
                bipedStage->GetPrimAtPath(peer).GetRelationship(TfToken("rigExec:picker:controls")).GetTargets(&other);
                CHECK(found.empty()==other.empty() && (found.empty() || other!=found));
            }
            const bool overflowButton=prim.GetParent().GetName().GetString().rfind("page_",0)==0;
            CHECK(!overflowButton || found.size()==1);
            artwork+=found.empty();
            for(const auto &path:found) {
                const auto id=bipedStage->GetPrimAtPath(path).GetCustomDataByKey(TfToken("blender:sourceId")).Get<std::string>();
                bipedTargets.insert(id);
                if(overflowButton)++overflowCount[id];
            }
        }
        // Mirror pairs survive only between authored buttons.
        SdfPathVector armMirror;
        CHECK(bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath("Body/b_L_UpArm"))).GetRelationship(TfToken("rigExec:picker:mirror")).GetTargets(&armMirror));
        CHECK(armMirror==SdfPathVector{bipedPath.AppendPath(SdfPath("Body/b_R_UpArm"))});
        CHECK(selected("Body/b_L_LoArm")==Names{"forearm_fk.L"});
        CHECK(!bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath("Body/b_L_LoArm"))).HasRelationship(TfToken("rigExec:picker:mirror")));   // peer b_R_LoArm unresolved
        // The hand slots carry a pair of their own; FK hand has no live peer here.
        SdfPathVector handMirror;
        CHECK(bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath("Body/b_L_ArmIK"))).GetRelationship(TfToken("rigExec:picker:mirror")).GetTargets(&handMirror));
        CHECK(handMirror==SdfPathVector{bipedPath.AppendPath(SdfPath("Body/b_R_ArmIK"))});
        CHECK(!bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath("Body/b_L_Hand"))).HasRelationship(TfToken("rigExec:picker:mirror")));
        // Upper arms, IK hands, IK feet and the two arm groups, plus three artwork pairs.
        CHECK(mirrorLinks==14 && artwork==7);
        // Coverage equals the source pages; controls without a slot appear exactly once on overflow.
        CHECK(bipedTargets.size()==18);
        CHECK((overflowCount==std::map<std::string,int>{{"IK-Wrist.R",1},{"foot_ik.L",1},{"FK-Wrist.L",1},{"Left Whisker",1},{"shin_fk.R",1}}));
        std::string overflowLabel;
        CHECK(bipedStage->GetPrimAtPath(bipedPath.AppendPath(SdfPath("page_0"))).GetAttribute(TfToken("ui:label")).Get(&overflowLabel));
        CHECK(overflowLabel=="Main");
        CHECK(bipedStage->GetPrimAtPath(bipedPath).GetCustomDataByKey(TfToken("blender:controlCount")).Get<int>()==18);
        CHECK(bipedStage->GetPrimAtPath(bipedPath).GetCustomDataByKey(TfToken("blender:bipedCount")).Get<int>()==13);
        CHECK(bipedStage->GetPrimAtPath(bipedPath).GetCustomDataByKey(TfToken("blender:overflowCount")).Get<int>()==5);
        // Role -> source control of a translated rig, read back through the
        // single-role slots, with the picker invariants every rig must hold:
        // each live button clickable, mirror links both ways, one single-target
        // button per control, counts on the picker prim.
        using Roles=std::map<std::string,std::string>;
        const auto resolvedRoles=[&](const UsdStageRefPtr &translated,size_t controls) {
            Roles roles; std::map<std::string,int> singles; std::set<std::string> all,slotted; int overflowButtons=0,groupButtons=0;
            const auto name=[&](const SdfPath &path) { return translated->GetPrimAtPath(path).GetCustomDataByKey(TfToken("blender:sourceId")).Get<std::string>(); };
            for(const auto &panel:translated->GetPrimAtPath(bipedPath).GetChildren()) {
                const bool overflowPanel=panel.GetName().GetString().rfind("page_",0)==0;
                std::vector<Box> live;
                for(const auto &slot:panel.GetChildren()) {
                    SdfPathVector found,peers;
                    slot.GetRelationship(TfToken("rigExec:picker:controls")).GetTargets(&found);
                    slot.GetRelationship(TfToken("rigExec:picker:mirror")).GetTargets(&peers);
                    CHECK(!slot.HasAuthoredCustomData());
                    for(const auto &peer:peers) {
                        SdfPathVector back;
                        CHECK(translated->GetPrimAtPath(peer).GetRelationship(TfToken("rigExec:picker:mirror")).GetTargets(&back) && back==SdfPathVector{slot.GetPath()});
                    }
                    if(found.empty()) { CHECK(!overflowPanel); continue; }
                    live.push_back(box(slot));
                    for(const auto &path:found)all.insert(name(path));
                    if(found.size()>1) { groupButtons+=overflowPanel; continue; }
                    const auto role=slotRole.find(panel.GetName().GetString()+"/"+slot.GetName().GetString());
                    CHECK(overflowPanel || role!=slotRole.end());
                    if(overflowPanel) { ++singles[name(found[0])]; ++overflowButtons; }
                    else { CHECK(roles.emplace(role->second,name(found[0])).first->second==name(found[0])); slotted.insert(name(found[0])); }
                }
                CHECK(!live.empty());
                for(const auto &entry:clickable(live))CHECK(entry.second>=0.25);
            }
            // A role takes one control and a control one role; no control is on overflow twice or beside its slot.
            CHECK(slotted.size()==roles.size() && all.size()==controls && slotted.size()+singles.size()==controls);
            for(const auto &entry:singles)CHECK(entry.second==1 && !slotted.count(entry.first));
            const auto count=[&](const char *key) { return translated->GetPrimAtPath(bipedPath).GetCustomDataByKey(TfToken(key)).Get<int>(); };
            CHECK(count("blender:controlCount")==int(controls) && count("blender:bipedCount")==int(roles.size()));
            CHECK(count("blender:overflowCount")==overflowButtons && count("blender:groupCount")==groupButtons);
            return roles;
        };
        CHECK(resolvedRoles(bipedStage,18).size()==13);
        // Rigify: what the bundled rig types generate for the bundled metarig
        // bones. A name no generator emits (a character's own bone, or a
        // generated control with no biped counterpart) takes no slot.
        const auto rigify=resolvedRoles(roleStage({{"torso"},{"torso_pivot"},{"hips"},{"chest"},{"spine_fk.001"},{"spine_fk.002"},{"spine_fk.003"},
            {"tweak_spine.001"},{"neck"},{"head"},{"shoulder.L"},{"upper_arm_fk.L"},{"forearm_fk.L"},{"hand_fk.L"},{"hand_ik.L"},
            {"upper_arm_ik_target.L"},{"upper_arm_parent.L"},{"upper_arm_ik.L"},{"upper_arm_tweak.L"},{"upper_arm_tweak.L.001"},
            {"thigh_fk.L"},{"shin_fk.L"},{"foot_fk.L"},{"toe.L"},{"r_toe.R"},{"foot_ik.L"},{"thigh_ik_target.L"},{"foot_heel_ik.L"},
            {"thumb.01.L"},{"f_index.02.L"},{"palm.01_fk.L"},{"eye_common"},{"eye.L"},{"eye_master.L"},{"jaw_master"},{"jaw_master_mouth"},
            {"teeth.T"},{"nose_master"},{"nose.001"},{"brow.T.L.002"},{"lid.T.L.002"},{"lid.T.L"},{"lid.B.L"},{"lip.T"},{"lip.T.L.001"},{"lip_end.L.001"},
            {"cheek.B.L.001"},{"ear.L"},
            {"Root Offset01"},{"Eyelid01.T.L"},{"Jaw_mouth"},{"foot_spin_ik.L"},{"f_index.01_master.L"}}),54);
        CHECK((rigify==Roles{{"Main","Root"},{"M_Body","torso"},{"M_BodyGimbal","torso_pivot"},{"M_HipSwivel","hips"},{"M_Chest","chest"},
            {"M_Torso","spine_fk.001"},{"M_SpineMid","spine_fk.002"},{"M_ChestTop","spine_fk.003"},{"M_SpineBend1","tweak_spine.001"},
            {"M_Neck","neck"},{"M_Head","head"},{"L_Shldr","shoulder.L"},{"L_UpArm","upper_arm_fk.L"},{"L_LoArm","forearm_fk.L"},
            {"L_Hand","hand_fk.L"},{"L_ArmIK","hand_ik.L"},{"L_ArmPV","upper_arm_ik_target.L"},{"L_Arm","upper_arm_parent.L"},
            {"L_UpArmSwing","upper_arm_ik.L"},{"L_ArmBend0","upper_arm_tweak.L"},{"L_ArmBend1","upper_arm_tweak.L.001"},
            {"L_UpLeg","thigh_fk.L"},{"L_LoLeg","shin_fk.L"},{"L_Foot","foot_fk.L"},{"L_Toe","toe.L"},{"R_Toe","r_toe.R"},
            {"L_LegIK","foot_ik.L"},{"L_LegPV","thigh_ik_target.L"},{"L_Heel","foot_heel_ik.L"},{"L_ThumbBase","thumb.01.L"},
            {"L_IndexMid","f_index.02.L"},{"L_IndexMeta","palm.01_fk.L"},{"M_Look","eye_common"},{"L_Eye","eye.L"},{"M_Jaw","jaw_master"},
            {"M_Mouth","jaw_master_mouth"},{"teeth_upper_def","teeth.T"},{"M_Nose","nose_master"},{"M_NoseBridge","nose.001"},
            {"L_Brow","brow.T.L.002"},{"L_UpLid","lid.T.L.002"},{"M_UpLipMid","lip.T"},{"L_UpLip1","lip.T.L.001"},
            {"L_OutLip","lip_end.L.001"},{"L_Cheek","cheek.B.L.001"},{"L_Ear","ear.L"},
            // Lid corners by the bundled human metarig (rigify/metarigs/human.py):
            // eye.L head x 0.0516, lid.B.L 0.0262 towards the nose, lid.T.L 0.0768 away from it.
            {"L_InLid","lid.B.L"},{"L_OutLid","lid.T.L"}}));
        // CloudRig: component prefix on a bundled-metarig bone, current names
        // on the left side and the names before 4.1 on the right. The first
        // listed prefix wins among equals, a hidden unshaped bone never does,
        // and a hand-named face or prop control stays on overflow.
        const auto cloudRig=resolvedRoles(roleStage({{"TORSO-Spine"},{"MSTR-Spine_Torso"},{"HIP-Spine"},{"CHST-Spine",false,true},{"FK-Spine",false,true},
            {"FK-OS-Spine"},{"FK-RibCage"},{"FK-Chest"},{"STR-Spine"},{"FK-Neck"},{"STR-Neck"},{"FK-Head"},{"STR-Head"},{"Neck",false,false},
            {"FK-Shoulder.L"},{"FK-UpperArm.L"},{"FK-Forearm.L"},{"FK-Wrist.L"},{"IK-Wrist.L"},{"IK-C-Wrist.L"},{"POLE-UpperArm.L"},
            {"ROOT-UpperArm.L"},{"STR-UpperArm_1.L"},{"STR-Wrist.L"},{"IK-MSTR-Wrist.R"},{"IK-MSTR-C-Wrist.R"},{"IK-POLE-Arm.R"},{"STR-UpperArm1.R"},
            {"FK-Thigh.L"},{"FK-Knee.L"},{"FK-W-Foot.L"},{"FK-Foot.L"},{"FK-Toes.L"},{"IK-Foot.L"},{"POLE-Thigh.L"},{"ROOT-Thigh.L"},
            {"ROLL-M-Foot.L"},{"ROLL-Foot.L",false,false},{"IK-MSTR-Foot.R"},{"IK-POLE-Leg.R"},{"ROLL-Foot.R"},
            {"FK-Finger_Thumb1.L"},{"FK-Finger_Index3.L"},{"FK-Finger_Pinky_Carpal.L"},{"TGT-Eyes"},{"CTR-Eye.L"},{"TGT-Eye.L"},
            {"ACT-Eyelid_Upper.L"},{"ACT-Eyelid_Lower.L"},{"ACT-MouthCorner.L"},{"ACT-Eyebrow_Main.L"},{"Jaw"},
            {"MSTR-Mouth"},{"FK-Belly"},{"Chin_Main"},{"Eyebrow1.L"},{"CTR-Nose"},{"Nose",false,false},{"P-Button1"}}),60);
        CHECK((cloudRig==Roles{{"Main","Root"},{"M_Body","TORSO-Spine"},{"M_HipSwivel","HIP-Spine"},{"M_Chest","CHST-Spine"},{"M_Torso","FK-OS-Spine"},
            {"M_SpineMid","FK-RibCage"},{"M_ChestTop","FK-Chest"},{"M_SpineBend1","STR-Spine"},{"M_Neck","FK-Neck"},{"M_NeckShift","STR-Neck"},
            {"M_Head","FK-Head"},{"M_HeadGimbal","STR-Head"},{"L_Shldr","FK-Shoulder.L"},{"L_UpArm","FK-UpperArm.L"},{"L_LoArm","FK-Forearm.L"},
            {"L_Hand","FK-Wrist.L"},{"L_ArmIK","IK-Wrist.L"},{"L_ArmPV","POLE-UpperArm.L"},{"L_UpArmSwing","ROOT-UpperArm.L"},
            {"L_ArmBend0","STR-UpperArm_1.L"},{"L_ArmBend4","STR-Wrist.L"},{"R_ArmIK","IK-MSTR-Wrist.R"},{"R_ArmPV","IK-POLE-Arm.R"},
            {"R_ArmBend0","STR-UpperArm1.R"},{"L_UpLeg","FK-Thigh.L"},{"L_LoLeg","FK-Knee.L"},{"L_Foot","FK-W-Foot.L"},{"L_Toe","FK-Toes.L"},
            {"L_LegIK","IK-Foot.L"},{"L_LegPV","POLE-Thigh.L"},{"L_UpLegSwing","ROOT-Thigh.L"},{"L_BallRoll","ROLL-M-Foot.L"},
            {"R_LegIK","IK-MSTR-Foot.R"},{"R_LegPV","IK-POLE-Leg.R"},{"R_BallRoll","ROLL-Foot.R"},{"L_ThumbBase","FK-Finger_Thumb1.L"},
            {"L_IndexTip","FK-Finger_Index3.L"},{"L_PinkyMeta","FK-Finger_Pinky_Carpal.L"},{"M_Look","TGT-Eyes"},{"L_Eye","CTR-Eye.L"},
            {"L_UpLid","ACT-Eyelid_Upper.L"},{"L_LoLid","ACT-Eyelid_Lower.L"},{"L_Mouth","ACT-MouthCorner.L"},{"L_Brow","ACT-Eyebrow_Main.L"},
            {"M_Jaw","Jaw"}}));
        // The bundled human metarig renamed its spine and hand: Spine1..3 in
        // 4.5; Hips, Spine, Chest and Hand on main. Spine is the lowest bone
        // unless the chain starts at Hips.
        CHECK((resolvedRoles(roleStage({{"TORSO-Spine1"},{"HIP-Spine1"},{"CHST-Spine1"},{"FK-Spine1"},{"FK-Spine2"},{"FK-Spine3"},
            {"STR-Spine1"},{"STR-Spine2"},{"STR-Spine3"}}),10)==Roles{{"Main","Root"},{"M_Body","TORSO-Spine1"},{"M_HipSwivel","HIP-Spine1"},
            {"M_Chest","CHST-Spine1"},{"M_Torso","FK-Spine1"},{"M_SpineMid","FK-Spine2"},{"M_ChestTop","FK-Spine3"},
            {"M_SpineBend1","STR-Spine1"},{"M_SpineBend2","STR-Spine2"},{"M_SpineBend3","STR-Spine3"}}));
        CHECK((resolvedRoles(roleStage({{"TORSO-Hips"},{"HIP-Hips"},{"CHST-Hips"},{"FK-Hips"},{"FK-Spine"},{"FK-Chest"},
            {"STR-Hips"},{"STR-Spine"},{"STR-Chest"},{"FK-Hand.L"},{"IK-Hand.L"},{"STR-Hand.L"}}),13)==Roles{{"Main","Root"},
            {"M_Body","TORSO-Hips"},{"M_HipSwivel","HIP-Hips"},{"M_Chest","CHST-Hips"},{"M_Torso","FK-Hips"},{"M_SpineMid","FK-Spine"},
            {"M_ChestTop","FK-Chest"},{"M_SpineBend1","STR-Hips"},{"M_SpineBend2","STR-Spine"},{"M_SpineBend3","STR-Chest"},
            {"L_Hand","FK-Hand.L"},{"L_ArmIK","IK-Hand.L"},{"L_ArmBend4","STR-Hand.L"}}));
        CHECK((resolvedRoles(roleStage({{"FK-Spine"},{"FK-Chest"},{"STR-Spine"},{"STR-Chest"}}),5)==Roles{{"Main","Root"},
            {"M_Torso","FK-Spine"},{"M_ChestTop","FK-Chest"},{"M_SpineBend1","STR-Spine"},{"M_SpineBend3","STR-Chest"}}));
        // Overflow is cut by count once the placed controls are gone, never
        // by a page name, and a source group button leads its page: 50 loose
        // controls and one group make 51 entries, 48 and 3.
        {
            std::vector<std::string> extraNames; std::vector<Bone> many{{"hand_ik.L"}};
            for(int i=0;i<50;++i)extraNames.push_back("Prop"+std::to_string(100+i));
            for(const auto &extra:extraNames)many.push_back({extra.c_str()});
            auto chunked=roleStage(many,JsArray{JsValue(JsObject{{"label",JsValue("Left hand and prop")},
                {"controls",JsValue(JsArray{JsValue("hand_ik.L"),JsValue("Prop100")})}})});
            CHECK((resolvedRoles(chunked,52)==Roles{{"Main","Root"},{"L_ArmIK","hand_ik.L"}}));
            std::vector<std::string> labels; std::vector<size_t> sizes;
            for(const auto &panel:chunked->GetPrimAtPath(bipedPath).GetChildren()) {
                std::string label; panel.GetAttribute(TfToken("ui:label")).Get(&label);
                size_t buttons=0; for(const auto &child:panel.GetChildren())buttons+=bool(child);
                labels.push_back(label); sizes.push_back(buttons);
            }
            CHECK((std::vector<std::string>(labels.begin()+1,labels.end())==std::vector<std::string>{"Main 1/2","Main 2/2"}));
            CHECK(sizes[1]==48 && sizes[2]==3);
            // The group selects a control with a biped slot and one without: the whole source set.
            SdfPathVector members;
            CHECK(chunked->GetPrimAtPath(bipedPath.AppendPath(SdfPath("page_0/button_0"))).GetRelationship(TfToken("rigExec:picker:controls")).GetTargets(&members));
            CHECK(members.size()==2);
        }
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
        // Snapshot JSON is embedded on request only; bones keep a deform flag.
        CHECK(!stage->GetDefaultPrim().HasCustomDataKey(TfToken("blender:sourceData")));
        for(const auto &prim:stage->Traverse()) {
            CHECK(!prim.HasCustomDataKey(TfToken("blender:source")));
            CHECK((prim.GetTypeName()==TfToken("RigExecJoint"))==prim.GetCustomDataByKey(TfToken("blender:deform")).IsHolding<bool>());
        }
        auto dumped=SdfLayer::CreateAnonymous("dump.blendrig",{{"sourceData","1"},{"strict","1"}});
        CHECK(dumped->ImportFromString(buffer.str()));
        const auto dump=UsdStage::Open(dumped)->GetDefaultPrim().GetCustomDataByKey(TfToken("blender:sourceData"));
        CHECK(dump.IsHolding<std::string>()); CHECK(JsParseString(dump.Get<std::string>())==json);
        auto plain=SdfLayer::CreateAnonymous("plain.blendrig",{{"sourceData","0"}});
        CHECK(plain->ImportFromString(buffer.str()));
        std::string plainText; CHECK(plain->ExportToString(&plainText)); CHECK(plainText==original);
        auto badDump=SdfLayer::CreateAnonymous("bad-dump.blendrig",{{"sourceData","yes"}});
        { TfErrorMark errors; CHECK(!badDump->ImportFromString(buffer.str())); errors.Clear(); }
        // pruneUnobserved is opt-in: an armature nothing observes is omitted
        // with one diagnostic, every surviving prim keeps its path, and any
        // reference from a kept node keeps the whole armature.
        {
            auto hidden=json.GetJsObject(); auto hiddenNodes=hidden["nodes"].GetJsArray();
            auto object=hiddenNodes[1].GetJsObject(); object["id"]=JsValue("Meta"); object["name"]=JsValue("Meta");
            hiddenNodes.push_back(JsValue(object));
            for(const auto &link:std::vector<std::pair<std::string,std::string>>{{"Meta/A","Meta"},{"Meta/B","Meta/A"}}) {
                auto bone=hiddenNodes[2].GetJsObject(); bone["id"]=JsValue(link.first); bone["name"]=JsValue(link.first);
                bone["parent"]=JsValue(link.second); bone["armature"]=JsValue("Meta"); bone["visible"]=JsValue(false);
                hiddenNodes.push_back(JsValue(bone));
            }
            hidden["nodes"]=JsValue(hiddenNodes);
            auto hiddenCons=hidden["constraints"].GetJsArray(); auto follow=hiddenCons[0].GetJsObject();
            follow["owner"]=JsValue("Meta/B"); follow["source"]=JsValue("Meta/A"); hiddenCons.push_back(JsValue(follow));
            hidden["constraints"]=JsValue(hiddenCons);
            auto read=[&](const JsObject &data,const char *value,std::string *text) {
                auto layer=value ? SdfLayer::CreateAnonymous("prune.blendrig",{{"pruneUnobserved",value}}) : SdfLayer::CreateAnonymous("prune.blendrig");
                CHECK(layer->ImportFromString(JsWriteToString(JsValue(data))));
                if(text) CHECK(layer->ExportToString(text));
                return layer;
            };
            auto ids=[](const SdfLayerRefPtr &layer) {
                std::set<std::pair<std::string,SdfPath>> paths; const auto opened=UsdStage::Open(layer);
                for(const auto &prim:opened->Traverse()) {
                    const auto id=prim.GetCustomDataByKey(TfToken("blender:id"));
                    if(id.IsHolding<std::string>()) paths.emplace(id.Get<std::string>(),prim.GetPath());
                }
                return paths;
            };
            std::string full,off,pruned,observed,reference;
            auto all=ids(read(hidden,nullptr,&full)); read(hidden,"0",&off); CHECK(off==full);
            auto prunedLayer=read(hidden,"1",&pruned); const auto kept=ids(prunedLayer);
            auto named=[](const std::set<std::pair<std::string,SdfPath>> &paths,const std::string &id) {
                return std::count_if(paths.begin(),paths.end(),[&](const auto &entry) { return entry.first==id; });
            };
            CHECK(named(all,"Meta/A")==1 && named(all,"Meta/B")==1 && named(all,"Meta")==1);
            CHECK(named(kept,"Meta/A")==0 && named(kept,"Meta/B")==0 && named(kept,"Meta")==1);
            CHECK(kept.size()+2==all.size());
            CHECK(std::includes(all.begin(),all.end(),kept.begin(),kept.end()));
            const auto prunedDiagnostics=prunedLayer->GetCustomLayerData()["blenderRig:diagnostics"].Get<VtStringArray>();
            CHECK(std::count(prunedDiagnostics.begin(),prunedDiagnostics.end(),"unobserved armature omitted (pruneUnobserved): Meta (2 bones)")==1);
            CHECK(!prunedLayer->GetCustomLayerData()["blenderRig:complete"].Get<bool>());
            CHECK(pruned.find("Meta_B")==std::string::npos);
            auto rider=hiddenNodes[4].GetJsObject(); rider["id"]=JsValue("Rider"); rider["name"]=JsValue("Rider"); rider["parent"]=JsValue("Meta/B");
            hiddenNodes.push_back(JsValue(rider)); hidden["nodes"]=JsValue(hiddenNodes);
            read(hidden,nullptr,&reference); read(hidden,"1",&observed); CHECK(observed==reference);
            auto badPrune=SdfLayer::CreateAnonymous("bad-prune.blendrig",{{"pruneUnobserved","yes"}});
            { TfErrorMark errors; CHECK(!badPrune->ImportFromString(buffer.str())); errors.Clear(); }
        }
        // A bone with a non-rigid rest keeps its neutral Blender control-space
        // provider; its rigid neighbour still switches natively.
        {
            auto sheared=withPicker; sheared.erase("pickers");
            auto shearedNodes=sheared["nodes"].GetJsArray();
            auto bone=shearedNodes[3].GetJsObject(); auto rest=bone["rest"].GetJsArray();
            for(size_t i=0;i<3;++i) rest[i]=JsValue(2.0*(rest[i].IsInt() ? rest[i].GetInt() : rest[i].GetReal()));
            bone["rest"]=JsValue(rest); shearedNodes[3]=JsValue(bone); sheared["nodes"]=JsValue(shearedNodes);
            auto shearedLayer=SdfLayer::CreateAnonymous("sheared.blendrig");
            CHECK(shearedLayer->ImportFromString(JsWriteToString(JsValue(sheared))));
            auto shearedStage=UsdStage::Open(shearedLayer); int native=0,neutral=0;
            for(const auto &prim:shearedStage->Traverse()) {
                if(prim.GetTypeName()!=TfToken("RigExecJoint"))continue;
                SdfPathVector targets,connections,sources;
                CHECK(prim.GetRelationship(TfToken("blender:channelControl")).GetTargets(&targets)); CHECK(targets.size()==1);
                const auto name=targets.front().GetParentPath().GetNameToken();
                const auto defaultSpace=shearedStage->GetPrimAtPath(targets.front()).GetAttribute(TfToken("default:space"));
                defaultSpace.GetConnections(&connections);
                CHECK(shearedStage->GetPrimAtPath(SdfPath("/Rig/Mechanisms").AppendChild(TfToken("controlSpace_"+name.GetString())))
                      .GetRelationship(TfToken("rigExec:sources")).GetTargets(&sources)); CHECK(sources.size()==1);
                const auto space=SdfPath("/Rig/ControlSpaces").AppendChild(name),frame=SdfPath("/Rig/Mechanisms").AppendChild(TfToken("controlFrame_"+name.GetString()));
                const bool shearedBone=prim.GetCustomDataByKey(TfToken("blender:id")).Get<std::string>()==bone.at("id").GetString();
                if(shearedBone) CHECK(connections==SdfPathVector{space.AppendProperty(TfToken("default:space"))});
                else CHECK(connections.empty() ? defaultSpace.HasAuthoredValue() : connections==SdfPathVector{SdfPath("/Rig/Mechanisms/identityParent.outputs:matrix")});
                CHECK(sources.front()==(shearedBone ? space : prim.GetPath().GetParentPath()));
                CHECK(bool(shearedStage->GetPrimAtPath(space))==shearedBone);
                CHECK(bool(shearedStage->GetPrimAtPath(frame))==shearedBone);
                ++(shearedBone ? neutral : native);
            }
            CHECK(native==1); CHECK(neutral==1);
        }
        // Connected lower-limb calibration probes a dependency closure of the
        // legs, not the rig. On Rigify-convention legs that closure must be
        // usable, and the whole-rig probe it replaces, requested here with
        // probeWholeRig=1 and always diagnosed, must author the very same layer.
        {
            std::ifstream legInput(std::string(argv[3])+"/leg.blendrig"); std::stringstream legText; legText<<legInput.rdbuf();
            const auto read=[&](bool whole,std::string *text) {
                auto layer=whole ? SdfLayer::CreateAnonymous("leg.blendrig",{{"probeWholeRig","1"}}) : SdfLayer::CreateAnonymous("leg.blendrig");
                CHECK(layer->ImportFromString(legText.str()));
                const auto diagnostics=layer->GetCustomLayerData()["blenderRig:diagnostics"].Get<VtStringArray>();
                const auto count=[&](const std::string &part) { return std::count_if(diagnostics.begin(),diagnostics.end(),
                    [&](const std::string &d) { return d.find(part)!=std::string::npos; }); };
                CHECK(count("calibration skipped")==0); CHECK(count("use calibrated native frames")==1);
                CHECK(count("calibration probed the whole rig")==(whole ? 1 : 0));
                CHECK(count("probed the whole rig: probeWholeRig=1")==(whole ? 1 : 0));
                // Everything but the diagnostics, which name the probe.
                auto copy=SdfLayer::CreateAnonymous("leg.usda"); copy->TransferContent(layer);
                copy->SetCustomLayerData({}); copy->GetAttributeAtPath(SdfPath("/Rig.blender:diagnostics"))->ClearDefaultValue();
                CHECK(copy->ExportToString(text));
                return layer;
            };
            std::string closure,whole;
            const auto legStage=UsdStage::Open(read(false,&closure)); CHECK(legStage);
            read(true,&whole); CHECK(closure==whole);
            // A copy-only stack follows its last source, a Stretch To stack its first.
            std::map<std::string,std::string> calibrated;
            for(const auto &prim:legStage->Traverse()) {
                if(prim.GetTypeName()!=TfToken("RigExecJoint"))continue;
                SdfPathVector posed,source; prim.GetAttribute(TfToken("posed:space")).GetConnections(&posed);
                if(posed.size()!=1 || !TfStringStartsWith(posed.front().GetPrimPath().GetName(),"calibrated_leg_"))continue;
                const auto mapped=legStage->GetPrimAtPath(posed.front().GetPrimPath());
                CHECK(mapped.GetTypeName()==TfToken("RigExecMappedFrame"));
                CHECK(mapped.GetRelationship(TfToken("rigExec:source")).GetTargets(&source) && source.size()==1);
                for(const char *rest:{"inputs:targetRest","inputs:sourceRest"}) {
                    GfMatrix4d value(0.0); CHECK(mapped.GetAttribute(TfToken(rest)).Get(&value)); CHECK(std::abs(value.GetDeterminant())>1e-6);
                }
                calibrated[prim.GetCustomDataByKey(TfToken("blender:id")).Get<std::string>()]=
                    legStage->GetPrimAtPath(source.front()).GetCustomDataByKey(TfToken("blender:id")).Get<std::string>();
            }
            CHECK((calibrated==std::map<std::string,std::string>{
                {"Rig/ORG-shin.L","Rig/MCH-shin_ik.L"},{"Rig/ORG-shin.R","Rig/MCH-shin_ik.R"},
                {"Rig/ORG-foot.L","Rig/MCH-thigh_ik_target.L"},{"Rig/ORG-foot.R","Rig/MCH-thigh_ik_target.R"},
                {"Rig/DEF-shin.L","Rig/shin_tweak.L"},{"Rig/DEF-shin.R","Rig/shin_tweak.R"}}));
        }
        // Anonymous USDA serialization can be reopened without the source reader.
        auto usd=SdfLayer::CreateAnonymous("native.usda"); CHECK(usd->ImportFromString(original));
        CHECK(UsdStage::Open(usd)->GetDefaultPrim().GetTypeName()==TfToken("RigExecRoot"));
        // A signed shape key blends its negative range from a mover-inverted weight,
        // which that branch must read after movers; the positive branch reads the channel.
        {
            auto shaped=json.GetJsObject(); auto shapedMeshes=shaped["meshes"].GetJsArray(); auto shapedMesh=shapedMeshes[0].GetJsObject();
            JsArray deltas(shapedMesh.at("points").GetJsArray().size(),JsValue(JsArray{JsValue(0.0),JsValue(0.0),JsValue(0.25)}));
            const auto key=[&](const char *name,double minimum) { return JsValue(JsObject{{"name",JsValue(name)},{"value",JsValue(0.0)},
                {"mute",JsValue(false)},{"minimum",JsValue(minimum)},{"maximum",JsValue(1.0)},{"deltas",JsValue(deltas)}}); };
            shapedMesh["shape_keys"]=JsValue(JsArray{key("Signed",-1.0),key("Unsigned",0.0)});
            shapedMeshes[0]=JsValue(shapedMesh); shaped["meshes"]=JsValue(shapedMeshes);
            auto shapedLayer=SdfLayer::CreateAnonymous("shaped.blendrig");
            TfErrorMark mark;
            CHECK(shapedLayer->ImportFromString(JsWriteToString(JsValue(shaped))));
            auto shapedStage=UsdStage::Open(shapedLayer); CHECK(shapedStage); CHECK(mark.IsClean());
            std::map<std::string,std::string> phases;
            for(const auto &prim:shapedStage->Traverse()) if(prim.GetTypeName()==TfToken("RigExecBlendInput")) {
                auto weight=prim.GetAttribute(TfToken("inputs:weight")); SdfPathVector sources;
                CHECK(weight.GetConnections(&sources) && sources.size()==1);
                auto channel=shapedStage->GetAttributeAtPath(sources.front()); CHECK(channel);
                const bool inverted=channel.GetCustomDataByKey(TfToken("blender:shapeKey")).IsEmpty();
                CHECK(inverted==TfStringEndsWith(prim.GetName().GetString(),"_negative"));
                if(inverted) {
                    const std::string name=channel.GetName().GetString(),prefix="blender:shapeKeyNegative:";
                    CHECK(name.find(prefix)==0);
                    channel=channel.GetPrim().GetAttribute(TfToken("blender:shapeKey:"+name.substr(prefix.size()))); CHECK(channel);
                }
                std::string phase; weight.GetMetadata(TfToken("rigExecReadPhase"),&phase);
                phases[channel.GetCustomDataByKey(TfToken("blender:shapeKey")).Get<std::string>()+(inverted ? "-" : "+")]=phase;
            }
            CHECK((phases==std::map<std::string,std::string>{{"Signed-","final"},{"Signed+",""},{"Unsigned+",""}}));
        }
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
