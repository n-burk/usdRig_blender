#include "translator.h"
#include "rigExecRigging/rigBuilder.h"
#include "rigExecRigging/schemaAuthoring.h"
#include "rigExec/rigEvaluator.h"
#include "pxr/base/js/json.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/usd/ar/resolvedPath.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/subset.h"
#include "pxr/usd/usdShade/material.h"
#include "pxr/usd/usdShade/materialBindingAPI.h"
#include "pxr/usd/usdShade/shader.h"
#include "pxr/usd/usdShade/udimUtils.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE
namespace usdBlenderRig {
namespace {
constexpr size_t MaxElements=10000000;
const JsObject &Object(const JsValue &v) {
    if(!v.IsObject()) throw std::runtime_error("expected JSON object");
    return v.GetJsObject();
}
const JsArray &Array(const JsValue &v) {
    if(!v.IsArray() || v.GetJsArray().size()>MaxElements) throw std::runtime_error("invalid JSON array");
    return v.GetJsArray();
}
const JsValue &Field(const JsValue &v,const std::string &key) {
    const auto &obj=Object(v); const auto it=obj.find(key);
    if(it==obj.end()) throw std::runtime_error("missing snapshot field: "+key);
    return it->second;
}
bool Has(const JsValue &v,const std::string &key) { return Object(v).count(key)!=0; }
std::string String(const JsValue &v) {
    if(!v.IsString()) throw std::runtime_error("expected JSON string");
    if(v.GetString().find('\0')!=std::string::npos) throw std::runtime_error("embedded NUL in string");
    return v.GetString();
}
std::string Str(const JsValue &v,const std::string &key) { return String(Field(v,key)); }
double Number(const JsValue &v) {
    double d;
    if(v.IsUInt64()) d=static_cast<double>(v.GetUInt64());
    else if(v.IsInt()) d=static_cast<double>(v.GetInt64());
    else if(v.IsReal()) d=v.GetReal();
    else throw std::runtime_error("expected JSON number");
    if(!std::isfinite(d)) throw std::runtime_error("non-finite number");
    return d;
}
int Integer(const JsValue &v) {
    const double d=Number(v);
    if(std::floor(d)!=d || d<std::numeric_limits<int>::min() || d>std::numeric_limits<int>::max())
        throw std::runtime_error("invalid integer");
    return static_cast<int>(d);
}
float Float(const JsValue &v) {
    const double d=Number(v);
    if(std::abs(d)>std::numeric_limits<float>::max()) throw std::runtime_error("float overflow");
    return static_cast<float>(d);
}
bool Bool(const JsValue &v) {
    if(!v.IsBool()) throw std::runtime_error("expected JSON boolean");
    return v.GetBool();
}
bool Flag(const JsValue &v,const std::string &key,bool fallback) {
    return Has(v,key) ? Bool(Field(v,key)) : fallback;
}
double Num(const JsValue &v,const std::string &key,double fallback) {
    return Has(v,key) ? Number(Field(v,key)) : fallback;
}
GfMatrix4d Matrix(const JsValue &v) {
    const auto &a=Array(v); if(a.size()!=16) throw std::runtime_error("matrix must contain 16 numbers");
    GfMatrix4d result(1);
    for(int i=0;i<16;++i) result[i/4][i%4]=Number(a[i]);
    if(std::abs(result.GetDeterminant())<1e-12 || result[0][3]!=0 || result[1][3]!=0 ||
       result[2][3]!=0 || result[3][3]!=1) throw std::runtime_error("singular or non-affine rest matrix");
    return result;
}
GfVec3f Point(const JsValue &v) {
    const auto &a=Array(v); if(a.size()!=3) throw std::runtime_error("point must contain 3 numbers");
    return GfVec3f(Float(a[0]),Float(a[1]),Float(a[2]));
}
// UV property identifiers remain reversible; prim names need not encode IDs.
std::string PropertyName(const std::string &s) {
    std::string result="n"; const char *hex="0123456789abcdef";
    for(unsigned char c:s) {
        if((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9')) result+=c;
        else { result+='_'; result+=hex[c>>4]; result+=hex[c&15]; }
    }
    return result;
}
std::map<std::string,std::string> PrimNames(const std::map<std::string,std::string> &labels) {
    std::map<std::string,std::vector<std::string>> groups;
    for(const auto &entry:labels) {
        std::string base;
        for(unsigned char c:entry.second) {
            const bool valid=(c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9');
            if(valid) base+=c;
            else if(!base.empty() && base.back()!='_') base+='_' ;
            if(base.size()==32) break;
        }
        while(!base.empty() && base.back()=='_') base.pop_back();
        if(base.empty()) base="node";
        if(base.front()>='0' && base.front()<='9') base="n_"+base;
        groups[base].push_back(entry.first);
    }
    std::map<std::string,std::string> result;
    // Reserve unambiguous labels first so a disambiguation cannot steal one.
    std::set<std::string> used{"EditorFrame","Display"};
    for(const auto &group:groups) if(group.second.size()==1 && !used.count(group.first)) {
        result[group.second.front()]=group.first;used.insert(group.first);
    }
    for(const auto &group:groups) for(const auto &id:group.second) {
        if(result.count(id)) continue;
        uint64_t hash=14695981039346656037ull;
        for(unsigned char c:id) { hash^=c;hash*=1099511628211ull; }
        std::ostringstream suffix;suffix<<std::hex<<std::setw(8)<<std::setfill('0')<<uint32_t(hash);
        const auto stem=group.first+"_"+suffix.str();auto name=stem;
        for(size_t collision=1;!used.insert(name).second;++collision) name=stem+"_"+std::to_string(collision);
        result[id]=name;
    }
    return result;
}
bool Rigid(const GfMatrix4d &m) {
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) {
        double dot=0; for(int k=0;k<3;++k) dot+=m[i][k]*m[j][k];
        if(std::abs(dot-(i==j ? 1.0 : 0.0))>1e-6) return false;
    }
    return m.GetDeterminant()>0;
}
GfMatrix4d PoseMatrix(const JsArray &pose) {
    if(pose.size()!=9) throw std::runtime_error("pose must contain 9 TRS channels");
    GfMatrix4d result(1);result.SetScale(GfVec3d(Number(pose[6]),Number(pose[7]),Number(pose[8])));
    for(int i=0;i<3;++i) {GfVec3d axis(0);axis[i]=1;result*=GfMatrix4d(GfRotation(axis,Number(pose[i+3])),GfVec3d(0));}
    result.SetTranslateOnly(GfVec3d(Number(pose[0]),Number(pose[1]),Number(pose[2])));
    return result;
}
struct Node {
    JsValue data;
    std::string id,parent,kind,name;
    SdfPath path,controlPath;
    GfMatrix4d rest{1},world{1};
    int state=0;
};
class Importer {
public:
    JsValue scene;
    std::string source;
    UsdStageRefPtr stage=UsdStage::CreateInMemory();
    rigExec::RigExecRigBuilder rig=rigExec::RigExecRigBuilder::Create(stage);
    std::map<std::string,Node> nodes;
    std::map<std::string,UsdShadeMaterial> materials;
    std::map<std::string,double> guideExtents;
    std::vector<std::string> diagnostics,dependencies;
    TfTokenVector operationOrder;
    explicit Importer(JsValue data,std::string origin):scene(std::move(data)),source(std::move(origin)) {}
    void Warn(const std::string &message) { diagnostics.push_back(message); }
    void AbsolutePoseParent(const UsdPrim &prim) {
        const auto identity=stage->DefinePrim(SdfPath("/Rig/Mechanisms/identityParent"),TfToken("Scope"));
        auto matrix=identity.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);
        matrix.Set(GfMatrix4d(1));
        // A connected parent space is an explicit inheritance boundary in
        // usdRig, including when its value is identity. A literal identity
        // leaves namespace propagation enabled and duplicates pose refresh.
        prim.GetAttribute(TfToken("parent:space")).SetConnections({matrix.GetPath()});
    }
    Node &Find(const std::string &id) {
        auto it=nodes.find(id);
        if(it==nodes.end()) throw std::runtime_error("unresolved rig node: "+id);
        return it->second;
    }
    rigExec::RigExecMoverChain Chain(const std::string &name,const SdfPath &target={}) {
        // Evaluation walks reverse composed order. The authored reorder is
        // the reverse of source application order, including constraint stacks.
        operationOrder.insert(operationOrder.begin(),TfToken(name));
        return rig.NewMoverChain(name,target);
    }
    bool HasActiveConstraint(const Node &node) const {
        for(const auto &constraint:Array(Field(scene,"constraints")))
            if(Str(constraint,"owner")==node.id && Flag(constraint,"enabled",true) &&
               Number(Field(constraint,"influence"))>0) return true;
        return false;
    }
    bool EditableBoneControl(const Node &bone) const {
        if(bone.kind!="joint" || !Has(bone.data,"armature") ||
           Flag(bone.data,"connected",false) || !Flag(bone.data,"local_location",true)) return false;
        if(!bone.parent.empty()) {
            const auto &parent=nodes.at(bone.parent);
            if(parent.kind=="joint" &&
               (!Flag(bone.data,"inherit_rotation",true) ||
                (Has(bone.data,"inherit_scale") && Str(bone.data,"inherit_scale")!="FULL"))) return false;
        }
        return true;
    }
    void Build(Node &n,size_t depth=0) {
        if(depth>512) throw std::runtime_error("rig hierarchy exceeds 512 levels");
        if(n.state==2) return;
        if(n.state==1) throw std::runtime_error("rig hierarchy cycle: "+n.id);
        n.state=1;
        const auto &pose=Array(Field(n.data,"pose"));
        GfMatrix4d bindLocal=n.rest;
        if(Flag(n.data,"pose_is_object_basis",false)) bindLocal=PoseMatrix(pose)*bindLocal;
        SdfPath parent("/Rig/Dag");
        if(!n.parent.empty()) { auto &p=Find(n.parent); Build(p,depth+1); parent=p.path; n.world=bindLocal*p.world; }
        else n.world=bindLocal;
        n.path=parent.AppendChild(TfToken(n.name));
        n.controlPath=n.path;
        auto schema=rigExec::RigExecSchemaPrim::Define(stage,n.path,TfToken(n.kind=="joint" ? "RigExecJoint" : "RigExecControl"));
        schema.ApplyAPI(TfToken("NodeGraphNodeAPI"));
        schema.ApplyAPI(TfToken("RigExecControlAPI"));
        schema.SetAttribute(TfToken("rest:space"),VtValue(n.rest));
        auto prim=schema.GetPrim(); prim.SetDisplayName(Str(n.data,"name"));
        UsdGeomImageable(prim).CreatePurposeAttr().Set(UsdGeomTokens->guide);
        prim.SetCustomDataByKey(TfToken("blender:id"),VtValue(n.id));
        prim.SetCustomDataByKey(TfToken("blender:source"),VtValue(JsWriteToString(n.data)));
        if(!Rigid(n.rest)) Warn("rest scale/shear/reflection retained; RigExec orthonormalizes rest frames: "+n.id);
        if(pose.size()!=9) throw std::runtime_error("pose must contain 9 TRS channels");
        const char *channels[]={"tx","ty","tz","rx","ry","rz","sx","sy","sz"};
        for(int i=0;i<9;++i) {
            double value=Number(pose[i]);
            if(i>=6 && std::abs(value)<1e-4) Warn("near-zero pose scale is clamped by RigExec: "+n.id);
            schema.SetAttribute(TfToken(std::string("avars:")+channels[i]),VtValue(value));
        }
        schema.SetAttribute(TfToken("avars:rotationOrder"),VtValue(TfToken("XYZ")));
        const bool visible=Flag(n.data,"visible",true);
        if(n.kind=="control") schema.SetAttribute(TfToken("guide:displayOpacity"),VtValue(
            visible && Has(n.data,"object_type") && Str(n.data,"object_type")=="EMPTY" ? 0.8f : 0.0f));
        else schema.SetAttribute(TfToken("guide:radius"),VtValue(
            visible && !Has(n.data,"guide_points") ? Num(n.data,"length",1)*0.04 : 0.0));
        // USD visibility propagates down namespace ancestry, unlike Blender
        // bone collection visibility. Hide each guide, not its pose provider.
        n.state=2;
    }
    void Graph() {
        stage->DefinePrim(SdfPath("/Rig/Dag"),TfToken("Scope"));
        stage->DefinePrim(SdfPath("/Rig/Controls"),TfToken("Scope"));
        stage->DefinePrim(SdfPath("/Rig/ControlSpaces"),TfToken("Scope"));
        for(const auto &data:Array(Field(scene,"nodes"))) {
            Node n; n.data=data; n.id=Str(data,"id"); n.parent=Str(data,"parent");
            n.kind=Str(data,"kind"); n.rest=Matrix(Field(data,"rest"));
            if(n.id.empty() || (n.kind!="control" && n.kind!="joint")) throw std::runtime_error("invalid node identity/kind");
            if(n.kind=="joint" && Num(data,"length",1)<=0) throw std::runtime_error("non-positive bone length");
            if(!nodes.emplace(n.id,std::move(n)).second) throw std::runtime_error("duplicate node ID");
        }
        std::map<std::string,std::string> labels;
        for(const auto &entry:nodes) labels[entry.first]=Str(entry.second.data,"name");
        const auto names=PrimNames(labels);
        for(auto &entry:nodes) entry.second.name=names.at(entry.first);
        for(auto &entry:nodes) Build(entry.second);
        for(auto &entry:nodes) {
            auto &bone=entry.second;
            if(bone.kind!="joint" || !Has(bone.data,"armature")) continue;
            const auto mode=Has(bone.data,"inherit_scale") ? Str(bone.data,"inherit_scale") : "FULL";
            if(!std::set<std::string>{"FULL","NONE","AVERAGE","ALIGNED","FIX_SHEAR","NONE_LEGACY"}.count(mode))
                throw std::runtime_error("unknown bone inheritance mode: "+mode);
            auto &object=Find(Str(bone.data,"armature"));
            auto mechanism=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("bone_"+bone.name)),TfToken("RigExecBlenderBoneFrame"));
            mechanism.SetAttribute(TfToken("inputs:local"),VtValue(bone.rest));
            mechanism.SetAttribute(TfToken("inputs:inheritScale"),VtValue(TfToken(mode)));
            mechanism.SetAttribute(TfToken("inputs:inheritRotation"),VtValue(Flag(bone.data,"inherit_rotation",true)));
            mechanism.SetAttribute(TfToken("inputs:localLocation"),VtValue(Flag(bone.data,"local_location",true)));
            mechanism.SetAttribute(TfToken("inputs:connected"),VtValue(Flag(bone.data,"connected",false)));
            SdfPathVector dependencies={object.path};
            auto &parent=Find(bone.parent);
            if(parent.kind=="joint") {
                mechanism.SetAttribute(TfToken("inputs:hasParent"),VtValue(true));
                mechanism.SetAttribute(TfToken("inputs:parentRest"),VtValue(parent.world*object.world.GetInverse()));
                mechanism.SetRelationship(TfToken("rigExec:parent"),{parent.path});dependencies.push_back(parent.path);
            }
            mechanism.SetRelationship(TfToken("rigExec:sourceObject"),{object.path});
            mechanism.SetRelationship(TfToken("rigExec:poseInputs"),dependencies);
            for(const auto *channel:{"tx","ty","tz","rx","ry","rz","sx","sy","sz"})
                mechanism.GetPrim().GetAttribute(TfToken(std::string("inputs:")+channel)).SetConnections({bone.path.AppendProperty(TfToken(std::string("avars:")+channel))});
            // This expression already returns an absolute world frame and
            // explicitly reads its live parent. Namespace pose propagation
            // must stop here; dependency refresh computes inheritance once.
            AbsolutePoseParent(stage->GetPrimAtPath(bone.path));
            stage->GetPrimAtPath(bone.path).GetAttribute(TfToken("posed:space")).SetConnections({mechanism.GetPath().AppendProperty(TfToken("outputs:matrix"))});
            const bool splitChannels=!EditableBoneControl(bone);
            const SdfPath framePath=SdfPath("/Rig/ControlSpaces").AppendChild(TfToken(bone.name));
            auto frame=rigExec::RigExecSchemaPrim::Define(stage,framePath,TfToken("RigExecControl"));
            frame.SetAttribute(TfToken("rest:space"),VtValue(GfMatrix4d(1)));
            frame.SetAttribute(TfToken("default:space"),VtValue(GfMatrix4d(1)));
            frame.SetAttribute(TfToken("guide:displayOpacity"),VtValue(0.0f));
            auto neutral=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("controlFrame_"+bone.name)),TfToken("RigExecBlenderBoneFrame"));
            neutral.SetAttribute(TfToken("inputs:local"),VtValue(bone.rest));
            neutral.SetAttribute(TfToken("inputs:inheritScale"),VtValue(TfToken(mode)));
            neutral.SetAttribute(TfToken("inputs:inheritRotation"),VtValue(Flag(bone.data,"inherit_rotation",true)));
            neutral.SetAttribute(TfToken("inputs:localLocation"),VtValue(Flag(bone.data,"local_location",true)));
            neutral.SetAttribute(TfToken("inputs:connected"),VtValue(false));
            neutral.SetRelationship(TfToken("rigExec:sourceObject"),{object.path});
            neutral.SetRelationship(TfToken("rigExec:poseInputs"),dependencies);
            if(parent.kind=="joint") {
                neutral.SetAttribute(TfToken("inputs:hasParent"),VtValue(true));
                neutral.SetAttribute(TfToken("inputs:parentRest"),VtValue(parent.world*object.world.GetInverse()));
                neutral.SetRelationship(TfToken("rigExec:parent"),{parent.path});
            }
            frame.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({neutral.GetPath().AppendProperty(TfToken("outputs:matrix"))});
            const SdfPath controlParent=SdfPath("/Rig/Controls").AppendChild(TfToken(bone.name));
            stage->DefinePrim(controlParent,TfToken("Scope"));
            const SdfPath controlPath=controlParent.AppendChild(TfToken("Control"));
            auto control=rigExec::RigExecSchemaPrim::Define(stage,controlPath,TfToken("RigExecControl"));
            control.ApplyAPI(TfToken("RigExecControlAPI"));
            control.ApplyAPI(TfToken("NodeGraphNodeAPI"));
            control.SetAttribute(TfToken("rest:space"),VtValue(GfMatrix4d(1)));
            control.SetAttribute(TfToken("avars:rotationOrder"),VtValue(TfToken("XYZ")));
            auto prim=control.GetPrim();
            prim.GetAttribute(TfToken("default:space")).SetConnections({framePath.AppendProperty(TfToken("default:space"))});
            prim.SetDisplayName(Str(bone.data,"name"));
            prim.SetCustomDataByKey(TfToken("blender:sourceId"),VtValue(bone.id));
            UsdGeomImageable(prim).CreatePurposeAttr().Set(UsdGeomTokens->guide);
            if(!splitChannels) {
            auto spaceSwitch=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("controlSpace_"+bone.name)),TfToken("RigExecSpaceSwitch"));
            spaceSwitch.SetRelationship(TfToken("rigExec:target"),{controlPath});
            spaceSwitch.SetRelationship(TfToken("rigExec:sources"),{framePath});
            spaceSwitch.SetAttribute(TfToken("inputs:activeSpace"),VtValue(0.0));
            }
            const bool customShape=Has(bone.data,"guide_points") && !Array(Field(bone.data,"guide_points")).empty();
            const bool ownShape=!Has(bone.data,"guide_source") || Str(bone.data,"guide_source").empty() || Str(bone.data,"guide_source")==bone.id;
            control.SetAttribute(TfToken("guide:displayOpacity"),VtValue(Flag(bone.data,"visible",true) && (!customShape || !ownShape) ? 0.8f : 0.0f));
            control.SetAttribute(TfToken("guide:scaleX"),VtValue(Num(bone.data,"length",1)*0.04));
            control.SetAttribute(TfToken("guide:scaleY"),VtValue(Num(bone.data,"length",1)*0.04));
            control.SetAttribute(TfToken("guide:scaleZ"),VtValue(Num(bone.data,"length",1)*0.04));
            const auto &pose=Array(Field(bone.data,"pose"));
            const char *channels[]={"tx","ty","tz","rx","ry","rz","sx","sy","sz"};
            for(int i=0;i<9;++i) {
                const TfToken channel(std::string("avars:")+channels[i]);
                control.SetAttribute(channel,VtValue(Number(pose[i])));
                stage->GetPrimAtPath(bone.path).GetAttribute(channel).SetConnections({controlPath.AppendProperty(channel)});
            }
            stage->GetPrimAtPath(bone.path).CreateRelationship(TfToken("blender:channelControl"),true).SetTargets({controlPath});
            if(splitChannels) {
                // Publish channel bases independently: Blender inherits location
                // and rotation/scale through different parent transforms.
                neutral.SetAttribute(TfToken("inputs:spaceKind"),VtValue(TfToken("rotation")));
                const auto translationPath=framePath.AppendChild(TfToken("Translation"));
                auto translation=rigExec::RigExecSchemaPrim::Define(stage,translationPath,TfToken("RigExecControl"));
                translation.SetAttribute(TfToken("guide:displayOpacity"),VtValue(0.0f));
                AbsolutePoseParent(translation.GetPrim());
                const auto translationExprPath=neutral.GetPath().AppendChild(TfToken("Translation"));
                auto translationExpr=rigExec::RigExecSchemaPrim::Define(stage,translationExprPath,TfToken("RigExecBlenderBoneFrame"));
                for(const auto &attr:neutral.GetPrim().GetAuthoredAttributes()) {
                    VtValue value;
                    if(attr.Get(&value)) translationExpr.GetPrim().CreateAttribute(attr.GetName(),attr.GetTypeName()).Set(value);
                }
                for(const auto &rel:neutral.GetPrim().GetAuthoredRelationships()) {
                    SdfPathVector targets; rel.GetTargets(&targets);
                    translationExpr.SetRelationship(rel.GetName(),targets);
                }
                translationExpr.SetAttribute(TfToken("inputs:spaceKind"),VtValue(TfToken("translation")));
                translation.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({translationExprPath.AppendProperty(TfToken("outputs:matrix"))});
                control.SetRelationship(TfToken("rigExec:channelSpaces"),{framePath,translationPath});
                control.SetAttribute(TfToken("rigExec:translationEnabled"),VtValue(!Flag(bone.data,"connected",false)));
                // The original bone expression consumes these same nine channels.
                // Its pre-constraint pose is authoritative for the editor too.
                prim.GetAttribute(TfToken("posed:space")).SetConnections({mechanism.GetPath().AppendProperty(TfToken("outputs:matrix"))});
            }
            bone.controlPath=controlPath;
            stage->GetPrimAtPath(bone.path).GetAttribute(TfToken("guide:radius")).Set(0.0);
            if(HasActiveConstraint(bone) && customShape && ownShape)
                Warn("native control guide displays pre-constraint edit frame: "+bone.id);
        }
    }
    double GuideWidth(const Node &node) {
        const double authored=Num(node.data,"guide_wire_width",0);
        if(authored>0) return authored;
        // Older snapshots authored zero hairline widths. Reconstruct a modest
        // armature-sized diameter once per armature for native Hydra picking.
        const std::string armature=Has(node.data,"armature") ? Str(node.data,"armature") : node.id;
        auto found=guideExtents.find(armature);
        if(found==guideExtents.end()) {
            GfVec3d low(std::numeric_limits<double>::max()),high(-std::numeric_limits<double>::max());
            const auto object=nodes.find(armature);
            const GfMatrix4d inverse=object==nodes.end() ? GfMatrix4d(1) : object->second.world.GetInverse();
            bool any=false;
            for(const auto &entry:nodes) {
                const auto &bone=entry.second;
                if(bone.kind!="joint" || !Has(bone.data,"armature") || Str(bone.data,"armature")!=armature ||
                   !Flag(bone.data,"deform",true)) continue;
                const auto frame=bone.world*inverse;
                for(const auto &point:{frame.ExtractTranslation(),frame.Transform(GfVec3d(0,Num(bone.data,"length",1),0))})
                    for(int axis=0;axis<3;++axis) {low[axis]=std::min(low[axis],point[axis]);high[axis]=std::max(high[axis],point[axis]);}
                any=true;
            }
            const double extent=any ? std::max({high[0]-low[0],high[1]-low[1],high[2]-low[2]}) : Num(node.data,"length",1);
            found=guideExtents.emplace(armature,std::max(extent,1e-6)).first;
        }
        return found->second*0.001*std::max(0.5,Num(node.data,"source_wire_width_pixels",1));
    }
    void Guides() {
        for(auto &entry:nodes) {
            auto &n=entry.second;
            if(!Has(n.data,"guide_points") || Array(Field(n.data,"guide_points")).empty())continue;
            const bool ownShape=!Has(n.data,"guide_source") || Str(n.data,"guide_source").empty() || Str(n.data,"guide_source")==n.id;
            if(n.controlPath!=n.path || (n.kind=="control" && !HasActiveConstraint(n) && ownShape)) {
                auto control=rigExec::RigExecSchemaPrim::Define(stage,n.controlPath,TfToken("RigExecControl"));
                VtVec3fArray points;VtIntArray counts;size_t total=0;
                for(const auto &p:Array(Field(n.data,"guide_points")))points.push_back(Point(p));
                for(const auto &v:Array(Field(n.data,"guide_counts"))) {
                    int count=Integer(v);if(count<2)throw std::runtime_error("invalid guide polyline count");
                    counts.push_back(count);total+=count;
                }
                if(total!=points.size())throw std::runtime_error("guide topology mismatch");
                if(!ownShape) control.SetRelationship(TfToken("guide:source"),{Find(Str(n.data,"guide_source")).path});
                control.SetAttribute(TfToken("guide:shape"),VtValue(TfToken("custom")));
                control.SetAttribute(TfToken("guide:points"),VtValue(points));
                control.SetAttribute(TfToken("guide:curveVertexCounts"),VtValue(counts));
                control.SetAttribute(TfToken("guide:wireWidth"),VtValue(GuideWidth(n)));
                control.SetAttribute(TfToken("guide:displayOpacity"),VtValue(Flag(n.data,"visible",true)?1.0f:0.0f));
                control.SetAttribute(TfToken("guide:scaleX"),VtValue(1.0));
                control.SetAttribute(TfToken("guide:scaleY"),VtValue(1.0));
                control.SetAttribute(TfToken("guide:scaleZ"),VtValue(1.0));
                if(Has(n.data,"guide_color"))control.SetAttribute(TfToken("guide:displayColor"),VtValue(Point(Field(n.data,"guide_color"))));
                continue;
            }
            if(!ownShape) Warn("guide follows a different source and is not the native editable control: "+n.id);
            auto frame=[&](rigExec::RigExecSchemaPrim &target,Node &sourceNode,const std::string &suffix) {
                auto expression=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("guide_"+n.name+suffix)),TfToken("RigExecBlenderCopyTransforms"));
                expression.SetRelationship(TfToken("rigExec:source"),{sourceNode.path});
                expression.SetRelationship(TfToken("rigExec:poseInputs"),{sourceNode.path});
                AbsolutePoseParent(target.GetPrim());
                target.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({expression.GetPath().AppendProperty(TfToken("outputs:matrix"))});
            };
            auto guide=rigExec::RigExecSchemaPrim::Define(stage,n.path.AppendChild(TfToken("Display")),TfToken("RigExecControl"));
            Node &shapeSource=Has(n.data,"guide_source") && !Str(n.data,"guide_source").empty() ? Find(Str(n.data,"guide_source")) : n;
            frame(guide,shapeSource,"_display");
            UsdGeomImageable(guide.GetPrim()).CreatePurposeAttr().Set(UsdGeomTokens->guide);
            VtVec3fArray points;VtIntArray counts;size_t total=0;
            for(const auto &p:Array(Field(n.data,"guide_points")))points.push_back(Point(p));
            for(const auto &v:Array(Field(n.data,"guide_counts"))) {
                int count=Integer(v);if(count<2)throw std::runtime_error("invalid guide polyline count");
                counts.push_back(count);total+=count;
            }
            if(total!=points.size())throw std::runtime_error("guide topology mismatch");
            guide.SetAttribute(TfToken("guide:shape"),VtValue(TfToken("custom")));
            guide.SetAttribute(TfToken("guide:points"),VtValue(points));
            guide.SetAttribute(TfToken("guide:curveVertexCounts"),VtValue(counts));
            guide.SetAttribute(TfToken("guide:wireWidth"),VtValue(GuideWidth(n)));
            guide.SetAttribute(TfToken("guide:displayOpacity"),VtValue(Flag(n.data,"visible",true)?1.0f:0.0f));
            if(Has(n.data,"guide_color"))guide.SetAttribute(TfToken("guide:displayColor"),VtValue(Point(Field(n.data,"guide_color"))));
        }
    }
    bool SplineIk(Node &owner,const JsValue &c,size_t index) {
        if(owner.kind!="joint" || !Has(owner.data,"armature") || Str(c,"source").empty() ||
           Number(Field(c,"influence"))!=1 || Num(c,"chain_count",0)<2 ||
           Str(c,"y_scale_mode")!="FIT_CURVE" || Str(c,"xz_scale_mode")!="NONE" ||
           !Flag(c,"use_even_divisions",false) || Flag(c,"use_chain_offset",false))return false;
        auto &curve=Find(Str(c,"source"));
        if(!Has(curve.data,"spline_hooks") || Array(Field(curve.data,"spline_hooks")).size()!=3 ||
           !Has(curve.data,"spline_radii"))return false;
        for(const auto &radius:Array(Field(curve.data,"spline_radii")))
            if(std::abs(Number(radius)-1)>1e-6)return false;
        std::vector<Node *> hooks,chain;
        for(const auto &id:Array(Field(curve.data,"spline_hooks"))) {
            auto &bone=Find(String(id));
            if(bone.kind!="joint" || Str(bone.data,"armature")!=Str(owner.data,"armature"))return false;
            hooks.push_back(&bone);
        }
        Node *bone=&owner;
        const int count=static_cast<int>(Num(c,"chain_count",0));
        for(int i=0;i<count;++i) {
            if(bone->kind!="joint" || Str(bone->data,"armature")!=Str(owner.data,"armature"))return false;
            chain.push_back(bone);
            if(i+1<count) {
                if(bone->parent.empty())return false;
                bone=&Find(bone->parent);
            }
        }
        std::reverse(chain.begin(),chain.end());
        const auto name="spline_"+std::to_string(index);
        const auto controlScope=SdfPath("/Rig/SplineControls").AppendChild(TfToken(name));
        const auto jointScope=SdfPath("/Rig/SplineJoints").AppendChild(TfToken(name));
        stage->DefinePrim(SdfPath("/Rig/SplineControls"),TfToken("Scope"));
        stage->DefinePrim(SdfPath("/Rig/SplineJoints"),TfToken("Scope"));
        stage->DefinePrim(controlScope,TfToken("Scope"));
        stage->DefinePrim(jointScope,TfToken("Scope"));
        SdfPathVector controls,joints;
        for(int i=0;i<3;++i) {
            auto proxy=rigExec::RigExecSchemaPrim::Define(stage,controlScope.AppendChild(TfToken("Hook"+std::to_string(i))),TfToken("RigExecControl"));
            proxy.SetAttribute(TfToken("rest:space"),VtValue(hooks[i]->world));
            proxy.SetAttribute(TfToken("guide:displayOpacity"),VtValue(0.0f));
            proxy.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({hooks[i]->path.AppendProperty(TfToken("posed:space"))});
            controls.push_back(proxy.GetPath());
        }
        std::vector<GfMatrix4d> virtualRests;
        for(int i=0;i<=count;++i) {
            auto rest=chain[std::min(i,count-1)]->world;
            if(i==count)rest.SetTranslateOnly(rest.Transform(GfVec3d(0,Num(owner.data,"length",1),0)));
            GfVec3d x(rest[1][0],rest[1][1],rest[1][2]);
            GfVec3d y(-rest[0][0],-rest[0][1],-rest[0][2]);
            for(int axis=0;axis<3;++axis) {rest[0][axis]=x[axis];rest[1][axis]=y[axis];}
            auto virtualJoint=rigExec::RigExecSchemaPrim::Define(stage,jointScope.AppendChild(TfToken("Joint"+std::to_string(i))),TfToken("RigExecJoint"));
            virtualJoint.SetAttribute(TfToken("rest:space"),VtValue(rest));
            virtualJoint.SetAttribute(TfToken("guide:radius"),VtValue(0.0));
            virtualJoint.SetAttribute(TfToken("guide:displayOpacity"),VtValue(0.0f));
            virtualRests.push_back(rest);joints.push_back(virtualJoint.GetPath());
        }
        auto solver=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Solvers").AppendChild(TfToken(name)),TfToken("RigExecSplineIk"));
        solver.ApplyAPI(TfToken("NodeGraphNodeAPI"));
        // The stock solver publishes a unit-radius blue guide sphere. Its
        // three hook controls are the animator-facing guides instead.
        UsdGeomImageable(solver.GetPrim()).CreateVisibilityAttr().Set(UsdGeomTokens->invisible);
        solver.SetAttribute(TfToken("guide:radius"),VtValue(0.0));
        solver.SetAttribute(TfToken("guide:displayOpacity"),VtValue(0.0f));
        solver.SetRelationship(TfToken("rigExec:rootControl"),{controls[0]});
        solver.SetRelationship(TfToken("rigExec:midControl"),{controls[1]});
        solver.SetRelationship(TfToken("rigExec:endControl"),{controls[2]});
        solver.SetRelationship(TfToken("rigExec:joints"),joints);
        solver.SetAttribute(TfToken("inputs:preserveVolume"),VtValue(0.0));
        solver.SetAttribute(TfToken("inputs:midFollowWeight"),VtValue(0.35));
        solver.SetAttribute(TfToken("rigExec:restLength"),VtValue(TfToken("curve")));
        solver.GetPrim().SetCustomDataByKey(TfToken("blender:source"),VtValue(JsWriteToString(c)));
        for(int i=0;i<count;++i) {
            auto mapped=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken(name+"_bone_"+std::to_string(i))),TfToken("RigExecBlenderMappedFrame"));
            mapped.SetAttribute(TfToken("inputs:targetRest"),VtValue(chain[i]->world));
            mapped.SetAttribute(TfToken("inputs:sourceRest"),VtValue(virtualRests[i]));
            mapped.SetRelationship(TfToken("rigExec:source"),{joints[i]});
            mapped.SetRelationship(TfToken("rigExec:poseInputs"),{joints[i]});
            stage->GetPrimAtPath(chain[i]->path).GetAttribute(TfToken("posed:space")).SetConnections({mapped.GetPath().AppendProperty(TfToken("outputs:matrix"))});
        }
        Warn("Spline IK uses native three-control curve; Blender Bezier hook shape and twist may differ: "+owner.id);
        return true;
    }
    void Constraints() {
        const auto &items=Array(Field(scene,"constraints"));
        // Dependencies include ancestors of every source and owner. Reject
        // feedback explicitly, then apply each owner's stack in source order.
        std::map<std::string,std::vector<size_t>> stacks;
        for(size_t i=0;i<items.size();++i) {
            const auto &c=items[i]; Find(Str(c,"owner"));
            const double influence=Number(Field(c,"influence"));
            if(influence<0 || influence>1) throw std::runtime_error("constraint influence outside [0,1]");
            if(Flag(c,"enabled",true) && influence>0) stacks[Str(c,"owner")].push_back(i);
        }
        std::map<std::string,int> state;
        size_t depth=0;
        std::function<void(const std::string &)> visit;
        visit=[&](const std::string &owner) {
            if(state[owner]==2) return;
            if(state[owner]==1) throw std::runtime_error("constraint dependency cycle: "+owner);
            if(++depth>512) throw std::runtime_error("constraint dependency depth exceeds 512");
            state[owner]=1;
            const auto &node=Find(owner);
            if(!node.parent.empty()) visit(node.parent);
            for(size_t index:stacks[owner]) {
                const auto &c=items[index];
                const auto src=Str(c,"source");
                if(!src.empty()) {
                    visit(src);
                    if(Str(c,"type")=="SPLINE_IK" && Has(Find(src).data,"spline_hooks"))
                        for(const auto &hook:Array(Field(Find(src).data,"spline_hooks")))visit(String(hook));
                }
                if(Has(c,"pole")) visit(Str(c,"pole"));
                if(Has(c,"custom_space") && !Str(c,"custom_space").empty())visit(Str(c,"custom_space"));
                if(Str(c,"type")=="ARMATURE" && Has(c,"targets"))
                    for(const auto &target:Array(Field(c,"targets")))
                        if(Number(Field(target,"weight"))>0 && !Str(target,"source").empty())visit(Str(target,"source"));
            }
            bool native=NativeConstraintStack(Find(owner),stacks[owner],items);
            const bool hasIk=std::any_of(stacks[owner].begin(),stacks[owner].end(),
                [&](size_t index){return Str(items[index],"type")=="IK";});
            if(!native && !hasIk) {
                // Unimplemented Action/Transform/limit nodes must not erase
                // the supported Armature parenting in the same stack. Keep
                // the native subsequence in source order, and diagnose every
                // omitted node; this remains an incomplete conversion.
                std::vector<size_t> subset;
                for(size_t index:stacks[owner])
                    if(NativeConstraintStack(Find(owner),{index},items,true))subset.push_back(index);
                if(subset.size()!=stacks[owner].size() && NativeConstraintStack(Find(owner),subset,items)) {
                    native=true;
                    for(size_t index:stacks[owner])if(std::find(subset.begin(),subset.end(),index)==subset.end())
                        Warn("unsupported constraint omitted from native stack: "+owner+"/"+Str(items[index],"name")+" ("+Str(items[index],"type")+", "+Str(items[index],"owner_space")+"/"+Str(items[index],"target_space")+")");
                }
            }
            if(!native)for(size_t i=0;i<stacks[owner].size();++i) {
                    size_t index=stacks[owner][i]; ConvertConstraint(items[index],index,i==0);
                }
            state[owner]=2;
            --depth;
        };
        std::vector<std::string> owners;
        for(const auto &entry:stacks) owners.push_back(entry.first);
        for(const auto &owner:owners) visit(owner);
    }
    bool NativeConstraintStack(Node &owner,const std::vector<size_t> &stack,const JsArray &items,bool validateOnly=false) {
        if(owner.kind!="joint" || !Has(owner.data,"armature") || stack.empty()) return false;
        for(size_t index:stack) {
            const auto &c=items[index];auto type=Str(c,"type");
            if(!std::set<std::string>{"COPY_LOCATION","COPY_ROTATION","COPY_SCALE","COPY_TRANSFORMS","ARMATURE","STRETCH_TO","DAMPED_TRACK","LIMIT_ROTATION"}.count(type)) return false;
            bool copy=type.find("COPY_")==0;
            for(const char *space:{"owner_space","target_space"}) {
                auto value=Str(c,space);
                if(value!="WORLD" && !(copy && (value=="POSE" || value=="LOCAL" || value=="CUSTOM" ||
                    (std::string(space)=="target_space" && value=="LOCAL_OWNER_ORIENT"))))return false;
                if(value=="CUSTOM" && (!Has(c,"custom_space") || Flag(c,"custom_space_unsupported",false)))return false;
            }
            if(type=="LIMIT_ROTATION") {
                if(!Has(c,"use_limit_x") || !Has(c,"use_limit_y") || !Has(c,"use_limit_z") ||
                   Flag(c,"use_limit_x",false) || Flag(c,"use_limit_y",false) || Flag(c,"use_limit_z",false))return false;
                continue; // With no angle limits this constraint only removes shear.
            }
            if(type=="ARMATURE") {
                if(Flag(c,"use_bone_envelopes",false) || !Has(c,"targets"))return false;
                for(const auto &target:Array(Field(c,"targets"))) {
                    double weight=Number(Field(target,"weight"));
                    if(weight<0)throw std::runtime_error("negative armature target weight");
                    if(weight==0)continue;
                    if(Str(target,"source").empty())return false;
                    auto &bone=Find(Str(target,"source"));
                    if(bone.kind!="joint" || !Has(bone.data,"armature"))return false;
                }
                continue;
            }
            if(Str(c,"source").empty())return false;
            auto &src=Find(Str(c,"source"));
            if(Str(c,"target_space")!="WORLD" && Str(c,"target_space")!="CUSTOM" && (src.kind!="joint" || !Has(src.data,"armature")))return false;
            if(type=="COPY_ROTATION") {
                int count=Flag(c,"use_x",true)+Flag(c,"use_y",true)+Flag(c,"use_z",true);
                if((count!=0 && count!=3) || Flag(c,"invert_x",false) || Flag(c,"invert_y",false) || Flag(c,"invert_z",false) ||
                   (Has(c,"mix_mode") && !std::set<std::string>{"REPLACE","BEFORE","AFTER"}.count(Str(c,"mix_mode"))))return false;
            }
            if(type=="COPY_SCALE" && Num(c,"power",1)<0)throw std::runtime_error("negative Copy Scale power");
            if(Flag(c,"use_bbone_shape",false) && Num(src.data,"bbone_segments",1)>1 && Num(c,"head_tail",0)!=0) return false;
            if(type=="COPY_TRANSFORMS" && (Num(c,"head_tail",0)!=0 ||
                (Has(c,"mix_mode") && !std::set<std::string>{"REPLACE","BEFORE_FULL","AFTER_FULL","BEFORE","AFTER","BEFORE_SPLIT","AFTER_SPLIT"}.count(Str(c,"mix_mode"))))) return false;
            if(type=="STRETCH_TO") {
                if(Num(c,"rest_length",0)<=0 || !Has(c,"volume") || !Has(c,"keep_axis"))return false;
                if(!std::set<std::string>{"VOLUME_XZX","VOLUME_X","VOLUME_Z","NO_VOLUME"}.count(Str(c,"volume")) ||
                   !std::set<std::string>{"PLANE_X","PLANE_Z","SWING_Y"}.count(Str(c,"keep_axis"))) throw std::runtime_error("invalid Stretch To mode");
                if(Num(c,"bulge",1)<0 || Num(c,"bulge_min",0)<0 || Num(c,"bulge_min",0)>1 || Num(c,"bulge_max",1)<1 ||
                   Num(c,"bulge_smooth",0)<0 || Num(c,"bulge_smooth",0)>1) throw std::runtime_error("invalid Stretch To volume settings");
            }
            if(type=="DAMPED_TRACK" && (!Has(c,"track_axis") || !std::set<std::string>{"TRACK_X","TRACK_Y","TRACK_Z","TRACK_NEGATIVE_X","TRACK_NEGATIVE_Y","TRACK_NEGATIVE_Z"}.count(Str(c,"track_axis"))))
                throw std::runtime_error("invalid Damped Track axis");
        }
        if(validateOnly)return true;
        auto posed=stage->GetPrimAtPath(owner.path).GetAttribute(TfToken("posed:space"));
        SdfPathVector inputs;posed.GetConnections(&inputs);
        if(inputs.size()!=1)throw std::runtime_error("native constraint stack requires base bone frame");
        auto base=inputs[0],previous=base;
        // The incoming expression reads the owner's live parent even when
        // the constraint itself only names target frames. Declare that
        // closure, and earlier stack inputs, for usdRig's pose refresh.
        SdfPathVector stackDependencies={Find(Str(owner.data,"armature")).path};
        if(Find(owner.parent).kind=="joint")stackDependencies.push_back(Find(owner.parent).path);
        for(size_t index:stack) {
            const auto &c=items[index];auto type=Str(c,"type");
            Node *src=Str(c,"source").empty()?nullptr:&Find(Str(c,"source"));
            auto op=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("stack_"+std::to_string(index))),TfToken("RigExecBlenderConstraintFrame"));
            op.GetPrim().GetAttribute(TfToken("inputs:incoming")).SetConnections({previous});
            op.SetAttribute(TfToken("inputs:operation"),VtValue(TfToken(type)));
            op.SetAttribute(TfToken("inputs:influence"),VtValue(Number(Field(c,"influence"))));
            op.SetAttribute(TfToken("inputs:ownerSpace"),VtValue(TfToken(Str(c,"owner_space"))));
            op.SetAttribute(TfToken("inputs:targetSpace"),VtValue(TfToken(Str(c,"target_space"))));
            SdfPathVector dependencies=stackDependencies;
            if(src) {op.SetRelationship(TfToken("rigExec:source"),{src->path});dependencies.push_back(src->path);}
            if(Has(c,"custom_space") && !Str(c,"custom_space").empty()) {
                auto path=Find(Str(c,"custom_space")).path;
                op.SetRelationship(TfToken("rigExec:customSpace"),{path});dependencies.push_back(path);
            }
            auto localInputs=[&](Node &bone,const std::string &role) {
                auto &object=Find(Str(bone.data,"armature"));
                op.SetAttribute(TfToken("inputs:"+role+"Local"),VtValue(bone.rest));
                op.SetAttribute(TfToken("inputs:"+role+"Rest"),VtValue(bone.world*object.world.GetInverse()));
                op.SetAttribute(TfToken("inputs:"+role+"InheritScale"),VtValue(TfToken(Has(bone.data,"inherit_scale")?Str(bone.data,"inherit_scale"):"FULL")));
                op.SetAttribute(TfToken("inputs:"+role+"InheritRotation"),VtValue(Flag(bone.data,"inherit_rotation",true)));
                op.SetAttribute(TfToken("inputs:"+role+"LocalLocation"),VtValue(Flag(bone.data,"local_location",true)));
                auto &parent=Find(bone.parent);
                if(parent.kind=="joint") {
                    op.SetAttribute(TfToken("inputs:"+role+"HasParent"),VtValue(true));
                    op.SetAttribute(TfToken("inputs:"+role+"ParentRest"),VtValue(parent.world*object.world.GetInverse()));
                    op.SetRelationship(TfToken("rigExec:"+role+"Parent"),{parent.path});dependencies.push_back(parent.path);
                }
            };
            if(Str(c,"owner_space")!="WORLD" && Str(c,"owner_space")!="CUSTOM") {
                auto &object=Find(Str(owner.data,"armature"));
                op.SetRelationship(TfToken("rigExec:ownerObject"),{object.path});dependencies.push_back(object.path);
                if(Str(c,"owner_space")=="LOCAL")localInputs(owner,"owner");
            }
            if(Str(c,"target_space")!="WORLD" && Str(c,"target_space")!="CUSTOM") {
                auto &object=Find(Str(src->data,"armature"));
                op.SetRelationship(TfToken("rigExec:sourceObject"),{object.path});dependencies.push_back(object.path);
                if(Str(c,"target_space")!="POSE")localInputs(*src,"source");
                if(Str(c,"target_space")=="LOCAL_OWNER_ORIENT") {
                    auto &ownerObject=Find(Str(owner.data,"armature"));
                    op.SetAttribute(TfToken("inputs:ownerRest"),VtValue(owner.world*ownerObject.world.GetInverse()));
                }
            }
            if(src && src->kind=="joint")op.SetAttribute(TfToken("inputs:targetOffset"),VtValue(GfVec3d(0,Num(src->data,"length",1)*Num(c,"head_tail",0),0)));
            if(type=="ARMATURE") {
                op.SetAttribute(TfToken("inputs:operation"),VtValue(TfToken("ARMATURE_BLEND")));
                VtMatrix4dArray binds;VtDoubleArray weights;VtIntArray targetIndices,objectIndices;
                SdfPathVector targets,objects;
                auto add=[](SdfPathVector &paths,const SdfPath &path) {
                    auto it=std::find(paths.begin(),paths.end(),path);
                    if(it!=paths.end())return int(it-paths.begin());
                    paths.push_back(path);return int(paths.size()-1);
                };
                for(const auto &target:Array(Field(c,"targets"))) {
                    double weight=Number(Field(target,"weight"));if(weight==0)continue;
                    auto &bone=Find(Str(target,"source"));auto &object=Find(Str(bone.data,"armature"));
                    if(Num(bone.data,"bbone_segments",1)!=1)Warn("Armature B-Bone target uses its whole-bone frame; segment binding is not translated: "+bone.id);
                    binds.push_back((bone.world*object.world.GetInverse()).GetInverse());weights.push_back(weight);
                    targetIndices.push_back(add(targets,bone.path));objectIndices.push_back(add(objects,object.path));
                    add(dependencies,bone.path);add(dependencies,object.path);
                }
                op.SetAttribute(TfToken("inputs:targetBinds"),VtValue(binds));
                op.SetAttribute(TfToken("inputs:targetWeights"),VtValue(weights));
                op.SetAttribute(TfToken("inputs:targetIndices"),VtValue(targetIndices));
                op.SetAttribute(TfToken("inputs:objectIndices"),VtValue(objectIndices));
                op.SetAttribute(TfToken("inputs:dualQuaternion"),VtValue(Flag(c,"use_deform_preserve_volume",false)));
                op.SetAttribute(TfToken("inputs:currentPivot"),VtValue(Flag(c,"use_current_location",false)));
                auto &object=Find(Str(owner.data,"armature"));
                op.SetAttribute(TfToken("inputs:pivot"),VtValue((owner.world*object.world.GetInverse()).ExtractTranslation()));
                op.SetRelationship(TfToken("rigExec:ownerObject"),{object.path});add(dependencies,object.path);
                op.SetRelationship(TfToken("rigExec:targets"),targets);op.SetRelationship(TfToken("rigExec:targetObjects"),objects);
            }
            if(type=="COPY_LOCATION" || type=="COPY_SCALE" || type=="COPY_ROTATION") {
                int axes=0,invert=0;const char *axisNames[]={"x","y","z"};
                for(int i=0;i<3;++i) {if(Flag(c,std::string("use_")+axisNames[i],true))axes|=1<<i;if(Flag(c,std::string("invert_")+axisNames[i],false))invert|=1<<i;}
                op.SetAttribute(TfToken("inputs:axisMask"),VtValue(axes));op.SetAttribute(TfToken("inputs:invertMask"),VtValue(invert));
                op.SetAttribute(TfToken("inputs:offset"),VtValue(Flag(c,"use_offset",false)));
            }
            if(type=="COPY_TRANSFORMS") {
                op.SetAttribute(TfToken("inputs:rotationMix"),VtValue(TfToken(Has(c,"mix_mode")?Str(c,"mix_mode"):"REPLACE")));
                op.SetAttribute(TfToken("inputs:removeTargetShear"),VtValue(Flag(c,"remove_target_shear",false)));
            }
            if(type=="COPY_ROTATION")op.SetAttribute(TfToken("inputs:rotationMix"),VtValue(TfToken(Has(c,"mix_mode")?Str(c,"mix_mode"):"REPLACE")));
            if(type=="COPY_SCALE") {
                op.SetAttribute(TfToken("inputs:uniformScale"),VtValue(Flag(c,"use_make_uniform",false)));
                op.SetAttribute(TfToken("inputs:scaleAdd"),VtValue(Flag(c,"use_add",false)));
                op.SetAttribute(TfToken("inputs:power"),VtValue(Num(c,"power",1)));
            }
            if(type=="DAMPED_TRACK")op.SetAttribute(TfToken("inputs:trackAxis"),VtValue(TfToken(Str(c,"track_axis"))));
            if(type=="STRETCH_TO") {
                Warn("near-zero-length Stretch To retains its incoming frame because native providers require invertible transforms");
                op.SetAttribute(TfToken("inputs:keepAxis"),VtValue(TfToken(Str(c,"keep_axis"))));
                op.SetAttribute(TfToken("inputs:volume"),VtValue(TfToken(Str(c,"volume"))));
                for(const auto &pair:std::vector<std::pair<const char *,const char *>>{{"rest_length","restLength"},{"bulge","bulge"},{"bulge_min","bulgeMin"},{"bulge_max","bulgeMax"},{"bulge_smooth","bulgeSmooth"}})
                    if(Has(c,pair.first))op.SetAttribute(TfToken(std::string("inputs:")+pair.second),VtValue(Number(Field(c,pair.first))));
                op.SetAttribute(TfToken("inputs:useBulgeMin"),VtValue(Flag(c,"use_bulge_min",false)));
                op.SetAttribute(TfToken("inputs:useBulgeMax"),VtValue(Flag(c,"use_bulge_max",false)));
            }
            op.SetRelationship(TfToken("rigExec:poseInputs"),dependencies);
            stackDependencies=dependencies;
            previous=op.GetPath().AppendProperty(TfToken("outputs:matrix"));
        }
        if(Flag(owner.data,"connected",false)) {
            auto op=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("origin_"+owner.name)),TfToken("RigExecBlenderConstraintFrame"));
            op.SetAttribute(TfToken("inputs:operation"),VtValue(TfToken("PRESERVE_ORIGIN")));
            op.GetPrim().GetAttribute(TfToken("inputs:incoming")).SetConnections({previous});
            op.GetPrim().GetAttribute(TfToken("inputs:origin")).SetConnections({base});
            previous=op.GetPath().AppendProperty(TfToken("outputs:matrix"));
        }
        posed.SetConnections({previous});return true;
    }
    void ConvertConstraint(const JsValue &c,size_t index,bool first) {
        auto &owner=Find(Str(c,"owner"));
        const std::string type=Str(c,"type"),label=owner.id+"/"+Str(c,"name");
        const std::string sourceId=Str(c,"source");
        if(sourceId.empty()) { Warn("constraint has no source: "+label); return; }
        auto &src=Find(sourceId);
        if(type=="SPLINE_IK") {
            if(!first || !SplineIk(owner,c,index))
                Warn("Spline IK curve/hooks or solver settings are unsupported: "+label);
            return;
        }
        if(type=="ARMATURE") {
            bool oneTarget=false;
            if(Has(c,"targets")) {
                size_t active=0;
                for(const auto &target:Array(Field(c,"targets"))) {
                    double weight=Number(Field(target,"weight"));
                    if(weight<0) throw std::runtime_error("negative armature target weight");
                    if(weight>0) { ++active; oneTarget=Str(target,"source")==sourceId; }
                }
                oneTarget=oneTarget && active==1;
            }
            if(!first || !oneTarget || src.kind!="joint" || !Has(src.data,"armature") ||
               owner.kind!="joint" || Flag(c,"use_deform_preserve_volume",false) ||
               Flag(c,"use_bone_envelopes",false) || Number(Field(c,"influence"))!=1 ||
               Num(src.data,"bbone_segments",1)!=1) {
                Warn("armature constraint requires first position, one simple target, full influence and linear/no-envelope mode: "+label); return;
            }
            auto mechanism=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("armature_"+std::to_string(index))),TfToken("RigExecBlenderArmatureParent"));
            mechanism.SetAttribute(TfToken("inputs:local"),VtValue(owner.rest));
            mechanism.SetAttribute(TfToken("inputs:preserveLocation"),VtValue(Flag(owner.data,"connected",false)));
            auto posed=stage->GetPrimAtPath(owner.path).GetAttribute(TfToken("posed:space"));
            SdfPathVector incoming;posed.GetConnections(&incoming);
            if(!incoming.empty()) {
                mechanism.SetAttribute(TfToken("inputs:useIncoming"),VtValue(true));
                mechanism.GetPrim().GetAttribute(TfToken("inputs:incoming")).SetConnections(incoming);
            }
            auto &sourceObject=Find(Str(src.data,"armature"));
            mechanism.SetAttribute(TfToken("inputs:inverseBind"),VtValue((src.world*sourceObject.world.GetInverse()).GetInverse()));
            SdfPathVector inputs={src.path,sourceObject.path};
            if(!owner.parent.empty()) {
                auto parent=Find(owner.parent).path;
                mechanism.SetRelationship(TfToken("rigExec:parent"),{parent}); inputs.push_back(parent);
            }
            mechanism.SetRelationship(TfToken("rigExec:source"),{src.path});
            mechanism.SetRelationship(TfToken("rigExec:sourceObject"),{sourceObject.path});
            mechanism.SetRelationship(TfToken("rigExec:poseInputs"),inputs);
            for(const auto *channel:{"tx","ty","tz","rx","ry","rz","sx","sy","sz"})
                mechanism.GetPrim().GetAttribute(TfToken(std::string("inputs:")+channel)).SetConnections({owner.path.AppendProperty(TfToken(std::string("avars:")+channel))});
            stage->GetPrimAtPath(owner.path).GetAttribute(TfToken("posed:space")).SetConnections({mechanism.GetPath().AppendProperty(TfToken("outputs:matrix"))});
            return;
        }
        if(Str(c,"owner_space")!="WORLD" || Str(c,"target_space")!="WORLD" ||
           Flag(c,"use_offset",false) || Flag(c,"invert_x",false) || Flag(c,"invert_y",false) || Flag(c,"invert_z",false) ||
           Flag(c,"use_make_uniform",false) || Flag(c,"use_add",false) || Num(c,"power",1)!=1 ||
           Num(c,"head_tail",0)!=0 || (Has(c,"mix_mode") && Str(c,"mix_mode")!="REPLACE")) {
            Warn("constraint spaces/offset/inversion/mix/head-tail settings are unsupported: "+label); return;
        }
        const std::string name="constraint_"+std::to_string(index);
        if(type=="COPY_TRANSFORMS" && first && Number(Field(c,"influence"))==1 && !Flag(c,"remove_target_shear",false)) {
            auto mechanism=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("copy_"+std::to_string(index))),TfToken("RigExecBlenderCopyTransforms"));
            mechanism.SetRelationship(TfToken("rigExec:source"),{src.path});
            mechanism.SetRelationship(TfToken("rigExec:poseInputs"),{src.path});
            bool connected=Flag(owner.data,"connected",false);
            mechanism.SetAttribute(TfToken("inputs:preserveLocation"),VtValue(connected));
            auto posed=stage->GetPrimAtPath(owner.path).GetAttribute(TfToken("posed:space"));
            if(connected) {
                SdfPathVector inputs;posed.GetConnections(&inputs);
                if(inputs.empty()) throw std::runtime_error("connected bone requires native base frame");
                mechanism.GetPrim().GetAttribute(TfToken("inputs:incoming")).SetConnections(inputs);
            }
            posed.SetConnections({mechanism.GetPath().AppendProperty(TfToken("outputs:matrix"))});
            return;
        }
        if(type=="IK") {
            if(owner.kind!="joint" || !Flag(c,"use_tail",true) ||
               Flag(c,"use_rotation",false) || !Flag(c,"use_location",true)) {
                Warn("IK requires tail targeting, location, joint owner and no rotation: "+label); return;
            }
            const int count=Has(c,"chain_count") ? Integer(Field(c,"chain_count")) : 0;
            if(count<0) throw std::runtime_error("negative IK chain length");
            Node *first=&owner; int length=1;
            while(!first->parent.empty() && Find(first->parent).kind=="joint" && (!count || length<count)) {
                first=&Find(first->parent); ++length;
            }
            // Blender's terminal bone has its endpoint at its tail. RigExec's
            // inclusive chain ends at a joint origin, so author a virtual tip.
            const SdfPath end=owner.path.AppendChild(TfToken("IkTip_"+std::to_string(index)));
            auto tip=rigExec::RigExecSchemaPrim::Define(stage,end,TfToken("RigExecJoint"));
            tip.SetAttribute(TfToken("guide:radius"),VtValue(0.0));
            tip.SetAttribute(TfToken("guide:displayOpacity"),VtValue(0.0f));
            GfMatrix4d local(1); local.SetTranslate(GfVec3d(0,Num(owner.data,"length",1),0));
            tip.SetAttribute(TfToken("rest:space"),VtValue(local));
            std::vector<SdfPath> poles;
            if(Has(c,"pole")) poles.push_back(Find(Str(c,"pole")).path);
            auto ik=Chain(name).AddSingleChainIkConstraint("solve",first->path,end,src.path,poles);
            ik.SetOrientationMode(TfToken("preserve"));
            double stretch=0;
            if(Flag(c,"use_stretch",true)) {
                std::vector<double> values;
                for(Node *bone=&owner;;bone=&Find(bone->parent)) {
                    values.push_back(Num(bone->data,"ik_stretch",0));
                    if(bone==first)break;
                }
                stretch=*std::max_element(values.begin(),values.end());
                if(std::any_of(values.begin(),values.end(),[&](double value){return value!=stretch;}))
                    Warn("non-uniform per-bone IK stretch requires a weighted solver: "+label);
            }
            ik.GetPrim().GetAttribute(TfToken("inputs:stretch")).Set(static_cast<float>(stretch));
            ik.SetSolverMode(TfToken(poles.empty() ? "singleChain" : "rotatePlane"));
            if(!poles.empty()) ik.SetPoleVectorMode(TfToken("object"));
            // Blender measures pole angle from the root bone's X axis;
            // native rotate-plane IK measures it from the chain bend plane.
            double poleOffset=0;
            if(!poles.empty()) {
                const auto root=first->world.ExtractTranslation();
                const auto tipPosition=local.Transform(GfVec3d(0));
                const auto endPosition=owner.world.Transform(tipPosition);
                const auto axis=(endPosition-root).GetNormalized();
                auto x=first->world.TransformDir(GfVec3d(1,0,0)); x-=axis*GfDot(x,axis);
                Node *second=&owner;
                while(second!=first && second->parent!=first->id)second=&Find(second->parent);
                auto bend=second->world.ExtractTranslation()-root; bend-=axis*GfDot(bend,axis);
                if(bend.GetLength()<1e-8)bend=GfCross(axis,x);
                if(x.GetLength()>1e-8 && bend.GetLength()>1e-8)
                    poleOffset=std::atan2(GfDot(GfCross(x.GetNormalized(),bend.GetNormalized()),axis),
                                         GfDot(x.GetNormalized(),bend.GetNormalized()));
            }
            ik.SetTwistDegrees((Num(c,"pole_angle",0)+poleOffset)*180/3.14159265358979323846);
            ik.SetDefaultWeight(static_cast<float>(Number(Field(c,"influence"))));
            Warn("IK uses native RigExec solve; Blender solver/pole-angle equivalence is not established: "+label);
            return;
        }
        const bool x=Flag(c,"use_x",true),y=Flag(c,"use_y",true),z=Flag(c,"use_z",true);
        rigExec::RigExecSourceConstraintHandle handle;
        if(type=="COPY_LOCATION") {
            if(Flag(owner.data,"connected",false)) return;
            auto op=Chain(name,owner.path).AddPositionConstraint("solve"); op.SetAffectTranslation(x,y,z); handle=op;
        } else if(type=="COPY_ROTATION") {
            auto op=Chain(name,owner.path).AddParentConstraint("solve");
            op.SetAffectTranslation(false,false,false); op.SetAffectRotation(x,y,z); handle=op;
            if(!x || !y || !z) Warn("partial rotation constraint Euler semantics approximated: "+label);
        } else if(type=="COPY_SCALE") {
            auto op=Chain(name,owner.path).AddScaleConstraint("solve"); op.SetAffectScale(x,y,z); handle=op;
        } else if(type=="COPY_TRANSFORMS") {
            auto op=Chain(name,owner.path).AddParentConstraint("solve"); op.SetAffectScale(true,true,true);
            if(Flag(owner.data,"connected",false)) op.SetAffectTranslation(false,false,false);
            handle=op;
        } else { Warn("constraint type is not translated: "+label+" ("+type+")"); return; }
        handle.SetSources({src.path});
        handle.SetReadPhase(TfToken("rigExec:sources"),"final");
        handle.SetDefaultWeight(static_cast<float>(Number(Field(c,"influence"))));
        handle.GetPrim().SetCustomDataByKey(TfToken("blender:source"),VtValue(JsWriteToString(c)));
    }
    std::string Asset(const std::string &path) {
        if(path.empty()) return {};
        // Snapshots preserve their original .blend anchor when exported to a
        // different directory. Anonymous snapshots require absolute assets.
        const std::string anchor=Has(scene,"source") && !Str(scene,"source").empty() ? Str(scene,"source") : source;
        const auto id=ArGetResolver().CreateIdentifier(path,ArResolvedPath(anchor));
        if(UsdShadeUdimUtils::IsUdimIdentifier(id)) {
            auto tiles=UsdShadeUdimUtils::ResolveUdimTilePaths(id,{});
            if(tiles.empty())Warn("unresolved UDIM texture: "+id);
            for(const auto &tile:tiles)dependencies.push_back(tile.first);
            return id;
        }
        auto resolved=ArGetResolver().Resolve(id);
        const auto result=resolved.empty() ? id : resolved.GetPathString();
        dependencies.push_back(result);
        if(resolved.empty()) Warn("unresolved asset dependency: "+id);
        return result;
    }
    void Materials() {
        std::map<std::string,std::string> labels;
        for(const auto &data:Array(Field(scene,"materials"))) {
            const auto id=Str(data,"id");
            if(!labels.emplace(id,Has(data,"name") ? Str(data,"name") : id).second)
                throw std::runtime_error("duplicate material ID");
        }
        const auto names=PrimNames(labels);
        for(const auto &data:Array(Field(scene,"materials"))) {
            const auto id=Str(data,"id");
            if(id.empty() || materials.count(id)) throw std::runtime_error("duplicate/empty material ID");
            auto material=UsdShadeMaterial::Define(stage,SdfPath("/Rig/Materials").AppendChild(TfToken(names.at(id))));
            material.GetPrim().SetCustomDataByKey(TfToken("blender:materialId"),VtValue(id));
            material.GetPrim().SetDisplayName(Has(data,"name") ? Str(data,"name") : id);
            auto shader=UsdShadeShader::Define(stage,material.GetPath().AppendChild(TfToken("Surface")));
            shader.CreateIdAttr().Set(TfToken("UsdPreviewSurface"));
            const auto &color=Array(Field(data,"color"));
            if(color.size()!=4) throw std::runtime_error("material color must contain RGBA");
            shader.CreateInput(TfToken("diffuseColor"),SdfValueTypeNames->Color3f).Set(GfVec3f(Float(color[0]),Float(color[1]),Float(color[2])));
            shader.CreateInput(TfToken("opacity"),SdfValueTypeNames->Float).Set(Float(color[3]));
            shader.CreateInput(TfToken("roughness"),SdfValueTypeNames->Float).Set(Float(Field(data,"roughness")));
            shader.CreateInput(TfToken("metallic"),SdfValueTypeNames->Float).Set(Float(Field(data,"metallic")));
            material.CreateSurfaceOutput().ConnectToSource(shader.CreateOutput(TfToken("surface"),SdfValueTypeNames->Token));
            if(Has(data,"emission"))shader.CreateInput(TfToken("emissiveColor"),SdfValueTypeNames->Color3f).Set(Point(Field(data,"emission")));
            JsObject maps=Has(data,"textures")?Object(Field(data,"textures")):JsObject{};
            const auto texture=Str(data,"texture");
            if(!texture.empty())maps.emplace("diffuseColor",JsValue(JsObject{{"file",JsValue(texture)},{"uv",JsValue("")},{"colorspace",JsValue("sRGB")},{"channel",JsValue("rgb")}}));
            std::map<std::string,std::string> uvLabels;
            for(const auto &entry:maps) uvLabels[Str(entry.second,"uv")]=Str(entry.second,"uv");
            const auto uvNames=PrimNames(uvLabels);
            for(const auto &entry:maps) {
                if(!std::set<std::string>{"diffuseColor","roughness","metallic","opacity","normal","emissiveColor","occlusion"}.count(entry.first))throw std::runtime_error("unsupported preview texture input");
                const auto &tex=entry.second;const auto uv=Str(tex,"uv");
                auto reader=UsdShadeShader::Define(stage,material.GetPath().AppendChild(TfToken("UV_"+uvNames.at(uv))));
                reader.CreateIdAttr().Set(TfToken("UsdPrimvarReader_float2"));
                reader.CreateInput(TfToken("varname"),SdfValueTypeNames->Token).Set(TfToken(uv.empty()?"st":PropertyName("uv:"+uv)));
                const std::string mapName=entry.first=="diffuseColor"?"Texture":"Texture_"+entry.first;
                auto map=UsdShadeShader::Define(stage,material.GetPath().AppendChild(TfToken(mapName)));
                map.CreateIdAttr().Set(TfToken("UsdUVTexture"));
                map.CreateInput(TfToken("file"),SdfValueTypeNames->Asset).Set(SdfAssetPath(Asset(Str(tex,"file"))));
                map.CreateInput(TfToken("st"),SdfValueTypeNames->Float2).ConnectToSource(reader.CreateOutput(TfToken("result"),SdfValueTypeNames->Float2));
                map.CreateInput(TfToken("sourceColorSpace"),SdfValueTypeNames->Token).Set(TfToken(Str(tex,"colorspace")));
                map.GetInput(TfToken("file")).GetAttr().SetColorSpace(TfToken(Str(tex,"colorspace")));
                map.GetPrim().SetCustomDataByKey(TfToken("blender:source"),VtValue(JsWriteToString(tex)));
                for(const char *name:{"scale","bias"})if(Has(tex,name)) {
                    const auto &v=Array(Field(tex,name));if(v.size()!=4)throw std::runtime_error("texture scale/bias requires four values");
                    map.CreateInput(TfToken(name),SdfValueTypeNames->Float4).Set(GfVec4f(Float(v[0]),Float(v[1]),Float(v[2]),Float(v[3])));
                }
                const auto channel=Str(tex,"channel");
                bool vector=channel=="rgb";
                if(!vector && !std::set<std::string>{"r","g","b","a"}.count(channel))throw std::runtime_error("invalid texture channel");
                const auto type=entry.first=="normal"?SdfValueTypeNames->Normal3f:vector?SdfValueTypeNames->Color3f:SdfValueTypeNames->Float;
                shader.CreateInput(TfToken(entry.first),type).ConnectToSource(map.CreateOutput(TfToken(channel),vector?SdfValueTypeNames->Float3:SdfValueTypeNames->Float));
            }
            materials.emplace(id,material);
        }
    }
    SdfPath SkinProvider(Node &mesh,Node *bone,const std::string &name,bool fromBind) {
        auto provider=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken(name)),TfToken("RigExecControl"));
        provider.SetAttribute(TfToken("guide:displayOpacity"),VtValue(0.0f));
        auto expression=rigExec::RigExecSchemaPrim::Define(stage,provider.GetPath().AppendChild(TfToken("Compute")),TfToken("RigExecBlenderSkinInfluence"));
        expression.SetAttribute(TfToken("inputs:inverseMesh"),VtValue(mesh.world.GetInverse()));
        expression.SetAttribute(TfToken("inputs:fromBind"),VtValue(fromBind));
        expression.SetAttribute(TfToken("inputs:followOnly"),VtValue(!bone));
        SdfPathVector dependencies;
        if(fromBind) { expression.SetRelationship(TfToken("rigExec:owner"),{mesh.path}); dependencies.push_back(mesh.path); }
        if(bone) {
            auto &sourceObject=Find(Str(bone->data,"armature"));
            expression.SetAttribute(TfToken("inputs:inverseBind"),VtValue((bone->world*sourceObject.world.GetInverse()).GetInverse()));
            expression.SetRelationship(TfToken("rigExec:source"),{bone->path});
            expression.SetRelationship(TfToken("rigExec:sourceObject"),{sourceObject.path});
            dependencies.push_back(bone->path);dependencies.push_back(sourceObject.path);
        }
        expression.SetRelationship(TfToken("rigExec:poseInputs"),dependencies);
        provider.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({expression.GetPath().AppendProperty(TfToken("outputs:matrix"))});
        return provider.GetPath();
    }
    void Geometry() {
        std::set<std::string> ids;
        for(const auto &data:Array(Field(scene,"meshes"))) {
            const auto id=Str(data,"id"); auto &node=Find(id);
            if(!ids.insert(id).second) throw std::runtime_error("duplicate mesh ID");
            auto mesh=UsdGeomMesh::Define(stage,SdfPath("/Rig/Geometry").AppendChild(TfToken(node.name)));
            mesh.GetPrim().SetDisplayName(Str(node.data,"name"));
            mesh.GetPrim().SetCustomDataByKey(TfToken("blender:id"),VtValue(id));
            VtVec3fArray points; VtIntArray counts,indices; size_t corners=0;
            for(const auto &p:Array(Field(data,"points"))) {
                const GfVec3d point=node.world.Transform(GfVec3d(Point(p)));
                for(int axis=0;axis<3;++axis) if(!std::isfinite(point[axis]) || std::abs(point[axis])>std::numeric_limits<float>::max())
                    throw std::runtime_error("transformed point overflow");
                points.push_back(GfVec3f(point));
            }
            for(const auto &c:Array(Field(data,"counts"))) {
                const int count=Integer(c); if(count<3) throw std::runtime_error("polygon has fewer than 3 corners");
                counts.push_back(count); corners+=count;
            }
            if(corners>MaxElements) throw std::runtime_error("too many mesh corners");
            for(const auto &v:Array(Field(data,"indices"))) {
                const int index=Integer(v); if(index<0 || static_cast<size_t>(index)>=points.size()) throw std::runtime_error("mesh index out of range");
                indices.push_back(index);
            }
            if(corners!=indices.size()) throw std::runtime_error("mesh topology size mismatch");
            mesh.CreatePointsAttr().Set(points); mesh.CreateFaceVertexCountsAttr().Set(counts);
            mesh.CreateFaceVertexIndicesAttr().Set(indices); mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->catmullClark);
            if(!points.empty()) {
                GfVec3f lower=points[0],upper=points[0];
                for(const auto &point:points) for(int axis=0;axis<3;++axis) { lower[axis]=std::min(lower[axis],point[axis]); upper[axis]=std::max(upper[axis],point[axis]); }
                mesh.CreateExtentAttr().Set(VtVec3fArray{lower,upper});
            }
            if(!Flag(node.data,"visible",true)) mesh.CreateVisibilityAttr().Set(UsdGeomTokens->invisible);
            const auto &uv=Array(Field(data,"uv"));
            if(!uv.empty()) {
                if(uv.size()!=corners) throw std::runtime_error("UV corner count mismatch");
                VtVec2fArray values;
                for(const auto &value:uv) {
                    const auto &pair=Array(value); if(pair.size()!=2) throw std::runtime_error("UV must contain 2 numbers");
                    values.push_back(GfVec2f(Float(pair[0]),Float(pair[1])));
                }
                UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken("st"),SdfValueTypeNames->TexCoord2fArray,UsdGeomTokens->faceVarying).Set(values);
            }
            if(Has(data,"uv_sets"))for(const auto &entry:Object(Field(data,"uv_sets"))) {
                const auto &uvs=Array(entry.second);if(uvs.size()!=corners)throw std::runtime_error("named UV corner count mismatch");
                VtVec2fArray values;
                for(const auto &value:uvs) {const auto &pair=Array(value);if(pair.size()!=2)throw std::runtime_error("UV must contain 2 numbers");values.emplace_back(Float(pair[0]),Float(pair[1]));}
                UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken(PropertyName("uv:"+entry.first)),SdfValueTypeNames->TexCoord2fArray,UsdGeomTokens->faceVarying).Set(values);
            }
            // Smooth flags are retained in sourceData, but are not equivalent
            // to a USD mesh without Blender's split-normal evaluation.
            const auto &smooth=Array(Field(data,"smooth"));
            if(smooth.size()!=counts.size()) throw std::runtime_error("smooth flag count mismatch");
            if(std::any_of(smooth.begin(),smooth.end(),[](const JsValue &v){return Bool(v);}))
                Warn("smooth shading normals require host recomputation: "+id);
            const auto &slots=Array(Field(data,"materials"));
            const auto &faceMaterials=Array(Field(data,"material_indices"));
            if(faceMaterials.size()!=counts.size()) throw std::runtime_error("material face count mismatch");
            std::map<int,VtIntArray> assignments;
            for(size_t i=0;i<faceMaterials.size();++i) {
                int slot=Integer(faceMaterials[i]);
                if(slot<0 || (slots.empty() ? slot!=0 : static_cast<size_t>(slot)>=slots.size())) throw std::runtime_error("material slot out of range");
                if(!slots.empty()) assignments[slot].push_back(static_cast<int>(i));
            }
            for(const auto &entry:assignments) {
                const auto matId=String(slots[entry.first]); if(matId.empty()) continue;
                if(!materials.count(matId)) throw std::runtime_error("unresolved material: "+matId);
                if(entry.second.size()==counts.size()) UsdShadeMaterialBindingAPI::Apply(mesh.GetPrim()).Bind(materials.at(matId));
                else {
                    auto subset=UsdGeomSubset::CreateGeomSubset(mesh,TfToken("material_"+std::to_string(entry.first)),
                        UsdGeomTokens->face,entry.second,TfToken("materialBind"),UsdGeomTokens->nonOverlapping);
                    UsdShadeMaterialBindingAPI::Apply(subset.GetPrim()).Bind(materials.at(matId));
                }
            }
            JsArray skins;
            if(Has(data,"skin_stack")) skins=Array(Field(data,"skin_stack"));
            else if(!Field(data,"skin").IsNull()) skins.push_back(Field(data,"skin"));
            if(skins.empty()) {
                auto follow=SkinProvider(node,nullptr,"follow_"+node.name,true);
                Chain("mesh_"+node.name,mesh.GetPath().AppendProperty(TfToken("points"))).AddMatrixMover("follow",follow,{},{},TfToken("final"));
                continue;
            }
            for(size_t skinIndex=0;skinIndex<skins.size();++skinIndex) {
            const auto &skin=skins[skinIndex];
            const auto method=Str(skin,"method");
            if(method!="classicLinear" && method!="dualQuaternion") throw std::runtime_error("unsupported skinning method: "+method);
            std::vector<SdfPath> palette;
            std::vector<Node *> bones;
            bool objectSpace=true;
            for(const auto &influence:Array(Field(skin,"influences"))) {
                auto &bone=Find(String(influence));bones.push_back(&bone);
                palette.push_back(bone.path);objectSpace=objectSpace && Has(bone.data,"armature");
            }
            if(palette.empty()) { Warn("skin has no deforming bones: "+id); continue; }
            const size_t boneCount=palette.size();
            const std::string prefix="influence_"+node.name+"_"+std::to_string(skinIndex)+"_";
            const bool fromBase=Flag(skin,"use_base_input",false);
            if(objectSpace) {
                for(size_t i=0;i<bones.size();++i) palette[i]=SkinProvider(node,bones[i],prefix+std::to_string(i),skinIndex==0 || fromBase);
                palette.push_back(SkinProvider(node,nullptr,prefix+"unweighted",skinIndex==0 || fromBase));
            }
            const auto &rows=Array(Field(skin,"weights"));
            if(rows.size()!=points.size()) throw std::runtime_error("skin vertex count mismatch");
            size_t slotsPerPoint=1;
            for(const auto &row:rows) slotsPerPoint=std::max(slotsPerPoint,Array(row).size());
            if(points.size() && slotsPerPoint>MaxElements/points.size()) throw std::runtime_error("skin layout too large");
            std::vector<int> jointIndices(points.size()*slotsPerPoint,0);
            std::vector<float> weights(jointIndices.size(),0);
            for(size_t i=0;i<rows.size();++i) {
                std::set<int> seen; size_t j=0; double total=0;
                for(const auto &entry:Array(rows[i])) {
                    const auto &pair=Array(entry); if(pair.size()!=2) throw std::runtime_error("skin entry must be [index,weight]");
                    const int index=Integer(pair[0]); const float weight=Float(pair[1]);
                    if(index<0 || static_cast<size_t>(index)>=boneCount || weight<0 || !seen.insert(index).second)
                        throw std::runtime_error("invalid skin influence/weight");
                    jointIndices[i*slotsPerPoint+j]=index; weights[i*slotsPerPoint+j]=weight; total+=weight; ++j;
                }
                if(total>0) for(size_t j=0;j<slotsPerPoint;++j) weights[i*slotsPerPoint+j]=static_cast<float>(weights[i*slotsPerPoint+j]/total);
                else if(objectSpace) {jointIndices[i*slotsPerPoint]=static_cast<int>(boneCount);weights[i*slotsPerPoint]=1;}
            }
            const std::string chainName="skin_"+node.name+"_"+std::to_string(skinIndex);
            const auto target=mesh.GetPath().AppendProperty(TfToken("points"));
            auto chain=Chain(chainName,target);
            VtFloatArray mask;
            if(Has(skin,"mask")) for(const auto &v:Array(Field(skin,"mask"))) {
                float w=Float(v); if(w<0 || w>1) throw std::runtime_error("skin mask outside [0,1]");
                mask.push_back(w);
            }
            if(!mask.empty() && mask.size()!=points.size()) throw std::runtime_error("skin mask vertex count mismatch");
            if(mask.empty() && !fromBase) {
                auto op=chain.AddSkinMover("deform",palette,{},{},TfToken("final"));
                op.SetJointInfluences(jointIndices,weights,static_cast<int>(slotsPerPoint));
                op.SetSkinningMethod(TfToken(Str(skin,"method")));
            } else {
                auto op=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Movers").AppendChild(TfToken(chainName)).AppendChild(TfToken("deform")),TfToken("RigExecBlenderArmatureMover"));
                op.ApplyAPI(TfToken("RigExecMoverAPI"));
                op.SetRelationship(TfToken("rigExec:moves"),{target});
                op.SetRelationship(TfToken("rigExec:influences"),SdfPathVector(palette.begin(),palette.end()));
                op.SetReadPhase(TfToken("rigExec:influences"),"final");
                op.SetAttribute(TfToken("rigExec:jointIndices"),VtValue(VtIntArray(jointIndices.begin(),jointIndices.end())));
                op.SetAttribute(TfToken("rigExec:jointWeights"),VtValue(VtFloatArray(weights.begin(),weights.end())));
                op.SetAttribute(TfToken("rigExec:elementSize"),VtValue(static_cast<int>(slotsPerPoint)));
                op.SetAttribute(TfToken("rigExec:skinningMethod"),VtValue(TfToken(Str(skin,"method"))));
                op.SetAttribute(TfToken("inputs:mask"),VtValue(mask));
                op.SetAttribute(TfToken("inputs:useBaseInput"),VtValue(fromBase));
                if(objectSpace && skinIndex==0) {
                    op.SetAttribute(TfToken("inputs:transformInput"),VtValue(true));
                    op.SetRelationship(TfToken("rigExec:transform"),{palette.back()});
                    op.SetReadPhase(TfToken("rigExec:transform"),"final");
                }
            }
            }
        }
    }
    void Pickers() {
        if(!Has(scene,"pickers") || Array(Field(scene,"pickers")).empty())return;
        stage->DefinePrim(SdfPath("/Rig/Pickers"),TfToken("Scope"));
        std::set<std::string> owners;
        int pickerOrder=0;
        for(const auto &data:Array(Field(scene,"pickers"))) {
            auto &owner=Find(Str(data,"owner"));
            if(!owners.insert(owner.id).second)throw std::runtime_error("duplicate picker owner");
            auto picker=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Pickers").AppendChild(TfToken(owner.name)),TfToken("RigExecPicker"));
            picker.SetAttribute(TfToken("ui:label"),VtValue(Str(data,"name")));
            picker.SetAttribute(TfToken("ui:order"),VtValue(pickerOrder++));
            picker.SetRelationship(TfToken("rigExec:picker:rig"),{SdfPath("/Rig")});
            picker.GetPrim().SetCustomDataByKey(TfToken("blender:sourceKind"),VtValue(Str(data,"source_kind")));
            picker.GetPrim().SetCustomDataByKey(TfToken("blender:source"),VtValue(JsWriteToString(data)));
            if(Has(data,"source_ui") && !Str(data,"source_ui").empty())
                Warn("picker source UI script is retained as metadata and not executed: "+owner.id);
            int pageOrder=0;
            for(const auto &pageData:Array(Field(data,"pages"))) {
                auto panel=rigExec::RigExecSchemaPrim::Define(stage,picker.GetPath().AppendChild(TfToken("page_"+std::to_string(pageOrder))),TfToken("RigExecPickerPanel"));
                panel.SetAttribute(TfToken("ui:label"),VtValue(Str(pageData,"name")));
                panel.SetAttribute(TfToken("ui:order"),VtValue(pageOrder++));
                panel.GetPrim().SetCustomDataByKey(TfToken("blender:source"),VtValue(JsWriteToString(pageData)));
                const auto &buttons=Array(Field(pageData,"buttons"));
                panel.SetAttribute(TfToken("ui:size"),VtValue(GfVec2f(528,std::max(90.0f,30.0f*float((buttons.size()+2)/3)))));
                int index=0;
                for(const auto &buttonData:buttons) {
                    auto button=rigExec::RigExecSchemaPrim::Define(stage,panel.GetPath().AppendChild(TfToken("button_"+std::to_string(index))),TfToken("RigExecPickerButton"));
                    button.SetAttribute(TfToken("ui:position"),VtValue(GfVec2f(176*(index%3),30*(index/3))));
                    button.SetAttribute(TfToken("ui:size"),VtValue(GfVec2f(168,24)));
                    button.SetAttribute(TfToken("ui:shape"),VtValue(TfToken("roundedRectangle")));
                    button.SetAttribute(TfToken("ui:roundness"),VtValue(0.15f));
                    button.SetAttribute(TfToken("ui:fill"),VtValue(GfVec4f(0.18f,0.32f,0.48f,1)));
                    button.SetAttribute(TfToken("ui:textColor"),VtValue(GfVec4f(1)));
                    button.SetAttribute(TfToken("ui:text"),VtValue(Str(buttonData,"label")));
                    button.SetAttribute(TfToken("ui:fontSize"),VtValue(10.0f));
                    const bool sourceBinding=Has(buttonData,"source") && Has(Field(buttonData,"source"),"binding");
                    if(sourceBinding)
                        Warn("picker binding is selection-only; settings and operators are not translated: "+Str(buttonData,"label"));
                    SdfPathVector targets;
                    std::set<std::string> unique;
                    bool unavailable=false;
                    for(const auto &id:Array(Field(buttonData,"controls"))) {
                        auto &control=Find(String(id));
                        if(!unique.insert(control.id).second)throw std::runtime_error("duplicate picker control target");
                        if((control.kind=="joint" && control.controlPath==control.path &&
                            (Has(control.data,"armature") || HasActiveConstraint(control) ||
                             stage->GetPrimAtPath(control.path).GetAttribute(TfToken("posed:space")).HasAuthoredConnections())) ||
                           (control.kind=="control" && HasActiveConstraint(control))) {
                            Warn("picker target is read-only in standard usdRig interaction: "+control.id);
                            unavailable=true;
                            continue;
                        }
                        targets.push_back(control.controlPath);
                    }
                    unavailable=unavailable || (sourceBinding && targets.empty());
                    if(unavailable) {
                        targets.clear();
                        button.SetAttribute(TfToken("ui:text"),VtValue(Str(buttonData,"label")+" (unavailable)"));
                        button.SetAttribute(TfToken("ui:fill"),VtValue(GfVec4f(0.15f,0.15f,0.15f,1)));
                        button.SetAttribute(TfToken("ui:textColor"),VtValue(GfVec4f(0.5f,0.5f,0.5f,1)));
                        button.GetPrim().SetCustomDataByKey(TfToken("blender:unavailable"),VtValue(true));
                    }
                    else if(sourceBinding && !targets.empty())
                        button.SetAttribute(TfToken("ui:text"),VtValue("Select "+Str(buttonData,"label")));
                    if(!targets.empty())button.SetRelationship(TfToken("rigExec:picker:controls"),targets);
                    button.GetPrim().SetCustomDataByKey(TfToken("blender:source"),VtValue(JsWriteToString(buttonData)));
                    ++index;
                }
            }
        }
    }
    void CalibrateInertLegChains() {
        struct Candidate { Node *owner; Node *source; std::string side; };
        std::map<std::string,std::vector<const JsValue *>> stacks;
        for(const auto &constraint:Array(Field(scene,"constraints")))
            if(Flag(constraint,"enabled",true) && Number(Field(constraint,"influence"))>0)
                stacks[Str(constraint,"owner")].push_back(&constraint);
        std::vector<Candidate> candidates;
        for(const auto &entry:stacks) {
            auto &owner=Find(entry.first);
            if(owner.kind!="joint" || !Flag(owner.data,"connected",false))continue;
            const auto name=Str(owner.data,"name");
            const std::string side=name.find(".L")!=std::string::npos ? "L" :
                name.find(".R")!=std::string::npos ? "R" : "";
            if(side.empty() || (name.find("thigh")==std::string::npos &&
                name.find("shin")==std::string::npos && name.find("foot")==std::string::npos &&
                name.find("toe")==std::string::npos))continue;
            const auto &first=*entry.second.front();
            if(Str(first,"type")!="COPY_TRANSFORMS" || Number(Field(first,"influence"))!=1 ||
               Str(first,"owner_space")!="WORLD" || Str(first,"target_space")!="WORLD")continue;
            bool onlyCopies=std::all_of(entry.second.begin(),entry.second.end(),[](const JsValue *c){return Str(*c,"type")=="COPY_TRANSFORMS";});
            if(!std::all_of(entry.second.begin(),entry.second.end(),[](const JsValue *c){
                    const auto type=Str(*c,"type");return type=="COPY_TRANSFORMS" || type=="STRETCH_TO";
                }))continue;
            auto &source=Find(Str(onlyCopies ? *entry.second.back() : first,"source"));
            if(source.kind=="joint")candidates.push_back({&owner,&source,side});
        }
        if(candidates.empty())return;
        rigExec::RigExecRigEvaluator evaluator(stage,SdfPath("/Rig"));
        std::vector<std::string> errors;
        if(!evaluator.Compile(&errors)) {
            Warn("connected lower-limb calibration skipped: "+(errors.empty()?std::string("native evaluation failed"):errors.front()));
            return;
        }
        const auto baseline=evaluator.Evaluate(UsdTimeCode::Default());
        if(!baseline.valid) { Warn("connected lower-limb calibration skipped: invalid saved pose");return; }
        auto matrix=[](const rigExec::RigExecPointFrame &frame) {
            GfMatrix4d result(1);const auto origin=frame.Origin();
            const GfVec3d axes[3]={frame.X()-origin,frame.Y()-origin,frame.Z()-origin};
            for(int i=0;i<3;++i)for(int j=0;j<3;++j)result[i][j]=axes[i][j];
            result.SetTranslateOnly(origin);return result;
        };
        auto frame=[&](const rigExec::RigExecRigPose &pose,const Node &node)->const rigExec::RigExecPointFrame * {
            auto it=pose.jointFramesFinal.find(node.path);
            return it==pose.jointFramesFinal.end()?nullptr:&it->second;
        };
        std::set<std::pair<std::string,std::string>> inertSides;
        for(const auto &candidate:candidates) {
            if(Str(candidate.owner->data,"name")!="ORG-shin."+candidate.side)continue;
            const auto armature=Str(candidate.owner->data,"armature");
            auto control=std::find_if(nodes.begin(),nodes.end(),[&](const auto &entry) {
                const auto &node=entry.second;
                return node.kind=="joint" && Has(node.data,"armature") &&
                    Str(node.data,"armature")==armature && Str(node.data,"name")=="foot_ik."+candidate.side;
            });
            if(control==nodes.end())continue;
            auto attr=stage->GetPrimAtPath(control->second.controlPath).GetAttribute(TfToken("avars:tx"));
            double saved=0;
            if(!attr || !attr.Get(&saved))continue;
            stage->SetEditTarget(stage->GetSessionLayer());
            attr.Set(saved+0.12);
            const auto moved=evaluator.Evaluate(UsdTimeCode::Default());
            stage->GetSessionLayer()->Clear();
            stage->SetEditTarget(stage->GetRootLayer());
            if(!moved.valid)continue;
            const auto *owner0=frame(baseline,*candidate.owner),*owner1=frame(moved,*candidate.owner);
            const auto *source0=frame(baseline,*candidate.source),*source1=frame(moved,*candidate.source);
            if(!owner0 || !owner1 || !source0 || !source1)continue;
            auto distance=[](const GfMatrix4d &a,const GfMatrix4d &b) {
                double result=0;for(int i=0;i<4;++i)for(int j=0;j<4;++j)
                    result=std::max(result,std::abs(a[i][j]-b[i][j]));return result;
            };
            if(distance(matrix(*owner0),matrix(*owner1))<1e-5 &&
               distance(matrix(*source0),matrix(*source1))>1e-4)
                inertSides.insert({armature,candidate.side});
        }
        for(const auto &candidate:candidates) {
            if(!inertSides.count({Str(candidate.owner->data,"armature"),candidate.side}))continue;
            const auto *target=frame(baseline,*candidate.owner),*source=frame(baseline,*candidate.source);
            if(!target || !source)continue;
            auto mapped=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(
                TfToken("calibrated_leg_"+candidate.owner->name)),TfToken("RigExecBlenderMappedFrame"));
            mapped.SetAttribute(TfToken("inputs:targetRest"),VtValue(matrix(*target)));
            mapped.SetAttribute(TfToken("inputs:sourceRest"),VtValue(matrix(*source)));
            mapped.SetRelationship(TfToken("rigExec:source"),{candidate.source->path});
            mapped.SetRelationship(TfToken("rigExec:poseInputs"),{candidate.source->path});
            stage->GetPrimAtPath(candidate.owner->path).GetAttribute(TfToken("posed:space")).SetConnections({
                mapped.GetPath().AppendProperty(TfToken("outputs:matrix"))});
        }
        if(!inertSides.empty())
            Warn("connected lower-limb IK chains use calibrated native frames; Stretch To length/volume remains approximate");
    }
    SdfLayerRefPtr Run(bool metadataOnly,bool strict) {
        if(Str(scene,"format")!="usdBlenderRig" || Integer(Field(scene,"version"))!=1)
            throw std::runtime_error("unsupported Blender rig snapshot format/version");
        const double units=Number(Field(scene,"meters_per_unit")),fps=Number(Field(scene,"fps"));
        if(units<=0 || fps<=0) throw std::runtime_error("invalid units/frame rate");
        UsdGeomSetStageUpAxis(stage,UsdGeomTokens->z); UsdGeomSetStageMetersPerUnit(stage,units);
        stage->SetTimeCodesPerSecond(fps); stage->SetFramesPerSecond(fps);
        stage->SetStartTimeCode(Number(Field(scene,"start"))); stage->SetEndTimeCode(Number(Field(scene,"end")));
        for(const auto &v:Array(Field(scene,"diagnostics"))) Warn(String(v));
        for(const auto &v:Array(Field(scene,"dependencies"))) Asset(String(v));
        if(!metadataOnly) { Graph(); Constraints(); Guides(); Materials(); Geometry(); Pickers(); }
        if(!operationOrder.empty()) stage->GetPrimAtPath(SdfPath("/Rig/Movers")).SetChildrenReorder(operationOrder);
        auto root=stage->GetPrimAtPath(SdfPath("/Rig"));
        root.SetCustomDataByKey(TfToken("rigExec:connectedPoseSeedReuse"),VtValue(true));
        stage->SetDefaultPrim(root);
        if(!metadataOnly) CalibrateInertLegChains();
        std::sort(diagnostics.begin(),diagnostics.end()); diagnostics.erase(std::unique(diagnostics.begin(),diagnostics.end()),diagnostics.end());
        std::sort(dependencies.begin(),dependencies.end()); dependencies.erase(std::unique(dependencies.begin(),dependencies.end()),dependencies.end());
        if(strict && !diagnostics.empty()) throw std::runtime_error("strict import rejected: "+diagnostics.front());
        root.CreateAttribute(TfToken("blender:diagnostics"),SdfValueTypeNames->StringArray,true,SdfVariabilityUniform)
            .Set(VtStringArray(diagnostics.begin(),diagnostics.end()));
        root.SetCustomDataByKey(TfToken("blender:sourceData"),VtValue(JsWriteToString(scene)));
        root.SetCustomDataByKey(TfToken("rigExec:connectedPoseSeedReuse"),VtValue(true));
        stage->SetDefaultPrim(root);
        auto layer=stage->GetRootLayer();
        layer->SetCustomLayerData({{"blenderRig:source",VtValue(source)},
            {"blenderRig:diagnostics",VtValue(VtStringArray(diagnostics.begin(),diagnostics.end()))},
            {"blenderRig:dependencies",VtValue(VtStringArray(dependencies.begin(),dependencies.end()))},
            {"blenderRig:complete",VtValue(!metadataOnly && diagnostics.empty())}});
        return layer;
    }
};
}
SdfLayerRefPtr Translate(const std::string &json,const std::string &source,bool metadataOnly,bool strict) {
    if(json.size()>512*1024*1024) throw std::runtime_error("snapshot exceeds 512 MiB");
    JsParseError error; auto scene=JsParseString(json,&error);
    if(!scene.IsObject()) throw std::runtime_error("invalid snapshot JSON at line "+std::to_string(error.line)+": "+error.reason);
    return Importer(std::move(scene),source).Run(metadataOnly,strict);
}
}
